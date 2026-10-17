#define ARDUINOJSON_USE_LONG_LONG 1 // 64-bit epoch-ms timestamps
#include <Arduino.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <cstdlib>
#include <cstring>

/*
  ===========================
  Name ↔ Hardware cheat sheet
  ===========================

  USB (debug only):
    - Code name: `Serial`
    - Physical: Swan micro-USB port (CDC/USB serial)

  HM-10 BLE UART (wireless to the PWA via Web Bluetooth):
    - Code name: `bleUart` (UART on pins A0/A3)
    - Notecarrier-F silkscreen pins:
        F_A0 = MCU RX  <- HM-10 TXD
        F_A3 = MCU TX  -> HM-10 RXD
        3V3            -> HM-10 VCC
        GND            -> HM-10 GND
    - GATT (transparent UART):
        Service        0xFFE0
        Characteristic 0xFFE1 (write without response + notify)
    - BLE notifications are split into ~20-byte chunks, so the PWA must buffer
      incoming bytes until it sees '\n' before parsing a message.

  HM-10 behaviour this firmware relies on:
    - While NO phone is connected, everything the MCU writes to the HM-10 is read as an AT command.
      While a phone IS connected, everything is passed through to the phone (and "AT" from the MCU
      drops the link). So AT setup happens once at boot, and protocol messages are only sent while
      the link is up.
    - At boot the firmware sets (only if different, both persist in the HM-10's flash):
        AT+NAME<kBleNamePrefix><last 4 hex of MCU ID>   -> advertised name, e.g. "MS-BIN-1A2B"
        AT+NOTI1                                         -> HM-10 reports "OK+CONN" / "OK+LOST"
      and then AT+RESET so the new name is advertised.
    - "OK+CONN" / "OK+LOST" arrive on the UART with no line ending, mixed into the byte stream, so
      the line reader strips them out and uses them to track the link.
    - Genuine HM-10 (firmware V5xx+) wants AT commands with NO line ending; clones (CC41-A,
      MLT-BT05, ...) want "\r\n". The module may also have been set to another baud (AT+BAUDn).
      At boot the firmware tries every baud in `kBleBaudCandidates` with both line endings and keeps
      the first combination the module answers "OK" to.
    - If the module never answers, or doesn't support AT+NOTI, the firmware can't tell when a phone
      is connected, so it sends regardless (like a plain serial bridge).

  Important:
    - Do NOT connect a USB-Serial adapter to F_A0/F_A3 at the same time as the HM-10.

  ===================
  BLE protocol (JSON)
  ===================

  Follows the MS Connect app protocol (lib/bluetooth/commands/README.md in the PWA).
  Every message is one JSON object terminated by "\r\n" (device -> PWA) or '\n' (PWA -> device).

  Device -> PWA: always the envelope {"deviceId":"<serial>","timestamp":<epoch ms>,"data":{...}}
    Periodic sample (only while a phone is connected), or reply to `read_now`:
      "data":{"material_height":<adc>}
    Reply to `get_config`, `set_device_id`, `set_sample_period_ms`, `reset_config`:
      "data":{"time_synced":<bool>,"sample_period_ms":<ms>}
    Reply to `set_time`:
      "data":{"time_synced":true}
    Rejected command:
      "data":{"error":"<why>","cmd":"<command name, if recognised>"}

    `timestamp` is epoch milliseconds. The device has no clock source, so it stays 0 (the PWA then
    uses the arrival time) until the PWA sends `set_time`.

  PWA -> device:
    Actions, {"cmd":"<name>"}:
      {"cmd":"read_now"}                  -> send one sample immediately
      {"cmd":"get_config"}                -> report config
      {"cmd":"reset_config"}              -> clear stored device serial number
    Settings, {"<name>":"<value>"} (value always a string):
      {"set_time":"1759400000000"}        -> sync clock (epoch ms)
      {"set_device_id":"SN-001"}          -> store device serial number in EEPROM
      {"set_sample_period_ms":"10000"}    -> store sample interval in EEPROM (1000-3600000 ms)

  ==================
  Naming conventions
  ==================

  - `kSomething`: a compile-time constant (the "k" prefix is a common C++ convention for constants).
    Example: `kDefaultSamplePeriodMs` is the sample interval used until one is set over BLE.

  - Units are part of names when it helps avoid mistakes:
      `Ms` = milliseconds, so `kDefaultSamplePeriodMs` is measured in milliseconds.

  - `gSomething`: a runtime global variable.
    Example: `gSamplePeriodMs` is the current interval between samples.

  - `lastSomethingMs`: a runtime variable that stores the last time an event happened (in ms from `millis()`).
    Example: `lastSampleMs` is updated each time we take/send a sample, so we can send again after
    `gSamplePeriodMs` elapses without blocking the loop.
*/

