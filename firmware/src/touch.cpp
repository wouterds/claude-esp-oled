#include "touch.h"

#include <Arduino.h>
#include <Wire.h>

#include "board.h"

namespace {

constexpr int I2C_SDA = 11;
constexpr int I2C_SCL = 10;
constexpr int TP_RESET = 1;
constexpr int TP_INT = 4;
constexpr uint8_t TOUCH = 0x15;
// The finger count, then the first point's coordinates behind it - five bytes
// in one read, because the bus is shared with the battery gauge and a second
// transaction for the low byte of an x is a second thing to go wrong.
constexpr uint8_t REG_FINGERS = 0x02;
constexpr uint8_t REPORT_BYTES = 5;
// How far across the glass counts as having gone somewhere. Measured rather
// than chosen, and far short of what a swipe looks like to the person doing it:
// a finger is sampled once a frame and a frame here is 35ms, so both ends of a
// swipe are over before they are seen and half the glass reports as a third of
// it. Taps come in under ten pixels of jitter, and this sits above them.
constexpr int16_t CROSSED = 22;
// The controller reports at about a hundred hertz while a finger is on the
// glass, so a gap this long with nothing arriving is the finger being gone.
constexpr uint32_t RELEASE_MS = 200;

bool wasDown = false;
bool present = false;
volatile bool reported = false;
uint32_t lastReport = 0;
bool landed = false;
Swipe went = Swipe::None;
int16_t fromX = 0;
int16_t atX = 0;

// Where the finger started against where it ended. Called wherever a finger
// stops being down, which is both when the controller says so and when it goes
// quiet for long enough to have meant it.
void lift() {
  wasDown = false;
  int16_t by = (int16_t)(atX - fromX);
  if (by >= CROSSED) {
    went = Swipe::Right;
  } else if (by <= -CROSSED) {
    went = Swipe::Left;
  }
}

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

void touchStep() {
  landed = false;
  if (!present) {
    return;
  }
  if (!reported) {
    if (wasDown && millis() - lastReport > RELEASE_MS) {
      lift();
    }
    return;
  }
  reported = false;
  lastReport = millis();

  Wire.beginTransmission(TOUCH);
  Wire.write(REG_FINGERS);
  if (Wire.endTransmission(false) != 0) {
    return;
  }
  if (Wire.requestFrom((int)TOUCH, (int)REPORT_BYTES) != REPORT_BYTES) {
    return;
  }

  bool down = Wire.read() > 0;
  uint8_t high = Wire.read();
  uint8_t low = Wire.read();
  Wire.read();
  Wire.read();
  // The top nibble of the high byte is the event, not the coordinate. The panel
  // is mounted upside down and everything drawn on it is turned to match, so a
  // touch is turned the same way or it disagrees with what it is pointing at.
  int16_t x = (int16_t)(SCREEN_W - 1 - (((high & 0x0F) << 8) | low));

  if (down && !wasDown) {
    fromX = x;
    landed = true;
  }
  // The report that says the finger is gone still carries where it was, and it
  // is the furthest along anything gets to see.
  atX = x;
  if (!down && wasDown) {
    lift();
  }
  wasDown = down;
}

bool touchTapped() { return landed; }

Swipe touchSwiped() {
  Swipe was = went;
  went = Swipe::None;
  return was;
}
