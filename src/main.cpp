#include <Arduino.h>
#include <EEPROM.h>
#include <cstring>

/*
  ===========================
  Name ↔ Hardware cheat sheet
  ===========================

  WIRED to Android (USB):
    - Code name: `Serial`
    - Physical: Swan micro-USB port (CDC/USB serial)

  Notecard (UART on Notecarrier-F):
    - Code name: `notecardUart` (alias of `Serial1`)
    - Physical wiring you described:
        F_TX -> N_RX
        F_RX -> N_TX
        F_D5 -> N_ATTN   (ATTN interrupt)

  Notefiles (Important):
    - `*.qi` = inbound (Notehub/device app -> firmware). Use `note.get` here, NOT `note.add`.
    - `*.qo` = outbound (firmware -> Notehub). Use `note.add` here for sensor telemetry.

  HM-10 BLE UART (wireless to Android):
    - Code name: `bleUart` (UART on pins A0/A3)
    - Notecarrier-F silkscreen pins:
        F_A0 = MCU RX  <- HM-10 TXD
        F_A3 = MCU TX  -> HM-10 RXD
        3V3            -> HM-10 VCC
        GND            -> HM-10 GND

  Important:
    - Do NOT connect a USB-Serial adapter to F_A0/F_A3 at the same time as the HM-10.
      Wired mode should use the Swan micro-USB (`Serial`), while wireless uses HM-10 (`bleUart`).

  ==================
  Naming conventions
  ==================

  - `kSomething`: a compile-time constant (the "k" prefix is a common C++ convention for constants).
    Example: `kSamplePeriodMs` is the fixed interval between samples.

  - Units are part of names when it helps avoid mistakes:
      `Ms` = milliseconds, so `kSamplePeriodMs` is measured in milliseconds.

  - `lastSomethingMs`: a runtime variable that stores the last time an event happened (in ms from `millis()`).
    Example: `lastSampleMs` is updated each time we take/send a sample, so we can send again after
    `kSamplePeriodMs` elapses without blocking the loop.
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
  // Notecard wiring (Notecarrier-F)
  // ----------------------------
  // - `F_TX` -> `N_RX` and `F_RX` -> `N_TX`  (this is `Serial1`)
  // - `F_D5` -> `N_ATTN` (attention interrupt)
  HardwareSerial &notecardUart = Serial1;
  constexpr uint8_t kNotecardAttnPin = D5;

  // ----------------------------
  // Android outputs (choose wired or wireless)
  // ----------------------------
  // WIRED (USB): Swan micro-USB -> `Serial`
  // WIRELESS (BLE HM-10): UART on Notecarrier-F `F_A0`/`F_A3`
  //
  // HM-10 connections:
  // - HM-10 TXD -> F_A0 (MCU RX)
  // - HM-10 RXD -> F_A3 (MCU TX)
  // - HM-10 VCC -> 3V3
  // - HM-10 GND -> GND
  //
  // Note: A USB-Serial adapter connected to F_A0/F_A3 conflicts with the HM-10.
  constexpr uint8_t kBleUartRxPin = A0; // MCU RX  <- HM-10 TXD
  constexpr uint8_t kBleUartTxPin = A3; // MCU TX  -> HM-10 RXD
  HardwareSerial bleUart(kBleUartRxPin, kBleUartTxPin);

  // ----------------------------
  // Sensor input
  // ----------------------------
  constexpr uint8_t kAdcPin = PA1;

  // ----------------------------
  // Notecard config
  // ----------------------------
  // Product UID and serial are supplied by the Android app (setup_device) or loaded from EEPROM.
  /** Inbound: commands from Notehub (card.attn + note.get). */
  constexpr const char *kInboundNotefile = "data.qi";
  /** Outbound: telemetry to Notehub (note.add). Must be `.qo`, not `.qi`. */
  constexpr const char *kOutboundNotefile = "height.qo";

  // ----------------------------
  // Debug/Dev flags
  // ----------------------------
  // Set to 1 to clear EEPROM on every boot (useful for testing/reflashing)
  // Set to 0 for normal operation (preserves stored config across power cycles)
  constexpr bool kClearEepromOnBoot = 0;

  // Runtime state: tracks if device needs setup/reconfiguration
  bool gNeedsSetup = false;

  // ----------------------------
  // EEPROM config (Flash storage)
  // ----------------------------
  // Layout:
  //   [0:0]       = 0xAA if product UID is configured, else 0x00
  //   [1:128]     = Product UID string (null-terminated, max 127 chars)
  //   [129:129]   = 0xBB if serial number is configured, else 0x00
  //   [130:257]   = Serial Number string (null-terminated, max 127 chars)
  constexpr uint16_t kEepromFlagAddr = 0;
  constexpr uint16_t kEepromProductUidAddr = 1;
  constexpr uint16_t kMaxProductUidLength = 127;
  constexpr uint8_t kConfiguredFlag = 0xAA;

  constexpr uint16_t kEepromSerialNumFlagAddr = 129;
  constexpr uint16_t kEepromSerialNumAddr = 130;
  constexpr uint16_t kMaxSerialNumLength = 127;
  constexpr uint8_t kSerialNumConfiguredFlag = 0xBB;

  // Runtime product UID and serial number (loaded from EEPROM or set during setup)
  char gProductUid[kMaxProductUidLength + 1] = {0};
  char gSerialNumber[kMaxSerialNumLength + 1] = {0};

  // ----------------------------
  // Timing
  // ----------------------------
  constexpr unsigned long kSamplePeriodMs = 300000; // 5 minutes in milliseconds
  unsigned long lastSampleMs = 0;

  void armNotecardAttn()
  {
    char attnCmd[200];
    snprintf(attnCmd, sizeof(attnCmd),
             "{\"req\":\"card.attn\",\"mode\":\"arm,files\",\"files\":[\"%s\"]}",
             kInboundNotefile);
    notecardUart.println(attnCmd);
    notecardUart.readStringUntil('\n'); // clear response
  }

  // ----------------------------
  // EEPROM helper functions
  // ----------------------------
  /**
   * Load product UID from EEPROM.
   * Returns true if a valid UID exists, false if not configured (first-time setup).
   */
  bool loadProductUidFromEeprom()
  {
    uint8_t flag = EEPROM.read(kEepromFlagAddr);
    if (flag != kConfiguredFlag)
    {
      dbgPrintln("-- No stored ProductUID (first-time setup) --");
      return false;
    }

    // Read null-terminated string from EEPROM
    uint16_t addr = kEepromProductUidAddr;
    uint16_t len = 0;
    while (len < kMaxProductUidLength)
    {
      char c = EEPROM.read(addr + len);
      gProductUid[len] = c;
      if (c == '\0')
      {
        dbgPrint("Loaded ProductUID from EEPROM: ");
        dbgPrintln(gProductUid);
        return true;
      }
      len++;
    }

    dbgPrintln("ERROR: ProductUID in EEPROM is not null-terminated");
    return false;
  }

  /**
   * Save product UID to EEPROM.
   */
  void saveProductUidToEeprom(const char *uid)
  {
    if (uid == nullptr || uid[0] == '\0')
    {
      dbgPrintln("ERROR: Cannot save empty ProductUID");
      return;
    }

    size_t len = strlen(uid);
    if (len > kMaxProductUidLength)
    {
      dbgPrint("ERROR: ProductUID too long (max ");
      dbgPrint(kMaxProductUidLength);
      dbgPrintln(" chars)");
      return;
    }

    // Write UID string + null, then clear tail (avoid stale chars when new UID is shorter)
    for (size_t i = 0; i <= len; i++)
    {
      EEPROM.write(kEepromProductUidAddr + i, uid[i]);
    }
    for (size_t i = len + 1; i <= kMaxProductUidLength; i++)
    {
      EEPROM.write(kEepromProductUidAddr + i, 0);
    }

    // Write configured flag
    EEPROM.write(kEepromFlagAddr, kConfiguredFlag);

    strcpy(gProductUid, uid);
    dbgPrint("Saved ProductUID to EEPROM: ");
    dbgPrintln(gProductUid);
  }

  /**
   * Clear stored product UID from EEPROM (reset to first-time setup).
   */
  void clearProductUidEeprom()
  {
    EEPROM.write(kEepromFlagAddr, 0x00);
    memset(gProductUid, 0, sizeof(gProductUid));
    dbgPrintln("Cleared ProductUID from EEPROM");
  }

  // ----------------------------
  // Serial Number EEPROM functions
  // ----------------------------
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
        dbgPrint("Loaded SerialNumber from EEPROM: ");
        dbgPrintln(gSerialNumber);
        return true;
      }
      len++;
    }

    dbgPrintln("ERROR: SerialNumber in EEPROM is not null-terminated");
    return false;
  }

  /**
   * Save serial number to EEPROM.
   */
  void saveSerialNumberToEeprom(const char *sn)
  {
    if (sn == nullptr || sn[0] == '\0')
    {
      dbgPrintln("ERROR: Cannot save empty SerialNumber");
      return;
    }

    size_t len = strlen(sn);
    if (len > kMaxSerialNumLength)
    {
      dbgPrint("ERROR: SerialNumber too long (max ");
      dbgPrint(kMaxSerialNumLength);
      dbgPrintln(" chars)");
      return;
    }

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

  /**
   * Clear entire configuration from EEPROM (both ProductUID and SerialNumber).
   */
  void clearAllConfigEeprom()
  {
    clearProductUidEeprom();
    clearSerialNumberEeprom();
    dbgPrintln("Cleared ALL config from EEPROM");
  }

  void sendGetConfigJsonTo(Print &port)
  {
    port.print("{\"status\":\"ok\",\"product_uid\":\"");
    port.print(gProductUid);
    port.print("\",\"serial_number\":\"");
    port.print(gSerialNumber);
    port.println("\"}");
  }

  /** If `line` is get_config, reply on `replyPort` and return true. */
  bool tryHandleGetConfigLine(const String &line, Print &replyPort)
  {
    if (line.indexOf("\"cmd\":\"get_config\"") == -1)
    {
      return false;
    }
    sendGetConfigJsonTo(replyPort);
    return true;
  }

  /** JSON fragment `"key":"value"` — value must not contain escaped quotes for this parser. */
  bool extractJsonStringValue(const String &line, const char *keyWithQuotes, String &out)
  {
    out = "";
    const int p = line.indexOf(keyWithQuotes);
    if (p < 0)
    {
      return false;
    }
    const int start = p + static_cast<int>(strlen(keyWithQuotes));
    const int end = line.indexOf('"', start);
    if (end <= start)
    {
      return false;
    }
    out = line.substring(start, end);
    return true;
  }

  void pushNotecardHubIdentityAndSync()
  {
    notecardUart.println((String("{\"req\":\"hub.set\",\"product\":\"") + gProductUid + "\",\"sn\":\"" + gSerialNumber + "\"}").c_str());
    delay(1000);
    notecardUart.println("{\"req\":\"hub.set\",\"mode\":\"continuous\",\"sync\":true}");
    delay(2000);
    dbgPrintln("Performing hub.sync after identity update...");
    notecardUart.println("{\"req\":\"hub.sync\"}");
    notecardUart.readStringUntil('\n');
    delay(1500);
  }

  /**
   * Runtime: apply product UID + serial from one JSON line, push to Notecard, restart MCU.
   * Recognizes setup_device (lenient) or set_config. Replies on primary; optional secondary (both ports).
   */
  bool tryHandleRuntimeIdentityCommand(const String &line, Print &replyPrimary, Print *replySecondary)
  {
    const bool cmdGetCfg = line.indexOf("\"cmd\":\"get_config\"") >= 0;
    if (cmdGetCfg)
    {
      return false;
    }

    const bool wantsSetup =
        line.indexOf("setup_device") >= 0 || line.indexOf("\"cmd\":\"setup_device\"") >= 0;
    const bool wantsSetCfg = line.indexOf("\"cmd\":\"set_config\"") >= 0;
    if (!wantsSetup && !wantsSetCfg)
    {
      return false;
    }

    String uid;
    String sn;
    const bool hasUid = extractJsonStringValue(line, "\"product_uid\":\"", uid);
    const bool hasSn = extractJsonStringValue(line, "\"serial_number\":\"", sn);

    if (wantsSetup)
    {
      if (!hasUid || !hasSn || uid.length() == 0 || sn.length() == 0)
      {
        replyPrimary.println("{\"status\":\"error\",\"msg\":\"setup_device requires product_uid and serial_number\"}");
        if (replySecondary != nullptr)
        {
          replySecondary->println("{\"status\":\"error\",\"msg\":\"setup_device requires product_uid and serial_number\"}");
        }
        return true;
      }
      if (uid.length() > kMaxProductUidLength || sn.length() > kMaxSerialNumLength)
      {
        replyPrimary.println("{\"status\":\"error\",\"msg\":\"UID or serial too long\"}");
        if (replySecondary != nullptr)
        {
          replySecondary->println("{\"status\":\"error\",\"msg\":\"UID or serial too long\"}");
        }
        return true;
      }
      saveProductUidToEeprom(uid.c_str());
      saveSerialNumberToEeprom(sn.c_str());
    }
    else
    {
      // set_config: allow partial updates
      bool any = false;
      if (hasUid && uid.length() > 0 && uid.length() <= kMaxProductUidLength)
      {
        saveProductUidToEeprom(uid.c_str());
        any = true;
      }
      if (hasSn && sn.length() > 0 && sn.length() <= kMaxSerialNumLength)
      {
        saveSerialNumberToEeprom(sn.c_str());
        any = true;
      }
      if (!any)
      {
        replyPrimary.println("{\"status\":\"error\",\"msg\":\"No valid product_uid or serial_number in set_config\"}");
        if (replySecondary != nullptr)
        {
          replySecondary->println("{\"status\":\"error\",\"msg\":\"No valid product_uid or serial_number in set_config\"}");
        }
        return true;
      }
    }

    replyPrimary.println(
        "{\"status\":\"ok\",\"msg\":\"Identity saved. Syncing Notehub and restarting...\"}");
    if (replySecondary != nullptr)
    {
      replySecondary->println(
          "{\"status\":\"ok\",\"msg\":\"Identity saved. Syncing Notehub and restarting...\"}");
    }
    dbgPrintln(">> Runtime identity update: pushing hub.set + sync, then reset");
    pushNotecardHubIdentityAndSync();
    delay(500);
    NVIC_SystemReset();
    return true;
  }

  // ----------------------------
  // Serial setup phase (receive ProductUID and SerialNumber from Android app)
  // ----------------------------
  /**
   * Block until both product UID and serial are stored AND confirmed by Android app.
   * Flow:
   *   1. Wait for setup_device command with product_uid and serial_number
   *   2. Store both values to EEPROM
   *   3. Ask for confirmation via confirm_setup command
   *   4. Only return when confirmed
   * Responds to get_config on USB and BLE during this wait so the app can read values.
   * No timeout: main loop / ADC are not started until this returns.
   */
  void setupPhaseReceiveDeviceConfig(bool uidAlreadyStored, bool snAlreadyStored)
  {
    bool uidReceived = uidAlreadyStored;
    bool snReceived = snAlreadyStored;
    bool confirmationReceived = false;

    dbgPrintln();
    dbgPrintln("========================================");
    dbgPrintln("  SETUP PHASE: Device Configuration");
    dbgPrintln("========================================");
    dbgPrintln();
    dbgPrintln("STEP 1: Send setup command from Android app:");
    dbgPrintln(R"({"cmd":"setup_device","product_uid":"YOUR_UID","serial_number":"YOUR_SN"})");
    dbgPrintln();
    dbgPrintln("STEP 2: After receiving values, send confirmation:");
    dbgPrintln(R"({"cmd":"confirm_setup"})");
    dbgPrintln();
    dbgPrintln("Optional: Send {\"cmd\":\"get_config\"} to read current values.");
    dbgPrintln();

    while (!confirmationReceived)
    {
      if (Serial.available())
      {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.length() > 0)
        {
          dbgPrint(">> USB: ");
          dbgPrintln(line);

          if (tryHandleGetConfigLine(line, Serial))
          {
            // handled
          }
          else if (line.indexOf("\"cmd\":\"confirm_setup\"") != -1)
          {
            if (uidReceived && snReceived)
            {
              confirmationReceived = true;
              Serial.println("{\"status\":\"ok\",\"msg\":\"Setup confirmed. Device booting...\"}");
              dbgPrintln("✓ Setup confirmed by Android app!");
            }
            else
            {
              Serial.println("{\"status\":\"error\",\"msg\":\"ProductUID and SerialNumber must be set first\"}");
              dbgPrintln("✗ Cannot confirm: missing ProductUID or SerialNumber");
            }
          }
          else
          {
            String uid;
            String sn;
            if (!uidReceived && extractJsonStringValue(line, "\"product_uid\":\"", uid))
            {
              if (uid.length() > 0 && uid.length() <= kMaxProductUidLength)
              {
                saveProductUidToEeprom(uid.c_str());
                uidReceived = true;
                dbgPrintln("✓ ProductUID received and stored");
              }
            }
            if (!snReceived && extractJsonStringValue(line, "\"serial_number\":\"", sn))
            {
              if (sn.length() > 0 && sn.length() <= kMaxSerialNumLength)
              {
                saveSerialNumberToEeprom(sn.c_str());
                snReceived = true;
                dbgPrintln("✓ SerialNumber received and stored");
              }
            }
          }
        }
      }

      if (bleUart.available())
      {
        String line = bleUart.readStringUntil('\n');
        line.trim();
        if (line.length() > 0)
        {
          dbgPrint(">> BLE: ");
          dbgPrintln(line);

          if (tryHandleGetConfigLine(line, bleUart))
          {
            // handled
          }
          else if (line.indexOf("\"cmd\":\"confirm_setup\"") != -1)
          {
            if (uidReceived && snReceived)
            {
              confirmationReceived = true;
              bleUart.println("{\"status\":\"ok\",\"msg\":\"Setup confirmed. Device booting...\"}");
              dbgPrintln("✓ Setup confirmed by Android app!");
            }
            else
            {
              bleUart.println("{\"status\":\"error\",\"msg\":\"ProductUID and SerialNumber must be set first\"}");
              dbgPrintln("✗ Cannot confirm: missing ProductUID or SerialNumber");
            }
          }
          else
          {
            String uid;
            String sn;
            if (!uidReceived && extractJsonStringValue(line, "\"product_uid\":\"", uid))
            {
              if (uid.length() > 0 && uid.length() <= kMaxProductUidLength)
              {
                saveProductUidToEeprom(uid.c_str());
                uidReceived = true;
                dbgPrintln("✓ ProductUID received and stored");
              }
            }
            if (!snReceived && extractJsonStringValue(line, "\"serial_number\":\"", sn))
            {
              if (sn.length() > 0 && sn.length() <= kMaxSerialNumLength)
              {
                saveSerialNumberToEeprom(sn.c_str());
                snReceived = true;
                dbgPrintln("✓ SerialNumber received and stored");
              }
            }
          }
        }
      }

      delay(50);
    }
  }

} // namespace