namespace
{

  // ----------------------------
  // Debug logging
  // ----------------------------
  constexpr bool kDebugLogEnabled = true;

  template <typename T>
  inline void dbgPrint(const T &v)
  {
    if (kDebugLogEnabled)
    {
      Serial.print(v);
    }
  }

  template <typename T>
  inline void dbgPrintln(const T &v)
  {
    if (kDebugLogEnabled)
    {
      Serial.println(v);
    }
  }

  inline void dbgPrintln()
  {
    if (kDebugLogEnabled)
    {
      Serial.println();
    }
  }

  // ----------------------------
  // HM-10 BLE UART (Notecarrier-F `F_A0`/`F_A3`)
  // ----------------------------
  constexpr uint8_t kBleUartRxPin = A0; // MCU RX  <- HM-10 TXD
  constexpr uint8_t kBleUartTxPin = A3; // MCU TX  -> HM-10 RXD
  // HM-10 AT+BAUD0..4. The first is the factory default and is used if the module never answers.
  constexpr unsigned long kBleBaudCandidates[] = {9600, 19200, 38400, 57600, 115200};
  // Genuine HM-10 first, then clones.
  constexpr const char *kAtLineEndings[] = {"", "\r\n"};
  // LPUART1. Core 3.x (framework 4.30000) made HardwareSerial abstract and added `Uart`, which takes
  // PinNames; the pinned core 2.x (see platformio.ini) only has HardwareSerial.
#if STM32_CORE_VERSION_MAJOR >= 3
  Uart bleUart(digitalPinToPinName(kBleUartRxPin), digitalPinToPinName(kBleUartTxPin));
#else
  HardwareSerial bleUart(kBleUartRxPin, kBleUartTxPin);
#endif

  // Advertised name is this prefix + the last 4 hex digits of the MCU unique ID, so several bins can be
  // told apart in the browser's chooser. The PWA filters on this prefix. HM-10 names are max 12 chars.
  constexpr const char *kBleNamePrefix = "MS-BIN-";
  constexpr unsigned long kAtResponseTimeoutMs = 500;
  // The HM-10 sends its reply in one burst; this much silence after the first byte ends it.
  constexpr unsigned long kAtResponseQuietMs = 100;
  constexpr uint8_t kAtRetries = 2; // per baud/line-ending combination

  constexpr unsigned long kHsiStartTimeoutMs = 10;

  // Found at boot by findHm10().
  unsigned long gBleBaud = kBleBaudCandidates[0];
  const char *gAtLineEnding = kAtLineEndings[0];

  // Link notifications the HM-10 injects into the UART stream (AT+NOTI1).
  constexpr const char *kHm10Connected = "OK+CONN";
  constexpr const char *kHm10Lost = "OK+LOST";

  // ----------------------------
  // Sensor input
  // ----------------------------
  constexpr uint8_t kAdcPin = PA1;
  constexpr uint8_t kAdcAverageSamples = 10; // averaged per reading to filter noise
  // Set to true to send a sweeping 0-4095 value instead of the ADC, to test the PWA without a sensor.
  constexpr bool kSimulateSensor = false;

  // ----------------------------
  // Debug/Dev flags
  // ----------------------------
  // Set to 1 to clear EEPROM on every boot (useful for testing/reflashing)
  // Set to 0 for normal operation (preserves stored config across power cycles)
  constexpr bool kClearEepromOnBoot = 0;
  // How often a link/traffic summary is printed on USB.
  constexpr unsigned long kStatusLogPeriodMs = 10000;

