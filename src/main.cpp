#include <Arduino.h>
#include <Notecard.h>
#include <NotecardPseudoSensor.h>

using namespace blues;

#define usbSerial Serial
#define productUID "com.gmail.amin.tazrian1979:bin_height_measurement_firmware"

Notecard notecard;
NotecardPseudoSensor sensor(notecard);

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
  float temperature = sensor.temp();
  float humidity = sensor.humidity();

  usbSerial.print("Temperature = ");
  usbSerial.print(temperature);
  usbSerial.println(" *C");
  usbSerial.print("Humidity = ");
  usbSerial.print(humidity);
  usbSerial.println(" %");

  delay(15000);
}
