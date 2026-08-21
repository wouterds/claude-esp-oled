#include "touch.h"

#include <Arduino.h>
#include <Wire.h>

namespace {

constexpr int I2C_SDA = 11;
constexpr int I2C_SCL = 10;
constexpr int TP_RESET = 1;
constexpr int TP_INT = 4;
constexpr uint8_t TOUCH = 0x15;
constexpr uint8_t REG_FINGERS = 0x02;
// The controller reports at about a hundred hertz while a finger is on the
// glass, so a gap this long with nothing arriving is the finger being gone.
constexpr uint32_t RELEASE_MS = 200;

bool wasDown = false;
bool present = false;
volatile bool reported = false;
uint32_t lastReport = 0;

void IRAM_ATTR onReport() { reported = true; }

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

  // It sleeps on its own the moment the glass is left alone, and asleep it
  // does not acknowledge its address at all - so asking it every frame is a
  // read that fails every frame. The interrupt is the part that stays awake:
  // it pulses for each report and is the only thing worth waiting on.
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), onReport, FALLING);
  reported = false;
}

bool touchTapped() {
  if (!present) {
    return false;
  }
  if (!reported) {
    if (wasDown && millis() - lastReport > RELEASE_MS) {
      wasDown = false;
    }
    return false;
  }
  reported = false;
  lastReport = millis();

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
