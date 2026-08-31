#include <Arduino.h>
#include <Wire.h>

#include "bus.h"
#include "pins.h"
#include "touchpad.h"

namespace {

// The finger count, then the first point's coordinates behind it - five bytes
// in one read, because the bus is shared and a second transaction for the low
// byte of an x is a second thing to go wrong.
constexpr uint8_t REG_FINGERS = 0x02;
constexpr uint8_t REPORT_BYTES = 5;

}  // namespace

bool touchpadBegin() {
  // The controller is held in reset out of power-on and answers nothing until
  // it is let go, which looks the same as it not being there.
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(60);

  busTake();
  Wire.beginTransmission(TOUCH_ADDR);
  bool present = Wire.endTransmission() == 0;
  busGive();
  Serial.printf("touch: %s\n", present ? "CST816S ready" : "not answering");
  return present;
}

bool touchpadRead(bool *down, int16_t *along) {
  uint8_t fingers = 0;
  uint8_t high = 0;
  uint8_t low = 0;

  busTake();
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(REG_FINGERS);
  bool got = Wire.endTransmission(false) == 0 &&
             Wire.requestFrom((int)TOUCH_ADDR, (int)REPORT_BYTES) == REPORT_BYTES;
  if (got) {
    fingers = Wire.read();
    high = Wire.read();
    low = Wire.read();
    Wire.read();
    Wire.read();
  }
  busGive();
  if (!got) {
    return false;
  }

  // The top nibble of the high byte is the event, not the coordinate. What is
  // left is the axis the controller calls X, which on this mounting runs down
  // the glass rather than across it - so it is read as a distance from the top,
  // turned the way everything drawn here is turned.
  *down = fingers != 0;
  *along = (int16_t)(SCREEN_W - 1 - (((high & 0x0F) << 8) | low));
  return true;
}