// ===================================================================
//  SETUP
// ===================================================================
void setup()
{
  Serial.begin(9600);       // wired output (USB)
  notecardUart.begin(9600); // Notecard (Serial1 via F_TX/F_RX)
  bleUart.begin(9600);      // HM-10 default baud unless changed via AT commands

  notecardUart.setTimeout(5000);
  bleUart.setTimeout(1000);

  pinMode(kAdcPin, INPUT_ANALOG);
  analogReadResolution(12);
  pinMode(kNotecardAttnPin, INPUT);

  delay(3000);

  // Clear EEPROM on boot if enabled (for testing/reflashing)
  if (kClearEepromOnBoot)
  {
    dbgPrintln("WARNING: Clearing EEPROM (kClearEepromOnBoot = 1)");
    clearAllConfigEeprom();
  }

  // Load product UID and serial number from EEPROM or enter setup phase
  bool uidLoaded = loadProductUidFromEeprom();
  bool snLoaded = loadSerialNumberFromEeprom();

  // Always run setup phase if EEPROM is empty OR if reconfiguration flag is set
  if (!uidLoaded || !snLoaded || gNeedsSetup)
  {
    gNeedsSetup = false; // Reset flag after entering setup
    setupPhaseReceiveDeviceConfig(uidLoaded, snLoaded);
  }

  // Configure Notecard with the product UID and serial number
  notecardUart.println((String("{\"req\":\"hub.set\",\"product\":\"") + gProductUid + "\",\"sn\":\"" + gSerialNumber + "\"}").c_str());
  delay(1000);
  notecardUart.println("{\"req\":\"hub.set\",\"mode\":\"continuous\",\"sync\":true}");
  delay(2000);

  // Sync immediately to register device on Notehub dashboard
  dbgPrintln("Performing initial hub.sync to register device...");
  notecardUart.println("{\"req\":\"hub.sync\"}");
  notecardUart.readStringUntil('\n'); // read response
  delay(3000);

  dbgPrint("Notecard configured - ProductUID: ");
  dbgPrint(gProductUid);
  dbgPrint(" | SerialNumber: ");
  dbgPrintln(gSerialNumber);
  delay(5000);

  armNotecardAttn();

  dbgPrintln("===Starting main loop===");
  delay(3000);
}

