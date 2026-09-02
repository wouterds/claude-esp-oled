#include "button.h"

#include <Arduino.h>

#include "pins.h"

namespace {

// Long enough to outlast the contact rattling, short enough that a deliberate
// press is never the one that gets thrown away.
constexpr uint32_t SETTLE_MS = 25;

bool down = false;
bool press = false;
bool armed = false;
// This press has already gone out as a hold, and is spent.
bool held = false;
uint32_t changed = 0;

// Both of the questions below ask this first, so whichever is asked comes to a
// pin that was read this time round rather than last. Reading twice in one pass
// costs a second digitalRead and settles nothing new - the state only moves on
// an edge that has outlasted the rattle.
void sample() {
  uint32_t now = millis();
  bool raw = digitalRead(BUTTON_PIN) == LOW;
  if (raw == down || (int32_t)(now - changed) < (int32_t)SETTLE_MS) {
    return;
  }
  down = raw;
  changed = now;
  if (down) {
    held = false;
    return;
  }
  // On the release rather than on the press: a press still being held has not
  // finished saying what it is.
  press = armed && !held;
  armed = true;
}

}  // namespace

void buttonBegin() {
  // The internal pull-up as well as the board's. The strap has to be high at
  // reset and this pin has no business floating either way.
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  down = digitalRead(BUTTON_PIN) == LOW;
  changed = millis();
  // Held as the board came up, so the release that follows is the end of a
  // press that began before there was anything to tell. Nothing is armed until
  // the pin has been seen up at least once.
  armed = !down;
}

bool buttonPressed() {
  sample();
  bool was = press;
  press = false;
  return was;
}

bool buttonHeld(uint32_t ms) {
  sample();
  // `armed` for the same reason the release checks it: a button already down as
  // the board came up is the tail of a press that started before there was
  // anything to tell, and three seconds of it is not three seconds of anybody
  // asking for something.
  if (!down || held || !armed || (int32_t)(millis() - changed) < (int32_t)ms) {
    return false;
  }
  held = true;
  return true;
}