  // ----------------------------
  // EEPROM config (Flash storage)
  // ----------------------------
  // Layout (kept at the same address as the previous firmware so existing devices keep their serial):
  //   [129:129]   = 0xBB if serial number is configured, else 0x00
  //   [130:257]   = Serial Number string (null-terminated, max 127 chars)
  //   [258:258]   = 0xBC if sample period is configured, else default
  //   [259:262]   = Sample period in ms (uint32, little-endian)
  constexpr uint16_t kEepromSerialNumFlagAddr = 129;
  constexpr uint16_t kEepromSerialNumAddr = 130;
  constexpr uint16_t kMaxSerialNumLength = 127;
  constexpr uint8_t kSerialNumConfiguredFlag = 0xBB;
  constexpr uint16_t kEepromSamplePeriodFlagAddr = 258;
  constexpr uint16_t kEepromSamplePeriodAddr = 259;
  constexpr uint8_t kSamplePeriodConfiguredFlag = 0xBC;

  // Longest command line accepted from the PWA; longer lines are dropped.
  constexpr size_t kMaxBleLineLength = 256;
  // At 9600 baud a full-length line takes ~270 ms to go out.
  constexpr unsigned long kBleWriteTimeoutMs = 500;
  String gBleLine;
  bool gBleLineOverflow = false;

  // Whether a phone is connected to the HM-10. Set by "OK+CONN"/"OK+LOST", and also by any command
  // from the PWA, so a clone without AT+NOTI still works once the PWA has sent something.
  bool gLinkUp = false;
  // False when the HM-10 can't report "OK+CONN"/"OK+LOST"; then messages are sent whatever the link.
  bool gTrackLink = false;

  // Traffic counters for the USB status line.
  unsigned long gRxBytes = 0; // raw bytes from the HM-10; bytes but no lines points at a baud mismatch
  unsigned long gRxLines = 0;
  unsigned long gTxLines = 0;
  unsigned long gRxErrors = 0;
  unsigned long lastStatusLogMs = 0;

  // Runtime serial number (loaded from EEPROM, set via BLE, or defaulted to the MCU unique ID)
  char gSerialNumber[kMaxSerialNumLength + 1] = {0};

  // ----------------------------
  // Timing
  // ----------------------------
  constexpr unsigned long kDefaultSamplePeriodMs = 5000; // 5 seconds (demo)
  constexpr unsigned long kMinSamplePeriodMs = 1000;     // 1 second
  constexpr unsigned long kMaxSamplePeriodMs = 3600000;  // 1 hour
  unsigned long gSamplePeriodMs = kDefaultSamplePeriodMs;
  unsigned long lastSampleMs = 0;

  // Clock: epoch ms supplied by the PWA (`set_time`) + millis() elapsed since then.
  bool gTimeSynced = false;
  uint64_t gEpochMsAtSync = 0;
  unsigned long gMillisAtSync = 0;

  uint64_t currentEpochMs()
  {
    if (!gTimeSynced)
    {
      return 0;
    }
    return gEpochMsAtSync + static_cast<uint32_t>(millis() - gMillisAtSync);
  }

  // ----------------------------
  // Serial Number EEPROM functions
  // ----------------------------
  /**
   * Device IDs are limited to printable ASCII (space to '~'). Anything else, e.g. leftovers from other
   * firmware in this EEPROM area, would put control characters or invalid UTF-8 into the JSON, and the
   * PWA drops every message as "Not valid JSON".
   */
  bool isValidSerialNumber(const char *sn)
  {
    if (sn == nullptr || sn[0] == '\0')
    {
      return false;
    }
    size_t len = 0;
    for (; sn[len] != '\0'; len++)
    {
      if (len >= kMaxSerialNumLength || sn[len] < 0x20 || sn[len] > 0x7E)
      {
        return false;
      }
    }
    return true;
  }