// ===================================================================
//  MAIN LOOP
// ===================================================================
void loop()
{
  const unsigned long nowMs = millis();

  // 0) Handle runtime commands from Android app (reset/reconfigure ProductUID and SerialNumber)
  if (Serial.available())
  {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0)
    {
      if (tryHandleGetConfigLine(line, Serial))
      {
        // handled
      }
      else if (tryHandleRuntimeIdentityCommand(line, Serial, &bleUart))
      {
        // handled (may reset MCU)
      }
      else if (line.indexOf("\"cmd\":\"reset_config\"") != -1)
      {
        dbgPrintln(">> Received reset_config command");
        clearProductUidEeprom();
        clearSerialNumberEeprom();
        Serial.println("{\"status\":\"ok\",\"msg\":\"Config cleared. Restarting for reconfiguration...\"}");
        bleUart.println("{\"status\":\"ok\",\"msg\":\"Config cleared. Restarting for reconfiguration...\"}");
        delay(1000);
        NVIC_SystemReset();
      }
      else if (line.indexOf("\"cmd\":\"reset_uid\"") != -1)
      {
        dbgPrintln(">> Received reset_uid command (legacy)");
        clearProductUidEeprom();
        Serial.println("{\"status\":\"ok\",\"msg\":\"ProductUID cleared. Please reset device to reconfigure.\"}");
      }
      else if (line.indexOf("\"cmd\":\"get_uid\"") != -1)
      {
        Serial.print("{\"status\":\"ok\",\"product_uid\":\"");
        Serial.print(gProductUid);
        Serial.println("\"}");
      }
      else if (line.indexOf("\"cmd\":\"set_uid\"") != -1)
      {
        String uid;
        if (extractJsonStringValue(line, "\"product_uid\":\"", uid) && uid.length() > 0 &&
            uid.length() <= kMaxProductUidLength)
        {
          saveProductUidToEeprom(uid.c_str());
          Serial.print("{\"status\":\"ok\",\"msg\":\"ProductUID updated to ");
          Serial.print(uid);
          Serial.println("\"}");
          Serial.println("{\"info\":\"Note: Restart device for Notecard to use new ProductUID\"}");
        }
        else
        {
          Serial.println("{\"status\":\"error\",\"msg\":\"Invalid ProductUID length\"}");
        }
      }
    }
  }

  // 1) Handle inbound Notecard note notifications (ATTN pin goes HIGH).
  if (digitalRead(kNotecardAttnPin) == HIGH)
  {
    dbgPrintln();
    dbgPrintln("-- Notecard ATTN HIGH: polling inbound note --");

    // Sync, then fetch and delete one note from kInboundNotefile.
    notecardUart.println("{\"req\":\"hub.sync\"}");
    notecardUart.readStringUntil('\n'); // clear immediate {}
    delay(5000);

    while (notecardUart.available())
    {
      notecardUart.read();
    }

    char getNoteCmd[300];
    snprintf(getNoteCmd, sizeof(getNoteCmd),
             "{\"req\":\"note.get\",\"file\":\"%s\",\"delete\":true}",
             kInboundNotefile);
    notecardUart.println(getNoteCmd);

    const String noteContent = notecardUart.readStringUntil('\n');
    dbgPrint(">> Note Received: ");
    dbgPrintln(noteContent);

    dbgPrintln("Re-arming Notecard ATTN...");
    armNotecardAttn();
    delay(3000);
  }

  // 2) Inbound bytes from HM-10 UART (Android BLE serial): commands + debug.
  if (bleUart.available())
  {
    String line = bleUart.readStringUntil('\n');
    line.trim();
    if (line.length() > 0)
    {
      if (tryHandleGetConfigLine(line, bleUart))
      {
        // handled
      }
      else if (tryHandleRuntimeIdentityCommand(line, bleUart, &Serial))
      {
        // handled (may reset MCU)
      }
      else
      {
        dbgPrint(">> HM-10 UART: ");
        dbgPrintln(line);
      }
    }
  }

  // 3) Periodic ADC sample -> send to USB (wired) + HM-10 (wireless) + Notecard (cloud).
  if (nowMs - lastSampleMs >= kSamplePeriodMs)
  {
    lastSampleMs = nowMs;

    const int adc = analogRead(kAdcPin);
    dbgPrint("Raw ADC: ");
    dbgPrintln(adc);

    // Wired to Android via USB serial
    Serial.print("ADC value:");
    Serial.print(adc);
    Serial.print("\r\n");

    // Wireless to Android via HM-10 (transparent UART over BLE)
    bleUart.print("ADC value:");
    bleUart.print(adc);
    bleUart.print("\r\n");

    // Cloud via Notecard (outbound queue only — see kOutboundNotefile)
    char addNoteCmd[220];
    snprintf(addNoteCmd, sizeof(addNoteCmd),
             "{\"req\":\"note.add\",\"file\":\"%s\",\"sync\":true,\"body\":{\"adc\":%d}}",
             kOutboundNotefile, adc);
    notecardUart.println(addNoteCmd);
    const String addResp = notecardUart.readStringUntil('\n');
    if (addResp.indexOf("\"err\"") >= 0)
    {
      dbgPrint("Warning: Note add response: ");
      dbgPrintln(addResp);
    }
  }
}
