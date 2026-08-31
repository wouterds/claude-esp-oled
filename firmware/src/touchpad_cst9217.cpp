#include <Arduino.h>
#include <Wire.h>

#include "bus.h"
#include "pins.h"
#include "touchpad.h"

namespace {

// This part addresses its registers with two bytes rather than one, so a read
// is a two-byte write and then the read - the same shape as the other board's,
// one byte wider.
constexpr uint16_t REG_REPORT = 0xD000;
// One point of five bytes, behind five of header.
constexpr uint8_t REPORT_BYTES = 10;
// The controller writes this into the seventh byte of every report it means.
// Without it a read that arrived early is indistinguishable from a finger at
// the top left corner, which is a tap the user never made.
constexpr uint8_t ACK = 0xAB;
// The status nibble that means this point holds a finger that is down.
constexpr uint8_t TOUCHING = 0x06;

}  // namespace

bool touchpadBegin() {
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(60);

  busTake();
  Wire.beginTransmission(TOUCH_ADDR);
  bool present = Wire.endTransmission() == 0;
  busGive();
  Serial.printf("touch: %s\n", present ? "CST9217 ready" : "not answering");
  return present;
}

bool touchpadRead(bool *down, int16_t *along) {
  uint8_t report[REPORT_BYTES];

  busTake();
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write((uint8_t)(REG_REPORT >> 8));
  Wire.write((uint8_t)(REG_REPORT & 0xFF));
  bool got = Wire.endTransmission(false) == 0 &&
             Wire.requestFrom((int)TOUCH_ADDR, (int)REPORT_BYTES) == REPORT_BYTES;
  if (got) {
    for (uint8_t i = 0; i < REPORT_BYTES; i++) {
      report[i] = Wire.read();
    }
  }
  busGive();
  if (!got || report[6] != ACK) {
    return false;
  }

  if ((report[5] & 0x7F) == 0 || (report[0] & 0x0F) != TOUCHING) {
    *down = false;
    *along = 0;
    return true;
  }

  // Twelve bits each, sharing the byte between them: the high eight of x, the
  // high eight of y, then their low nibbles packed into one.
  *down = true;
  *along = (int16_t)((report[2] << 4) | (report[3] & 0x0F));
  return true;
}