  /**
   * Load serial number from EEPROM.
   * Returns true if a valid serial number exists, false if not configured.
   */
  bool loadSerialNumberFromEeprom()
  {
    uint8_t flag = EEPROM.read(kEepromSerialNumFlagAddr);
    if (flag != kSerialNumConfiguredFlag)
    {
      dbgPrintln("-- No stored SerialNumber --");
      return false;
    }

    // Read null-terminated string from EEPROM
    uint16_t addr = kEepromSerialNumAddr;
    uint16_t len = 0;
    while (len < kMaxSerialNumLength)
    {
      char c = EEPROM.read(addr + len);
      gSerialNumber[len] = c;
      if (c == '\0')
      {
        if (!isValidSerialNumber(gSerialNumber))
        {
          dbgPrintln("ERROR: SerialNumber in EEPROM is empty or not printable ASCII; ignoring it");
          memset(gSerialNumber, 0, sizeof(gSerialNumber));
          return false;
        }
        dbgPrint("Loaded SerialNumber from EEPROM: ");
        dbgPrintln(gSerialNumber);
        return true;
      }
      len++;
    }

    dbgPrintln("ERROR: SerialNumber in EEPROM is not null-terminated; ignoring it");
    memset(gSerialNumber, 0, sizeof(gSerialNumber));
    return false;
  }

  /**
   * Save serial number to EEPROM.
   */
  bool saveSerialNumberToEeprom(const char *sn)
  {
    if (!isValidSerialNumber(sn))
    {
      dbgPrint("ERROR: SerialNumber must be 1-");
      dbgPrint(kMaxSerialNumLength);
      dbgPrintln(" printable ASCII characters");
      return false;
    }

    const size_t len = strlen(sn);

    // Write serial + null, then clear tail (avoid stale chars when new SN is shorter)
    for (size_t i = 0; i <= len; i++)
    {
      EEPROM.write(kEepromSerialNumAddr + i, sn[i]);
    }
    for (size_t i = len + 1; i <= kMaxSerialNumLength; i++)
    {
      EEPROM.write(kEepromSerialNumAddr + i, 0);
    }

    // Write configured flag
    EEPROM.write(kEepromSerialNumFlagAddr, kSerialNumConfiguredFlag);

    strcpy(gSerialNumber, sn);
    dbgPrint("Saved SerialNumber to EEPROM: ");
    dbgPrintln(gSerialNumber);
    return true;
  }

  /**
   * Clear stored serial number from EEPROM.
   */
  void clearSerialNumberEeprom()
  {
    EEPROM.write(kEepromSerialNumFlagAddr, 0x00);
    memset(gSerialNumber, 0, sizeof(gSerialNumber));
    dbgPrintln("Cleared SerialNumber from EEPROM");
  }

  // ----------------------------
  // Sample period EEPROM functions
  // ----------------------------
  bool isValidSamplePeriod(unsigned long periodMs)
  {
    return periodMs >= kMinSamplePeriodMs && periodMs <= kMaxSamplePeriodMs;
  }

  /** Loads the stored sample period, keeping the default if none (or an invalid one) is stored. */
  void loadSamplePeriodFromEeprom()
  {
    if (EEPROM.read(kEepromSamplePeriodFlagAddr) != kSamplePeriodConfiguredFlag)
    {
      dbgPrintln("-- No stored sample period, using default --");
      return;
    }

    uint32_t periodMs = 0;
    for (uint8_t i = 0; i < 4; i++)
    {
      periodMs |= static_cast<uint32_t>(EEPROM.read(kEepromSamplePeriodAddr + i)) << (8 * i);
    }
    if (!isValidSamplePeriod(periodMs))
    {
      dbgPrintln("ERROR: Stored sample period out of range, using default");
      return;
    }

    gSamplePeriodMs = periodMs;
    dbgPrint("Loaded sample period from EEPROM (ms): ");
    dbgPrintln(gSamplePeriodMs);
  }

  bool saveSamplePeriodToEeprom(unsigned long periodMs)
  {
    if (!isValidSamplePeriod(periodMs))
    {
      dbgPrintln("ERROR: Sample period out of range");
      return false;
    }

    for (uint8_t i = 0; i < 4; i++)
    {
      EEPROM.write(kEepromSamplePeriodAddr + i, static_cast<uint8_t>(periodMs >> (8 * i)));
    }
    EEPROM.write(kEepromSamplePeriodFlagAddr, kSamplePeriodConfiguredFlag);

    gSamplePeriodMs = periodMs;
    dbgPrint("Saved sample period to EEPROM (ms): ");
    dbgPrintln(gSamplePeriodMs);
    return true;
  }

