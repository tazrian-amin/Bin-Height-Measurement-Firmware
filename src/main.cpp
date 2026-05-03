#include <Arduino.h>

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

namespace {

// ----------------------------
// Debug logging
// ----------------------------
constexpr bool kDebugLogEnabled = true;

template <typename T>
inline void dbgPrint(const T& v) {
  if (kDebugLogEnabled) {
    Serial.print(v);
  }
}

template <typename T>
inline void dbgPrintln(const T& v) {
  if (kDebugLogEnabled) {
    Serial.println(v);
  }
}

inline void dbgPrintln() {
  if (kDebugLogEnabled) {
    Serial.println();
  }
}

// ----------------------------
// Notecard wiring (Notecarrier-F)
// ----------------------------
// - `F_TX` -> `N_RX` and `F_RX` -> `N_TX`  (this is `Serial1`)
// - `F_D5` -> `N_ATTN` (attention interrupt)
HardwareSerial& notecardUart = Serial1;
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
constexpr const char* kProductUid = "com.gmail.amin.tazrian1979:binheightv2";
/** Inbound: commands from Notehub (card.attn + note.get). */
constexpr const char* kInboundNotefile = "data.qi";
/** Outbound: telemetry to Notehub (note.add). Must be `.qo`, not `.qi`. */
constexpr const char* kOutboundNotefile = "height.qo";

// ----------------------------
// Timing
// ----------------------------
constexpr unsigned long kSamplePeriodMs = 5000;
unsigned long lastSampleMs = 0;

void armNotecardAttn() {
  char attnCmd[200];
  snprintf(attnCmd, sizeof(attnCmd),
           "{\"req\":\"card.attn\",\"mode\":\"arm,files\",\"files\":[\"%s\"]}",
           kInboundNotefile);
  notecardUart.println(attnCmd);
  notecardUart.readStringUntil('\n'); // clear response
}

} // namespace

// ===================================================================
//  SETUP
// ===================================================================
void setup() {
  Serial.begin(9600);        // wired output (USB)
  notecardUart.begin(9600);  // Notecard (Serial1 via F_TX/F_RX)
  bleUart.begin(9600);       // HM-10 default baud unless changed via AT commands

  notecardUart.setTimeout(5000);
  bleUart.setTimeout(1000);

  pinMode(kAdcPin, INPUT_ANALOG);
  analogReadResolution(12);
  pinMode(kNotecardAttnPin, INPUT);

  delay(3000);

  // Configure Notecard
  notecardUart.println((String("{\"req\":\"hub.set\",\"product\":\"") + kProductUid + "\"}").c_str());
  delay(1000);
  notecardUart.println("{\"req\":\"hub.set\",\"mode\":\"continuous\",\"sync\":true}");
  delay(3000);

  dbgPrint("Notecard configured for ProductUID: ");
  dbgPrintln(kProductUid);
  delay(5000);

  armNotecardAttn();

  dbgPrintln("===Starting main loop===");
  delay(3000);
}

// ===================================================================
//  MAIN LOOP
// ===================================================================
void loop() {
  const unsigned long nowMs = millis();

  // 1) Handle inbound Notecard note notifications (ATTN pin goes HIGH).
  if (digitalRead(kNotecardAttnPin) == HIGH) {
    dbgPrintln();
    dbgPrintln("-- Notecard ATTN HIGH: polling inbound note --");

    // Sync, then fetch and delete one note from kInboundNotefile.
    notecardUart.println("{\"req\":\"hub.sync\"}");
    notecardUart.readStringUntil('\n'); // clear immediate {}
    delay(5000);

    while (notecardUart.available()) {
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

  // 2) Optional: read inbound bytes from HM-10 UART (useful for debugging AT mode).
  if (bleUart.available()) {
    String line = bleUart.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      dbgPrint(">> HM-10 UART: ");
      dbgPrintln(line);
    }
  }

  // 3) Periodic ADC sample -> send to USB (wired) + HM-10 (wireless) + Notecard (cloud).
  if (nowMs - lastSampleMs >= kSamplePeriodMs) {
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
             "{\"req\":\"note.add\",\"file\":\"%s\",\"body\":{\"adc\":%d}}",
             kOutboundNotefile, adc);
    notecardUart.println(addNoteCmd);
    const String addResp = notecardUart.readStringUntil('\n');
    if (addResp.indexOf("\"err\"") >= 0) {
      dbgPrint("Warning: Note add response: ");
      dbgPrintln(addResp);
    }
  }
}
