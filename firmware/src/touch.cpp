#include "touch.h"

#include <Arduino.h>

#include "pins.h"
#include "touchpad.h"

namespace {

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
// How far a finger may wobble between landing and lifting and still have
// tapped rather than dragged.
constexpr int16_t SLOP = 20;
// From the first tap lifting to the second landing. Longer apart is two taps.
constexpr uint32_t DOUBLE_MS = 400;

bool present = false;
TaskHandle_t reader = nullptr;

// All of this belongs to the reader task and is only ever touched there.
bool down = false;
bool fired = false;
int16_t fromAlong = 0;
int16_t fromAcross = 0;
int16_t lastAlong = 0;
int16_t lastAcross = 0;
uint32_t downAt = 0;
uint32_t lastReport = 0;
// The first of a pair, kept until the second lands or the window shuts.
bool tapped = false;
uint32_t tapUpAt = 0;
int16_t tapAlong = 0;
int16_t tapAcross = 0;

volatile Swipe went = Swipe::None;
volatile bool doubled = false;
volatile int16_t doubledAlong = 0;
volatile int16_t doubledAcross = 0;
volatile bool singled = false;
volatile int16_t singledAlong = 0;
volatile int16_t singledAcross = 0;
volatile bool fingerDown = false;
volatile int16_t fingerAcross = 0;
volatile int16_t fingerAlong = 0;

void IRAM_ATTR onReport() {
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(reader, &woken);
  portYIELD_FROM_ISR(woken);
}

void lift() {
  // A swipe is reported the moment it gets far enough rather than when the
  // finger comes up. A tap is the opposite: a finger that came up having gone
  // nowhere, and it is only here that that is known.
  bool tap = !fired && abs(lastAlong - fromAlong) <= SLOP && abs(lastAcross - fromAcross) <= SLOP;
  if (tap && tapped && downAt - tapUpAt <= DOUBLE_MS && abs(fromAlong - tapAlong) <= 2 * SLOP &&
      abs(fromAcross - tapAcross) <= 2 * SLOP) {
    doubledAlong = tapAlong;
    doubledAcross = tapAcross;
    doubled = true;
    tapped = false;
  } else {
    tapped = tap;
    // When the finger was last seen rather than now: a controller that never
    // said the finger left is only found out RELEASE_MS later.
    tapUpAt = lastReport;
    tapAlong = fromAlong;
    tapAcross = fromAcross;
  }
  down = false;
  fired = false;
  fingerDown = false;
}

void look() {
  bool touching = false;
  int16_t along = 0;
  int16_t across = 0;
  if (!touchpadRead(&touching, &along, &across)) {
    return;
  }

  lastReport = millis();
  fingerAcross = across;
  fingerAlong = along;
  fingerDown = touching;
  if (!touching) {
    if (down) {
      lift();
    }
    return;
  }
  lastAlong = along;
  lastAcross = across;
  if (!down) {
    down = true;
    fired = false;
    fromAlong = along;
    fromAcross = across;
    downAt = lastReport;
    return;
  }
  if (fired) {
    return;
  }

  // Reported on the way past rather than on the way off: the page turns under
  // the finger that asked for it, which is both what a swipe feels like
  // everywhere else and one less thing to depend on the controller mentioning.
  //
  // Whichever axis it has gone furthest along wins, so a finger dragged at an
  // angle turns one page rather than both at once. At exactly forty-five
  // degrees the vertical has it, which is arbitrary and only has to be settled.
  int16_t down = (int16_t)(along - fromAlong);
  int16_t right = (int16_t)(across - fromAcross);
  if (abs(down) >= abs(right)) {
    if (down >= CROSSED) {
      went = Swipe::Down;
      fired = true;
    } else if (down <= -CROSSED) {
      went = Swipe::Up;
      fired = true;
    }
    return;
  }
  if (right >= CROSSED) {
    went = Swipe::Right;
    fired = true;
  } else if (right <= -CROSSED) {
    went = Swipe::Left;
    fired = true;
  }
}

// Woken by the controller rather than by the frame. The glass reports about
// three times for every frame drawn, and a swipe is over in a handful of them -
// read on the frame, most of one never happened.
void task(void *) {
  for (;;) {
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WAKE_MS)) == 0) {
      // A tap on its own is only known to be on its own once the window for a
      // second one has shut - fired on the way up, every double tap would ring
      // the single first, and the two mean different things here.
      if (tapped && millis() - tapUpAt > DOUBLE_MS) {
        tapped = false;
        singledAlong = tapAlong;
        singledAcross = tapAcross;
        singled = true;
      }
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

bool touchFinger(int16_t *across, int16_t *along) {
  *across = fingerAcross;
  *along = fingerAlong;
  return fingerDown;
}

bool touchDoubleTapped(int16_t *across, int16_t *along) {
  if (!doubled) {
    return false;
  }
  *across = doubledAcross;
  *along = doubledAlong;
  doubled = false;
  return true;
}

bool touchTapped(int16_t *across, int16_t *along) {
  if (!singled) {
    return false;
  }
  *across = singledAcross;
  *along = singledAlong;
  singled = false;
  return true;
}