  /** Fallback device ID when none is stored: the STM32 96-bit unique ID as hex. */
  void useMcuUniqueIdAsSerialNumber()
  {
    snprintf(gSerialNumber, sizeof(gSerialNumber), "%08lX%08lX%08lX",
             static_cast<unsigned long>(HAL_GetUIDw2()),
             static_cast<unsigned long>(HAL_GetUIDw1()),
             static_cast<unsigned long>(HAL_GetUIDw0()));
    dbgPrint("Using MCU unique ID as SerialNumber: ");
    dbgPrintln(gSerialNumber);
  }

  // ----------------------------
  // Raw HM-10 UART output
  // ----------------------------
  /**
   * Writes bytes to the HM-10 without ever blocking for long.
   * Uart::write() spins forever once its TX buffer is full and the UART is not draining it
   * (HM-10 unpowered, LPUART not running), which would freeze the whole loop. Write only what
   * fits and give up after a timeout instead.
   */
  bool writeToBleUart(const char *bytes, size_t len)
  {
    const unsigned long startMs = millis();
    size_t sent = 0;
    while (sent < len)
    {
      if (bleUart.availableForWrite() > 0)
      {
        bleUart.write(static_cast<uint8_t>(bytes[sent++]));
      }
      else if (millis() - startMs >= kBleWriteTimeoutMs)
      {
        dbgPrintln("ERROR: BLE UART not transmitting; check HM-10 power and wiring (F_A0/F_A3)");
        return false;
      }
    }
    return true;
  }

  /**
   * Clocks LPUART1 (A0/A3) from the 16 MHz HSI. Must run before the first bleUart.begin().
   * Left alone, the core can't use the 120 MHz PCLK1 at 9600 baud (LPUART needs 3-4096x the baud),
   * falls back to the 32 kHz LSE, and if that isn't ready leaves the UART unclocked: nothing is
   * received and the TX buffer never drains. 16 MHz suits every baud from 9600 to 115200, and the
   * selection survives bleUart.end()/begin().
   */
  bool clockBleUartFromHsi()
  {
    __HAL_RCC_HSI_ENABLE();
    const unsigned long startMs = millis();
    while (!__HAL_RCC_GET_FLAG(RCC_FLAG_HSIRDY))
    {
      if (millis() - startMs >= kHsiStartTimeoutMs)
      {
        dbgPrintln("ERROR: HSI oscillator did not start; BLE UART keeps the core's clock choice");
        return false;
      }
    }
    __HAL_RCC_LPUART1_CONFIG(RCC_LPUART1CLKSOURCE_HSI);
    return true;
  }

  // ----------------------------
  // HM-10 AT setup (boot only, while no phone is connected)
  // ----------------------------
  /** Sends one AT command and returns the module's reply ("" on timeout). */
  String sendAtCommand(const String &command)
  {
    while (bleUart.available())
    {
      bleUart.read(); // drop stale bytes so they don't look like the reply
    }

    const String line = command + gAtLineEnding;
    dbgPrint("AT >> ");
    dbgPrintln(command);
    if (!writeToBleUart(line.c_str(), line.length()))
    {
      return "";
    }

    String reply;
    const unsigned long startMs = millis();
    unsigned long lastByteMs = 0;
    while (millis() - startMs < kAtResponseTimeoutMs)
    {
      if (bleUart.available())
      {
        reply += static_cast<char>(bleUart.read());
        lastByteMs = millis();
      }
      else if (lastByteMs != 0 && millis() - lastByteMs >= kAtResponseQuietMs)
      {
        break;
      }
    }
    reply.trim();
    dbgPrint("AT << ");
    dbgPrintln(reply.length() > 0 ? reply : String("(no reply)"));
    return reply;
  }

