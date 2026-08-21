#include "touch.h"

#include <Arduino.h>
#include <Wire.h>

namespace {

constexpr int I2C_SDA = 11;
constexpr int I2C_SCL = 10;
constexpr int TP_RESET = 1;
constexpr uint8_t TOUCH = 0x15;
constexpr uint8_t REG_FINGERS = 0x02;

bool wasDown = false;
bool present = false;

}  // namespace

void touchBegin() {
  Wire.begin(I2C_SDA, I2C_SCL);

  // The controller is held in reset out of power-on and answers nothing until
  // it is let go, which looks the same as it not being there.
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(10);
  digitalWrite(TP_RESET, HIGH);
  delay(60);

  Wire.beginTransmission(TOUCH);
  present = Wire.endTransmission() == 0;
  Serial.printf("touch: %s\n", present ? "CST816S ready" : "not answering");
}

bool touchTapped() {
  if (!present) {
    return false;
  }
  Wire.beginTransmission(TOUCH);
  Wire.write(REG_FINGERS);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom((int)TOUCH, 1) != 1) {
    return false;
  }

  bool down = Wire.read() > 0;
  // The landing, not the holding: a finger left on the glass is one tap.
  bool landed = down && !wasDown;
  wasDown = down;
  return landed;
}
