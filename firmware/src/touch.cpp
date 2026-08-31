#include "touch.h"

#include <Arduino.h>

#include "board.h"
#include "pins.h"
#include "touchpad.h"

namespace {

// The finger count, then the first point's coordinates behind it - five bytes
// in one read, because the bus is shared and a second transaction for the low
// byte of an x is a second thing to go wrong.
constexpr uint8_t REG_FINGERS = 0x02;
constexpr uint8_t REPORT_BYTES = 5;
// How far along the glass a finger has to get. Now that every report is seen
// this is very nearly the distance the finger actually travelled, so it can be
// what it sounds like - a deliberate inch - rather than the fraction of one
// that used to survive being sampled once a frame.
constexpr int16_t CROSSED = 30;
// The controller reports at about a hundred hertz while a finger is on the
// glass, so a gap this long with nothing arriving is the finger being gone.
constexpr uint32_t RELEASE_MS = 200;
// How long to wait on the interrupt before looking up to check for that. The
// controller does not always say when a finger leaves, and waiting for it to is
// waiting for ever.
constexpr uint32_t WAKE_MS = 50;

bool present = false;
TaskHandle_t reader = nullptr;

// All of this belongs to the reader task and is only ever touched there.
bool down = false;
bool fired = false;
int16_t fromAlong = 0;
uint32_t lastReport = 0;

volatile Swipe went = Swipe::None;

void IRAM_ATTR onReport() {
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(reader, &woken);
  portYIELD_FROM_ISR(woken);
}

void lift() {
  // A swipe is reported the moment it gets far enough rather than when the
  // finger comes up, so there is nothing left to decide here - only the state
  // for the next finger to put back.
  down = false;
  fired = false;
}

void look() {
  bool touching = false;
  int16_t along = 0;
  if (!touchpadRead(&touching, &along)) {
    return;
  }

  lastReport = millis();
  if (!touching) {
    if (down) {
      lift();
    }
    return;
  }
  if (!down) {
    down = true;
    fired = false;
    fromAlong = along;
    return;
  }
  if (fired) {
    return;
  }

  // Reported on the way past rather than on the way off: the page turns under
  // the finger that asked for it, which is both what a swipe feels like
  // everywhere else and one less thing to depend on the controller mentioning.
  int16_t by = (int16_t)(along - fromAlong);
  if (by >= CROSSED) {
    went = Swipe::Down;
    fired = true;
  } else if (by <= -CROSSED) {
    went = Swipe::Up;
    fired = true;
  }
}

// Woken by the controller rather than by the frame. The glass reports about
// three times for every frame drawn, and a swipe is over in a handful of them -
// read on the frame, most of one never happened.
void task(void *) {
  for (;;) {
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WAKE_MS)) == 0) {
      if (down && millis() - lastReport > RELEASE_MS) {
        lift();
      }
      continue;
    }
    look();
  }
}

}  // namespace

void touchBegin() {
  present = touchpadBegin();
  if (!present) {
    return;
  }

  // Above the radio's own work on that core: a report that arrives while
  // something else is talking to the network is a report about a finger that
  // has since moved. The task exists before the interrupt that notifies it.
  xTaskCreatePinnedToCore(task, "touch", 3072, nullptr, 2, &reader, 0);

  // It sleeps on its own the moment the glass is left alone, and asleep it does
  // not acknowledge its address at all - so asking it on a timer is a read that
  // fails whenever nothing is happening. The interrupt is the part that stays
  // awake: it pulses for each report and is the only thing worth waiting on.
  pinMode(TOUCH_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TOUCH_INT), onReport, FALLING);
}

Swipe touchSwiped() {
  Swipe was = went;
  went = Swipe::None;
  return was;
}