  /** Tries every baud and AT line ending until the HM-10 answers "OK". Leaves the UART at the one found. */
  bool findHm10()
  {
    // A genuine HM-10 that was still linked (MCU reset while the phone stayed connected) drops the
    // link on "AT" and replies "OK+LOST"; the PWA then shows a disconnect and can reconnect.
    for (const unsigned long baud : kBleBaudCandidates)
    {
      bleUart.end();
      bleUart.begin(baud);
      delay(50);
      for (const char *ending : kAtLineEndings)
      {
        gAtLineEnding = ending;
        dbgPrint("Probing HM-10 at ");
        dbgPrint(baud);
        dbgPrintln(ending[0] == '\0' ? " baud, no line ending" : " baud, CRLF line ending");
        for (uint8_t attempt = 0; attempt < kAtRetries; attempt++)
        {
          if (sendAtCommand("AT").startsWith("OK"))
          {
            gBleBaud = baud;
            return true;
          }
        }
      }
    }

    bleUart.end();
    bleUart.begin(gBleBaud);
    gAtLineEnding = kAtLineEndings[0];
    return false;
  }

  /** Name, AT+NOTI1 and a reset if anything changed. Returns false if the HM-10 never answered. */
  bool configureHm10()
  {
    if (!findHm10())
    {
      dbgPrintln("WARNING: HM-10 never answered AT at any baud. Check 3V3/GND, and that HM-10 TXD -> F_A0 and");
      dbgPrintln("         HM-10 RXD -> F_A3 (not swapped). Sending without link tracking at the default baud.");
      return false;
    }
    dbgPrint("HM-10 found at ");
    dbgPrint(gBleBaud);
    dbgPrintln(" baud");

    bool needsReset = false;

    char name[13];
    snprintf(name, sizeof(name), "%s%04lX", kBleNamePrefix,
             static_cast<unsigned long>(HAL_GetUIDw0() & 0xFFFF));
    if (!sendAtCommand("AT+NAME?").endsWith(name))
    {
      sendAtCommand(String("AT+NAME") + name);
      needsReset = true;
    }

    gTrackLink = sendAtCommand("AT+NOTI?").endsWith("1");
    if (!gTrackLink)
    {
      gTrackLink = sendAtCommand("AT+NOTI1").startsWith("OK");
      needsReset = needsReset || gTrackLink;
    }
    if (!gTrackLink)
    {
      dbgPrintln("WARNING: HM-10 doesn't support AT+NOTI; sending without link tracking.");
    }

    if (needsReset)
    {
      sendAtCommand("AT+RESET");
      delay(1000); // the module reboots before it advertises again
    }

    dbgPrint("HM-10 advertising as: ");
    dbgPrintln(name);
    return true;
  }

  // ----------------------------
  // BLE output
  // ----------------------------
  void sendJsonToBle(const JsonDocument &doc)
  {
    char line[kMaxBleLineLength + 3];
    const size_t len = serializeJson(doc, line, kMaxBleLineLength);
    if (len == 0 || len >= kMaxBleLineLength)
    {
      dbgPrintln("ERROR: BLE message too long, not sent");
      return;
    }
    line[len] = '\r';
    line[len + 1] = '\n';
    line[len + 2] = '\0';

    // With no phone connected the HM-10 would read the JSON as AT input, so don't send it.
    if (gTrackLink && !gLinkUp)
    {
      dbgPrint("(no link, not sent) ");
      dbgPrint(line);
      return;
    }

    // Log first, so the USB monitor shows activity even if the BLE UART is stuck.
    dbgPrint("<< BLE: ");
    dbgPrint(line);
    if (writeToBleUart(line, len + 2))
    {
      gTxLines++;
    }
  }

  /** Sends one protocol envelope; the caller fills `data`. */
  void sendEnvelope(JsonDocument &doc)
  {
    doc["deviceId"] = gSerialNumber;
    doc["timestamp"] = currentEpochMs();
    if (!doc["data"].is<JsonObject>())
    {
      doc["data"].to<JsonObject>();
    }
    sendJsonToBle(doc);
  }

  /** Rejects a command; `cmd` is nullptr when the command name was not recognised. */
  void sendError(const char *error, const char *cmd = nullptr)
  {
    gRxErrors++;
    JsonDocument doc;
    doc["data"]["error"] = error;
    if (cmd != nullptr)
    {
      doc["data"]["cmd"] = cmd;
    }
    sendEnvelope(doc);
  }

