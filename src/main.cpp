#include <Arduino.h>

// Printf Definition
#define PRINT_FUNCTION 1 // 1 to Turn Printf On & 0 to turn off

// Macros for debug printing
#if PRINT_FUNCTION
  #define DEBUG_PRINT(x)    Serial.print(x)
  #define DEBUG_PRINTLN(x)  Serial.println(x)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
#endif

// ===================================================================
//  Serial Definitions
// ===================================================================
// 1. Notecard Serial (Standard Serial1)
#define notecardSerial Serial1

// 2. External android Serial (Serial2)
//    RX Pin = A0 (Receives data from the android's TX pin)
//    TX Pin = A3 (Transmits data to the android's RX pin)
HardwareSerial Serial2(A0, A3);
#define androidSerial Serial2

// Pin Definitions
#define ANALOG_INPUT_PIN PA1 // Pin for android analogue voltage reading
#define ATTN_PIN D5 

// Notecard Variables
#define PRODUCT_UID "com.gmail.amin.tazrian1979:binheightv2" // <<<<<<<<<<<<<<<<< CHANGE ACCORDINGLY
#define INBOUND_NOTEFILE "data.qi"

// Timing variables
unsigned long previousPrintMillis = 0;

// ===================================================================
//  SETUP
// ===================================================================
void setup() {
    Serial.begin(9600);           // USB Debugging
    notecardSerial.begin(9600);   // Notecard Communication
    androidSerial.begin(9600);     // External UART android Communication

    // Timeouts to prevent blocking forever
    notecardSerial.setTimeout(5000);
    androidSerial.setTimeout(1000);

    pinMode(ANALOG_INPUT_PIN, INPUT_ANALOG);
    analogReadResolution(12);
    pinMode(ATTN_PIN, INPUT);

    delay(3000);

    // Configure Notecard with the constant ProductUID
    notecardSerial.println("{\"req\":\"hub.set\",\"product\":\"" PRODUCT_UID "\"}");
    delay(1000);
    notecardSerial.println("{\"req\":\"hub.set\",\"mode\":\"continuous\",\"sync\":true}");
    delay(3000);
    
    DEBUG_PRINT("Notecard configured for ProductUID: ");
    DEBUG_PRINTLN(PRODUCT_UID);
    delay(5000);
    notecardSerial.println("{\"req\":\"card.attn\",\"mode\":\"arm,files\",\"files\":[\"data.qi\"]}");

    DEBUG_PRINT("===Starting main loop===\n");
    delay(3000);
}

// ===================================================================
//  MAIN LOOP
// ===================================================================
void loop() {
    // Track time for non-blocking functions
    unsigned long currentMillis = millis();

    // ---------------------------------------------------------------
    // 1. Check for inbound commands from Notecard via ATTN pin
    // ---------------------------------------------------------------
    if (digitalRead(ATTN_PIN) == HIGH) {
        DEBUG_PRINTLN("\n-- ATTN is HIGH! Event detected");
        DEBUG_PRINTLN("-- Polling for Notes --");
        
        // Step 1: Sync with Notehub
        notecardSerial.println("{\"req\":\"hub.sync\"}");
        notecardSerial.readStringUntil('\n'); // Clear the immediate {} response
        delay(5000); // Wait for sync to complete

        // Flush any old data from the serial buffer before making a new request
        while(notecardSerial.available()) {
          notecardSerial.read();
        }

        // Step 2: Get the note and delete it
        char getNoteCmd[300];
        snprintf(getNoteCmd, sizeof(getNoteCmd), "{\"req\":\"note.get\",\"file\":\"%s\",\"delete\":true}", INBOUND_NOTEFILE);
        notecardSerial.println(getNoteCmd);

        // Read the actual note content
        String noteContent = notecardSerial.readStringUntil('\n');
        DEBUG_PRINT(">> Note Received: ");
        DEBUG_PRINTLN(noteContent);

        // CRITICAL STEP: Re-arm the ATTN pin for the next event.
        DEBUG_PRINTLN("Re-arming ATTN pin...");
        notecardSerial.println("{\"req\":\"card.attn\",\"mode\":\"arm,files\",\"files\":[\"data.qi\"]}");
        notecardSerial.readStringUntil('\n'); // Clear the response
        delay(3000);
    }

    // ---------------------------------------------------------------
    // 2. Read incoming data from the External UART android
    // ---------------------------------------------------------------
    if (androidSerial.available()) {
        String androidData = androidSerial.readStringUntil('\n');
        
        // Clean up the string (optional, removes trailing \r)
        androidData.trim(); 
        
        if (androidData.length() > 0) {
            DEBUG_PRINT(">> Android UART Data: ");
            DEBUG_PRINTLN(androidData);
        }
    }

    // ---------------------------------------------------------------
    // 3. Formatted print statement with a 5s delay
    // ---------------------------------------------------------------
    if (currentMillis - previousPrintMillis >= 5000) {
        previousPrintMillis = currentMillis;
        
        int rawA1 = analogRead(ANALOG_INPUT_PIN);
        DEBUG_PRINT("Raw A1: ");
        DEBUG_PRINTLN(rawA1);

        DEBUG_PRINTLN("Sending ADC value to Android");
        androidSerial.print("ADC value:");
        androidSerial.print(rawA1);

        // Send ADC value to Notehub via Notecard
        char addNoteCmd[200];
        snprintf(addNoteCmd, sizeof(addNoteCmd), "{\"req\":\"note.add\",\"file\":\"data.qi\",\"body\":{\"adc\":%d}}", rawA1);
        notecardSerial.println(addNoteCmd);
        DEBUG_PRINTLN("ADC value sent to Notehub");
    }
}
