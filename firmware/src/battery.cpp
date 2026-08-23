#include "battery.h"

#include <Arduino.h>
#include <Wire.h>

#include "bus.h"

namespace {

constexpr uint8_t GAUGE = 0x55;
constexpr uint8_t REG_VOLTAGE = 0x08;
constexpr uint8_t REG_CURRENT = 0x0C;
constexpr uint8_t REG_FLAGS = 0x0A;
// Bit zero of the flags word: the gauge has decided the pack is discharging.
// It is the gauge's own answer to the question, rather than a threshold guessed
// at from a current reading that is zero for the first second after a boot and
// zero again whenever a full pack sits on a charger.
constexpr uint16_t FLAG_DISCHARGING = 0x0001;
constexpr uint8_t REG_CHARGE = 0x2C;
// Often enough that plugging the cable in changes the icon while your hand is
// still on it. Three word reads is nothing now that the bus is quiet.
constexpr uint32_t EVERY_MS = 500;
// Not discharging is the useful question, not whether charge is going in. A
// full pack on a charger draws nothing and would read as running on battery,
// which is the one time you most want to see that it is plugged in. On battery
// the board pulls a hundred milliamps and more, so the two are never close.
constexpr int16_t DISCHARGING_MA = -20;

BatteryState cached = {0, 0, false, false};
uint32_t nextRead = 0;

// Every register on this part is a little-endian pair, and a read is a write of
// the address followed by a read without releasing the bus.
bool readWord(uint8_t reg, uint16_t &out) {
  busTake();
  Wire.beginTransmission(GAUGE);
  Wire.write(reg);
  bool got = Wire.endTransmission(false) == 0 && Wire.requestFrom((int)GAUGE, 2) == 2;
  if (got) {
    uint8_t low = Wire.read();
    uint8_t high = Wire.read();
    out = (uint16_t)((high << 8) | low);
  }
  busGive();
  return got;
}

void sample() {
  uint16_t millivolts = 0;
  if (!readWord(REG_VOLTAGE, millivolts)) {
    cached = {0, 0, false, false};
    return;
  }

  uint16_t percent = 0;
  uint16_t flags = 0;
  readWord(REG_CHARGE, percent);
  bool told = readWord(REG_FLAGS, flags);

  cached.millivolts = millivolts;
  cached.percent = percent > 100 ? 100 : (uint8_t)percent;
  if (told) {
    cached.charging = (flags & FLAG_DISCHARGING) == 0;
  }
  // A gauge with nothing on its terminals still answers; it just reads flat.
  cached.present = millivolts >= 2500;
}

}  // namespace

void batteryBegin() {
  sample();
  nextRead = millis() + EVERY_MS;
  if (!cached.present) {
    Serial.printf("battery: none attached or flat (%u mV)\n", cached.millivolts);
    return;
  }
  uint16_t current = 0;
  uint16_t flags = 0;
  readWord(REG_CURRENT, current);
  readWord(REG_FLAGS, flags);
  Serial.printf("battery: %u mV, %u%%, %s (flags %04x, %d mA)\n", cached.millivolts,
                cached.percent, cached.charging ? "external power" : "on battery", flags,
                (int)(int16_t)current);
}

BatteryState batteryRead() {
  uint32_t now = millis();
  if ((int32_t)(now - nextRead) >= 0) {
    nextRead = now + EVERY_MS;
    sample();
  }
  return cached;
}
