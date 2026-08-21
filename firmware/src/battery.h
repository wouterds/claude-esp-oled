#pragma once

#include <stdint.h>

struct BatteryState {
  uint16_t millivolts;
  uint8_t percent;
  bool charging;
  bool present;
};

void batteryBegin();

// Cached. The gauge is on the same I2C bus as the touch controller and neither
// of them needs asking thirty times a second.
BatteryState batteryRead();
