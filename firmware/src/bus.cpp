#include "bus.h"

#include <Arduino.h>
#include <Wire.h>

#include "pins.h"

namespace {

SemaphoreHandle_t held = nullptr;

}  // namespace

void busBegin() {
  held = xSemaphoreCreateMutex();
  Wire.begin(I2C_SDA, I2C_SCL);
}

void busTake() {
  if (held) {
    xSemaphoreTake(held, portMAX_DELAY);
  }
}

void busGive() {
  if (held) {
    xSemaphoreGive(held);
  }
}
