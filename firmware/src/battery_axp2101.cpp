#include "battery.h"

#include <Arduino.h>
#include <Wire.h>

#include "bus.h"

namespace {

// This board carries no fuel gauge. The pack hangs off an AXP2101 PMIC, which
// runs the charger and keeps a gauge of its own, so the same three questions
// get asked of a different part rather than going unanswered.
constexpr uint8_t PMIC = 0x34;

constexpr uint8_t REG_STATUS1 = 0x00;
constexpr uint8_t REG_STATUS2 = 0x01;
constexpr uint8_t REG_ADC_ENABLE = 0x30;
// Thirteen bits of millivolts over two registers, the high five in the first.
constexpr uint8_t REG_VOLTAGE = 0x34;
constexpr uint8_t REG_BAT_DETECT = 0x68;
constexpr uint8_t REG_PERCENT = 0xA4;

// Bit three of the first status word: there is a pack on the terminals.
constexpr uint8_t BIT_BATTERY = 0x08;
// Bit zero of each control register, which is the channel and the detector.
constexpr uint8_t BIT_ENABLE = 0x01;
// The top three bits of the second status word say what the charger is doing,
// and two of them is discharging. Whether it is *not* that is the useful
// question rather than whether charge is going in: a full pack on a charger
// takes nothing and would otherwise read as running on battery, which is the
// one time you most want to see that it is plugged in.
constexpr uint8_t CHARGE_SHIFT = 5;
constexpr uint8_t CHARGE_DISCHARGING = 0x02;

// Often enough that plugging the cable in changes the icon while your hand is
// still on it.
constexpr uint32_t EVERY_MS = 500;

BatteryState cached = {0, 0, false, false};
uint32_t nextRead = 0;
bool wired = false;

bool readBytes(uint8_t reg, uint8_t *out, uint8_t len) {
  busTake();
  Wire.beginTransmission(PMIC);
  Wire.write(reg);
  bool got = Wire.endTransmission(false) == 0 && Wire.requestFrom((int)PMIC, (int)len) == len;
  if (got) {
    for (uint8_t i = 0; i < len; i++) {
      out[i] = Wire.read();
    }
  }
  busGive();
  return got;
}

bool readByte(uint8_t reg, uint8_t &out) { return readBytes(reg, &out, 1); }

// Read, set, write back. Every bit in these registers but the one being changed
// belongs to something else the PMIC is already doing.
void setBit(uint8_t reg, uint8_t mask) {
  uint8_t was = 0;
  if (!readByte(reg, was) || (was & mask) != 0) {
    return;
  }
  busTake();
  Wire.beginTransmission(PMIC);
  Wire.write(reg);
  Wire.write((uint8_t)(was | mask));
  Wire.endTransmission(true);
  busGive();
}

void sample() {
  uint8_t status1 = 0;
  if (!readByte(REG_STATUS1, status1)) {
    cached = {0, 0, false, false};
    return;
  }
  cached.present = (status1 & BIT_BATTERY) != 0;
  if (!cached.present) {
    cached.millivolts = 0;
    cached.percent = 0;
    return;
  }

  uint8_t pair[2] = {0, 0};
  if (readBytes(REG_VOLTAGE, pair, sizeof(pair))) {
    cached.millivolts = (uint16_t)(((pair[0] & 0x1F) << 8) | pair[1]);
  }
  uint8_t percent = 0;
  if (readByte(REG_PERCENT, percent)) {
    cached.percent = percent > 100 ? 100 : percent;
  }
  uint8_t status2 = 0;
  if (readByte(REG_STATUS2, status2)) {
    cached.charging = (uint8_t)(status2 >> CHARGE_SHIFT) != CHARGE_DISCHARGING;
  }
}

}  // namespace

void batteryBegin() {
  busTake();
  Wire.beginTransmission(PMIC);
  wired = Wire.endTransmission() == 0;
  busGive();
  if (!wired) {
    Serial.println("battery: the PMIC is not answering");
    return;
  }

  // Neither is on out of reset, and both are silent when off: the voltage reads
  // as a flat zero and the pack reads as absent, which looks exactly like a
  // board with no battery in it.
  setBit(REG_BAT_DETECT, BIT_ENABLE);
  setBit(REG_ADC_ENABLE, BIT_ENABLE);

  sample();
  nextRead = millis() + EVERY_MS;
  if (!cached.present) {
    Serial.println("battery: none attached");
    return;
  }
  Serial.printf("battery: %u mV, %u%%, %s\n", cached.millivolts, cached.percent,
                cached.charging ? "external power" : "on battery");
}

BatteryState batteryRead() {
  if (!wired) {
    return cached;
  }
  uint32_t now = millis();
  if ((int32_t)(now - nextRead) >= 0) {
    nextRead = now + EVERY_MS;
    sample();
  }
  return cached;
}
