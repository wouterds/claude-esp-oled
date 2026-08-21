#include <Arduino.h>
#include <esp_system.h>

#include "battery.h"
#include "board.h"
#include "scene.h"

static constexpr uint32_t FRAME_MS = 16;

static uint32_t lastFrame = 0;

// Neither button is wired to anything here. PWR switches the power path and the
// chip cannot see it; BOOT can be read, but it is the strapping pin that traps
// the ROM in the bootloader when it is low at reset, and PWR already does the
// only thing worth asking of a button. What the reset reason is good for is
// telling the two apart: a real power cut comes back POWERON, everything else
// does not.
static const char *why(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "power on";
    case ESP_RST_DEEPSLEEP:
      return "woke from deep sleep";
    case ESP_RST_SW:
      return "software reset";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_BROWNOUT:
      return "brownout";
    default:
      return "other";
  }
}

void setup() {
  Serial.begin(115200);
  Serial.printf("reset: %s\n", why(esp_reset_reason()));
  if (!boardBegin()) {
    while (true) {
      delay(1000);
    }
  }
  batteryReport();
  sceneBegin();
  lastFrame = millis();
}

void loop() {
  uint32_t now = millis();
  float dt = (float)(now - lastFrame) * 0.001f;
  lastFrame = now;
  if (dt > 0.05f) {
    dt = 0.05f;
  }

  sceneStep(dt);
  sceneDraw(boardFramebuffer());
  boardFlush();

  // Sixty is past what the panel or the eye wants; the rest goes back.
  uint32_t spent = millis() - now;
  if (spent < FRAME_MS) {
    delay(FRAME_MS - spent);
  }
}
