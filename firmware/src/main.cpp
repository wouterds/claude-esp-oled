#include <Arduino.h>

#include "board.h"
#include "scene.h"

// BOOT is the only button the S3 can see. PWR switches the power path itself
// and is not wired to the chip at all, so nothing here can read it or stand in
// for it. Held low at reset BOOT traps the ROM in the bootloader; once running
// it is an ordinary input with a pull-up, and a short press is safe.
static constexpr int BUTTON = 0;
static constexpr uint32_t DEBOUNCE_MS = 40;
static constexpr uint32_t FRAME_MS = 16;

static bool showing = true;
static bool held = false;
static uint32_t settled = 0;
static uint32_t lastFrame = 0;

static void pollButton(uint32_t now) {
  bool down = digitalRead(BUTTON) == LOW;
  if (down == held || (now - settled) < DEBOUNCE_MS) {
    return;
  }
  settled = now;
  held = down;
  if (!down) {
    return;
  }
  showing = !showing;
  boardDisplay(showing);
  Serial.printf("display: %s\n", showing ? "on" : "off");
}

void setup() {
  Serial.begin(115200);
  if (!boardBegin()) {
    while (true) {
      delay(1000);
    }
  }
  pinMode(BUTTON, INPUT_PULLUP);
  sceneBegin();
  lastFrame = millis();
}

void loop() {
  uint32_t now = millis();
  pollButton(now);
  if (!showing) {
    // Nothing is drawn while it is off, and the clock starts again on the way
    // back so nothing moves the width of the pause in one frame.
    lastFrame = now;
    delay(30);
    return;
  }

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
