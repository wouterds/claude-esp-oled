#include "battery.h"

#include <Arduino.h>
#include <Wire.h>

namespace {

constexpr int I2C_SDA = 11;
constexpr int I2C_SCL = 10;
constexpr uint8_t GAUGE = 0x55;
constexpr uint8_t REG_VOLTAGE = 0x08;
constexpr uint8_t REG_CHARGE = 0x2C;

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

}  // namespace

void batteryReport() {
  Wire.begin(I2C_SDA, I2C_SCL);

  uint16_t millivolts = 0;
  uint16_t percent = 0;
  if (!readWord(REG_VOLTAGE, millivolts)) {
    Serial.println("battery: no gauge on the bus");
    return;
  }
  readWord(REG_CHARGE, percent);

  // A gauge with nothing on its terminals still answers; it just reads flat.
  if (millivolts < 2500) {
    Serial.printf("battery: none attached or flat (%u mV)\n", millivolts);
    return;
  }
  Serial.printf("battery: %u mV, %u%%\n", millivolts, percent);
}
