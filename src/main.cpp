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

HardwareSerial& notecardUart = Serial1;
constexpr uint8_t kNotecardAttnPin = D5; // F_D5 -> N_ATTN (attention interrupt)
constexpr uint8_t kBleUartRxPin = A0; // HM-10 TXD -> F_A0 (MCU RX)
constexpr uint8_t kBleUartTxPin = A3; // HM-10 RXD -> F_A3 (MCU TX)
HardwareSerial bleUart(kBleUartRxPin, kBleUartTxPin);

// ----------------------------
// Sensor input
// ----------------------------
constexpr uint8_t kAdcPin = PA1;

// ----------------------------
// Notecard config
// ----------------------------
constexpr const char* kProductUid = "com.gmail.amin.tazrian1979:binheightv2";
constexpr const char* kInboundNotefile = "data.qi";

// ----------------------------
// Timing
// ----------------------------
constexpr unsigned long kSamplePeriodMs = 5000; // 5 seconds between ADC samples
const unsigned long kSyncTimeoutMs = 5000; // 5 seconds max to wait for Notecard sync response after ATTN
const unsigned long kNotecardResponseTimeoutMs = 5000; // 5 seconds max to wait for any Notecard response (e.g., product config, note add, etc.)
const unsigned long kAdcAverageSamples = 10; // Number of ADC samples to average for each reported value

unsigned long lastSampleMs = 0; // Timestamp of last ADC sample sent
unsigned long syncStartMs = 0; // Timestamp when Notecard sync was initiated (after ATTN)
bool isSyncing = false; // True if we're currently waiting for Notecard sync response after ATTN interrupt
bool attnTriggered = false; // Set to true in ATTN interrupt handler, indicating we should poll for note in main loop

void onAttnInterrupt() {
  attnTriggered = true;
}

bool readNotecardResponse(char* buffer, size_t bufferSize, unsigned long timeoutMs) {
  unsigned long startTime = millis();
  size_t bytesRead = 0;
  
  while (millis() - startTime < timeoutMs) {
    if (notecardUart.available()) {
      char c = notecardUart.read();
      if (bytesRead < bufferSize - 1) {
        buffer[bytesRead++] = c;
      }
      if (c == '\n') {
        buffer[bytesRead] = '\0';
        return true;
      }
    }
  }
  buffer[bytesRead] = '\0';
  return false;
}

bool isValidNotecardResponse(const char* response) {
  return response != nullptr && strlen(response) > 2 && strstr(response, "err") == nullptr;
}

void armNotecardAttn() {
  notecardUart.println("{\"req\":\"card.attn\",\"mode\":\"arm,files\",\"files\":[\"data.qi\"]}" );
  char response[256];
  readNotecardResponse(response, sizeof(response), 1000);
  if (isValidNotecardResponse(response)) {
    dbgPrintln("Notecard ATTN armed successfully");
  } else {
    dbgPrint("Warning: ATTN arm response: ");
    dbgPrintln(response);
  }
}

int readAveragedAdc() {
  long sum = 0;
  for (uint16_t i = 0; i < kAdcAverageSamples; i++) {
    sum += analogRead(kAdcPin);
  }
  return (int)(sum / kAdcAverageSamples);
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
  attachInterrupt(digitalPinToInterrupt(kNotecardAttnPin), onAttnInterrupt, RISING);

  delay(3000);

  // Configure Notecard
  char productCmd[256];
  snprintf(productCmd, sizeof(productCmd), "{\"req\":\"hub.set\",\"product\":\"%s\"}", kProductUid);
  notecardUart.println(productCmd);
  
  char response[256];
  if (readNotecardResponse(response, sizeof(response), kNotecardResponseTimeoutMs)) {
    if (isValidNotecardResponse(response)) {
      dbgPrint("Notecard product set: ");
      dbgPrintln(response);
    } else {
      dbgPrint("Warning: Product config response: ");
      dbgPrintln(response);
    }
  } else {
    dbgPrintln("Error: Notecard product config timeout");
  }
  
  delay(1000);
  
  notecardUart.println("{\"req\":\"hub.set\",\"mode\":\"continuous\",\"sync\":true}");
  if (readNotecardResponse(response, sizeof(response), kNotecardResponseTimeoutMs)) {
    if (isValidNotecardResponse(response)) {
      dbgPrint("Notecard mode set: ");
      dbgPrintln(response);
    } else {
      dbgPrint("Warning: Mode config response: ");
      dbgPrintln(response);
    }
  } else {
    dbgPrintln("Error: Notecard mode config timeout");
  }

  dbgPrint("Notecard configured for ProductUID: ");
  dbgPrintln(kProductUid);
  delay(2000);

  armNotecardAttn();

  dbgPrintln("===Starting main loop===");
  delay(2000);
}