  void sendConfig()
  {
    JsonDocument doc;
    doc["data"]["time_synced"] = gTimeSynced;
    doc["data"]["sample_period_ms"] = gSamplePeriodMs;
    sendEnvelope(doc);
  }

  int readMaterialHeight()
  {
    if (kSimulateSensor)
    {
      // Triangle wave over the 12-bit range, one full cycle every ~2 minutes.
      const unsigned long phase = (millis() / 15) % 8190;
      return phase < 4095 ? static_cast<int>(phase) : static_cast<int>(8190 - phase);
    }

    uint32_t sum = 0;
    for (uint8_t i = 0; i < kAdcAverageSamples; i++)
    {
      sum += analogRead(kAdcPin);
    }
    return static_cast<int>(sum / kAdcAverageSamples);
  }

  void sendSample()
  {
    JsonDocument doc;
    doc["data"]["material_height"] = readMaterialHeight(); // demo: raw ADC value
    sendEnvelope(doc);
  }

  // ----------------------------
  // BLE input (commands from the PWA)
  // ----------------------------
  void handleAction(const char *cmd)
  {
    if (strcmp(cmd, "read_now") == 0)
    {
      sendSample();
    }
    else if (strcmp(cmd, "get_config") == 0)
    {
      sendConfig();
    }
    else if (strcmp(cmd, "reset_config") == 0)
    {
      clearSerialNumberEeprom();
      useMcuUniqueIdAsSerialNumber();
      sendConfig();
    }
    else
    {
      sendError("Unknown action");
    }
  }

  void handleSetting(const char *name, const char *value)
  {
    if (strcmp(name, "set_time") == 0)
    {
      char *end = nullptr;
      const unsigned long long epochMs = strtoull(value, &end, 10);
      if (value[0] == '\0' || *end != '\0')
      {
        sendError("set_time needs epoch milliseconds", name);
        return;
      }
      gEpochMsAtSync = epochMs;
      gMillisAtSync = millis();
      gTimeSynced = true;

      JsonDocument doc;
      doc["data"]["time_synced"] = true;
      sendEnvelope(doc);
    }
    else if (strcmp(name, "set_device_id") == 0)
    {
      if (saveSerialNumberToEeprom(value))
      {
        sendConfig();
      }
      else
      {
        sendError("Device ID must be 1-127 printable ASCII characters", name);
      }
    }
    else if (strcmp(name, "set_sample_period_ms") == 0)
    {
      char *end = nullptr;
      const unsigned long periodMs = strtoul(value, &end, 10);
      if (value[0] == '\0' || *end != '\0' || !saveSamplePeriodToEeprom(periodMs))
      {
        sendError("Sample period must be 1000-3600000 ms", name);
        return;
      }
      // Count the new interval from now, so the next sample is a full period away.
      lastSampleMs = millis();
      sendConfig();
    }
    else
    {
      sendError("Unknown setting");
    }
  }

  void handleBleLine(const String &line)
  {
    dbgPrint(">> BLE: ");
    dbgPrintln(line);
    gRxLines++;
    // Only a connected phone can send us a command, even if "OK+CONN" was missed.
    gLinkUp = true;

    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok || !doc.is<JsonObject>())
    {
      sendError("Invalid JSON");
      return;
    }

    // Every command is a one-key object: {"cmd":"<action>"} or {"<setting>":"<value>"}.
    JsonObject command = doc.as<JsonObject>();
    if (command.size() != 1)
    {
      sendError("Command must have exactly one key");
      return;
    }
    JsonPair pair = *command.begin();
    const char *key = pair.key().c_str();
    if (!pair.value().is<const char *>())
    {
      sendError("Command value must be a string", strcmp(key, "cmd") == 0 ? nullptr : key);
      return;
    }
    const char *value = pair.value().as<const char *>();

