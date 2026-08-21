#include "battery.h"

#include <Arduino.h>
#include <Wire.h>

namespace {

constexpr int I2C_SDA = 11;
constexpr int I2C_SCL = 10;
constexpr uint8_t GAUGE = 0x55;
constexpr uint8_t REG_VOLTAGE = 0x08;
constexpr uint8_t REG_CURRENT = 0x0C;
constexpr uint8_t REG_CHARGE = 0x2C;
constexpr uint32_t EVERY_MS = 2000;
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
  Wire.beginTransmission(GAUGE);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom((int)GAUGE, 2) != 2) {
    return false;
  }
  uint8_t low = Wire.read();
  uint8_t high = Wire.read();
  out = (uint16_t)((high << 8) | low);
  return true;
}

void sample() {
  uint16_t millivolts = 0;
  if (!readWord(REG_VOLTAGE, millivolts)) {
    cached = {0, 0, false, false};
    return;
  }

  uint16_t percent = 0;
  uint16_t current = 0;
  readWord(REG_CHARGE, percent);
  readWord(REG_CURRENT, current);

  cached.millivolts = millivolts;
  cached.percent = percent > 100 ? 100 : (uint8_t)percent;
  // Current is signed, and out of the pack is negative.
  cached.charging = (int16_t)current > DISCHARGING_MA;
  // A gauge with nothing on its terminals still answers; it just reads flat.
  cached.present = millivolts >= 2500;
}

}  // namespace

void batteryBegin() {
  Wire.begin(I2C_SDA, I2C_SCL);
  sample();
  nextRead = millis() + EVERY_MS;
  if (!cached.present) {
    Serial.printf("battery: none attached or flat (%u mV)\n", cached.millivolts);
    return;
  }
  Serial.printf("battery: %u mV, %u%%, %s\n", cached.millivolts, cached.percent,
                cached.charging ? "external power" : "on battery");
}

BatteryState batteryRead() {
  uint32_t now = millis();
  if ((int32_t)(now - nextRead) >= 0) {
    nextRead = now + EVERY_MS;
    sample();
  }
  return cached;
}