// ===================================================================
//  MAIN LOOP
// ===================================================================
void loop() {
  const unsigned long nowMs = millis();

  // 1) Handle inbound Notecard note notifications (ATTN interrupt triggered).
  if (attnTriggered && !isSyncing) {
    attnTriggered = false;
    isSyncing = true;
    syncStartMs = nowMs;
    
    dbgPrintln();
    dbgPrintln("-- Notecard ATTN HIGH: polling inbound note --");

    // Initiate sync (non-blocking)
    notecardUart.println("{\"req\":\"hub.sync\"}");
  }

  // Handle sync completion with timeout
  if (isSyncing && (nowMs - syncStartMs >= kSyncTimeoutMs)) {
    isSyncing = false;
    
    // Clear any remaining data
    while (notecardUart.available()) {
      notecardUart.read();
    }

    char getNoteCmd[300];
    snprintf(getNoteCmd, sizeof(getNoteCmd),
             "{\"req\":\"note.get\",\"file\":\"%s\",\"delete\":true}",
             kInboundNotefile);
    notecardUart.println(getNoteCmd);

    char noteContent[512];
    if (readNotecardResponse(noteContent, sizeof(noteContent), 2000)) {
      dbgPrint(">> Note Received: ");
      dbgPrintln(noteContent);
    } else {
      dbgPrintln(">> No note or timeout reading note");
    }

    dbgPrintln("Re-arming Notecard ATTN...");
    armNotecardAttn();
  }

  // 2) Optional: read inbound bytes from HM-10 UART (useful for debugging AT mode).
  if (bleUart.available()) {
    char line[128];
    size_t bytesRead = 0;
    
    while (bleUart.available() && bytesRead < sizeof(line) - 1) {
      char c = bleUart.read();
      if (c == '\n') break;
      if (c != '\r') {
        line[bytesRead++] = c;
      }
    }
    line[bytesRead] = '\0';
    
    if (bytesRead > 0) {
      dbgPrint(">> HM-10 UART: ");
      dbgPrintln(line);
    }
  }

  // 3) Periodic ADC sample -> send to USB (wired) + HM-10 (wireless) + Notecard (cloud).
  if (nowMs - lastSampleMs >= kSamplePeriodMs) {
    lastSampleMs = nowMs;

    const int adc = readAveragedAdc();
    dbgPrint("Raw ADC (averaged): ");
    dbgPrintln(adc);

    // Wired to Android via USB serial
    Serial.print("ADC value:");
    Serial.print(adc);
    Serial.print("\r\n");

    // Wireless to Android via HM-10 (transparent UART over BLE)
    bleUart.print("ADC value:");
    bleUart.print(adc);
    bleUart.print("\r\n");

    // Cloud via Notecard
    char addNoteCmd[200];
    snprintf(addNoteCmd, sizeof(addNoteCmd),
             "{\"req\":\"note.add\",\"file\":\"data.qi\",\"body\":{\"adc\":%d}}",
             adc);
    notecardUart.println(addNoteCmd);
    
    char response[256];
    if (readNotecardResponse(response, sizeof(response), 1000)) {
      if (isValidNotecardResponse(response)) {
        dbgPrintln("ADC value sent to Notehub");
      } else {
        dbgPrint("Warning: Note add response: ");
        dbgPrintln(response);
      }
    }
  }
}