    if (strcmp(key, "cmd") == 0)
    {
      handleAction(value);
    }
    else
    {
      handleSetting(key, value);
    }
  }

  void setLinkUp(bool up)
  {
    if (up == gLinkUp)
    {
      return;
    }
    gLinkUp = up;
    dbgPrintln(up ? "-- HM-10: phone connected --" : "-- HM-10: phone disconnected --");
    if (up)
    {
      // Start the sample interval at the connect; the PWA asks for config itself once notifications
      // are on (anything sent before that is dropped by the HM-10).
      lastSampleMs = millis();
    }
  }

  /** Strips an HM-10 link notification off the end of the current line, if one just completed. */
  bool consumeLinkNotification()
  {
    if (gBleLine.endsWith(kHm10Connected))
    {
      gBleLine.remove(gBleLine.length() - strlen(kHm10Connected));
      setLinkUp(true);
      return true;
    }
    if (gBleLine.endsWith(kHm10Lost))
    {
      setLinkUp(false);
      // The link dropped mid-line: throw the partial command away.
      gBleLine = "";
      gBleLineOverflow = false;
      return true;
    }
    return false;
  }

  /** Collects bytes from the HM-10 without blocking; the PWA writes each line in 20-byte chunks. */
  void pollBleUart()
  {
    while (bleUart.available())
    {
      const char c = static_cast<char>(bleUart.read());
      gRxBytes++;
      if (c == '\n')
      {
        if (!gBleLineOverflow)
        {
          gBleLine.trim();
          if (gBleLine.length() > 0)
          {
            handleBleLine(gBleLine);
          }
        }
        gBleLine = "";
        gBleLineOverflow = false;
      }
      else if (gBleLine.length() >= kMaxBleLineLength)
      {
        gBleLineOverflow = true;
      }
      else
      {
        gBleLine += c;
        consumeLinkNotification();
      }
    }
  }

  void logStatus()
  {
    dbgPrint("[status] link=");
    dbgPrint(gTrackLink ? (gLinkUp ? "up" : "down") : "untracked");
    dbgPrint(" baud=");
    dbgPrint(gBleBaud);
    dbgPrint(" rx_bytes=");
    dbgPrint(gRxBytes);
    dbgPrint(" rx=");
    dbgPrint(gRxLines);
    dbgPrint(" tx=");
    dbgPrint(gTxLines);
    dbgPrint(" errors=");
    dbgPrint(gRxErrors);
    dbgPrint(" period_ms=");
    dbgPrint(gSamplePeriodMs);
    dbgPrint(" time_synced=");
    dbgPrintln(gTimeSynced ? "yes" : "no");
  }

} // namespace

// ===================================================================
//  SETUP
// ===================================================================
void setup()
{
  Serial.begin(9600); // USB debug log
  clockBleUartFromHsi();
  bleUart.begin(gBleBaud);
  gBleLine.reserve(kMaxBleLineLength);

  pinMode(kAdcPin, INPUT_ANALOG);
  analogReadResolution(12);

  delay(3000);
  dbgPrintln("=== Bin Height Measurement firmware booting (HM-10) ===");

  // Clear EEPROM on boot if enabled (for testing/reflashing)
  if (kClearEepromOnBoot)
  {
    dbgPrintln("WARNING: Clearing EEPROM (kClearEepromOnBoot = 1)");
    clearSerialNumberEeprom();
  }

  if (!loadSerialNumberFromEeprom())
  {
    useMcuUniqueIdAsSerialNumber();
  }
  loadSamplePeriodFromEeprom();

  configureHm10();

  dbgPrintln("===Starting main loop, waiting for a phone to connect===");
}

// ===================================================================
//  MAIN LOOP
// ===================================================================
void loop()
{
  const unsigned long nowMs = millis();

  // 1) Inbound commands and link notifications from the HM-10.
  pollBleUart();

  // 2) Periodic sample -> PWA, only while a phone is connected (or always, if that can't be told).
  if ((gLinkUp || !gTrackLink) && nowMs - lastSampleMs >= gSamplePeriodMs)
  {
    lastSampleMs = nowMs;
    sendSample();
  }

  // 3) Link/traffic summary on USB.
  if (nowMs - lastStatusLogMs >= kStatusLogPeriodMs)
  {
    lastStatusLogMs = nowMs;
    logStatus();
  }
}
