#include "button.h"

#include <Arduino.h>

namespace {

// Pulled up on the board, so it reads low while it is held.
constexpr uint8_t PIN = 0;
// Long enough to outlast the contact rattling, short enough that a deliberate
// press is never the one that gets thrown away.
constexpr uint32_t SETTLE_MS = 25;

bool down = false;
bool press = false;
bool armed = false;
uint32_t changed = 0;

}  // namespace

void buttonBegin() {
  // The internal pull-up as well as the board's. The strap has to be high at
  // reset and this pin has no business floating either way.
  pinMode(PIN, INPUT_PULLUP);
  down = digitalRead(PIN) == LOW;
  changed = millis();
  // Held as the board came up, so the release that follows is the end of a
  // press that began before there was anything to tell. Nothing is armed until
  // the pin has been seen up at least once.
  armed = !down;
}

bool buttonPressed() {
  uint32_t now = millis();
  bool raw = digitalRead(PIN) == LOW;
  if (raw != down && (int32_t)(now - changed) >= (int32_t)SETTLE_MS) {
    down = raw;
    changed = now;
    // On the release rather than on the press: a press still being held has not
    // finished saying what it is.
    if (!down) {
      press = armed;
      armed = true;
    }
  }
  bool was = press;
  press = false;
  return was;
}
