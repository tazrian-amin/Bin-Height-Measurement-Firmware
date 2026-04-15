#include <Arduino.h>
#include <Notecard.h>

#define usbSerial Serial
#define productUID "com.gmail.amin.tazrian1979:bin_height_measurement_firmware"

Notecard notecard;

void setup()
{
  // put your setup code here, to run once:
  delay(2500);
  usbSerial.begin(115200);

  notecard.begin();
  notecard.setDebugOutputStream(usbSerial);
  {
    J *req = notecard.newRequest("hub.set");
    if (req != NULL)
    {
      JAddStringToObject(req, "product", productUID);
      JAddStringToObject(req, "mode", "continuous");
      notecard.sendRequest(req);
    }
  }
}

void loop()
{
  // put your main code here, to run repeatedly:
}
