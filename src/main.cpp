#include <Arduino.h>
#include <Notecard.h>

int getSensorInterval();
float getRandomHeight();
String getCurrentTimestamp();
String buildHeightJSON(float height, String timestamp);

#define usbSerial Serial
#define productUID "com.gmail.amin.tazrian1979:bin_height_measurement_firmware"

Notecard notecard;

void setup()
{
  // This setup code run once:
  delay(2500);
  usbSerial.begin(115200);

  notecard.begin();
  {
    J *req = notecard.newRequest("hub.set");
    if (req != NULL)
    {
      JAddStringToObject(req, "product", productUID);
      JAddStringToObject(req, "mode", "continuous");
      JAddNumberToObject(req, "inbound", 5);
      notecard.sendRequest(req);
    }
  }
}

void loop()
{
  // This main code runs repeatedly:
  float height = getRandomHeight();
  String timestamp = getCurrentTimestamp();

  // Send height data to Android app via USB-Serial as JSON
  String jsonOutput = buildHeightJSON(height, timestamp);
  usbSerial.println(jsonOutput);

  // Send height data to Notecard for cloud synchronization
  {
    J *req = notecard.newRequest("note.add");
    if (req != NULL)
    {
      JAddStringToObject(req, "file", "sensors.qo");
      JAddBoolToObject(req, "sync", true);
      J *body = JAddObjectToObject(req, "body");
      if (body)
      {
        JAddNumberToObject(body, "height", height);
      }
      notecard.sendRequest(req);
    }
  }

  int sensorIntervalSeconds = getSensorInterval();
  usbSerial.print("Delaying ");
  usbSerial.print(sensorIntervalSeconds);
  usbSerial.println(" seconds");
  delay(sensorIntervalSeconds * 1000);
}

// This function assumes you’ll set the reading_interval environment variable to
// a positive integer. If the variable is not set, set to 0, or set to an invalid
// type, this function returns a default value of 60.
int getSensorInterval()
{
  int sensorIntervalSeconds = 60;
  J *req = notecard.newRequest("env.get");
  if (req != NULL)
  {
    JAddStringToObject(req, "name", "reading_interval");
    J *rsp = notecard.requestAndResponse(req);
    int readingIntervalEnvVar = atoi(JGetString(rsp, "text"));
    if (readingIntervalEnvVar > 0)
    {
      sensorIntervalSeconds = readingIntervalEnvVar;
    }
    notecard.deleteResponse(rsp);
  }
  return sensorIntervalSeconds;
}
// Generate random height in range 0-1000 cm
float getRandomHeight()
{
  return random(0, 100001) / 100.0; // Returns value between 0.00 and 1000.00
}

// Get current timestamp placeholder using boot time.
// Notecard time.status is unsupported on this firmware, so we use elapsed milliseconds.
String getCurrentTimestamp()
{
  unsigned long ms = millis();
  String timestamp = String(ms);
  return timestamp;
}

// Build JSON string with height and timestamp for Android app
String buildHeightJSON(float height, String timestamp)
{
  // Format: {"height": 123.45, "timestamp": "2026-04-15T10:30:00Z"}
  String json = "{\"height\": ";
  json += String(height, 2); // 2 decimal places
  json += ", \"timestamp\": \"";
  json += timestamp;
  json += "\"}";
  return json;
}