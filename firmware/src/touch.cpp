#include "touch.h"

#include <Arduino.h>
#include <Wire.h>

#include "board.h"
#include "bus.h"

namespace {

constexpr int TP_RESET = 1;
constexpr int TP_INT = 4;
constexpr uint8_t TOUCH = 0x15;
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
int16_t from = 0;
uint32_t lastReport = 0;

volatile bool tapped = false;
volatile Swipe went = Swipe::None;

void IRAM_ATTR onReport() {
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(reader, &woken);
  portYIELD_FROM_ISR(woken);
}

void lift() {
  // A finger that went nowhere was a tap. One that went somewhere was a swipe,
  // and that was reported the moment it got far enough - so it is not also a
  // tap, however it ends.
  if (!fired) {
    tapped = true;
  }
  down = false;
  fired = false;
}

void look() {
  uint8_t fingers = 0;
  uint8_t high = 0;
  uint8_t low = 0;

  busTake();
  Wire.beginTransmission(TOUCH);
  Wire.write(REG_FINGERS);
  bool got = Wire.endTransmission(false) == 0 &&
             Wire.requestFrom((int)TOUCH, (int)REPORT_BYTES) == REPORT_BYTES;
  if (got) {
    fingers = Wire.read();
    high = Wire.read();
    low = Wire.read();
    Wire.read();
    Wire.read();
  }
  busGive();
  if (!got) {
    return;
  }

  lastReport = millis();
  // The top nibble of the high byte is the event, not the coordinate. What is
  // left is the axis the controller calls X, which on this mounting runs down
  // the glass rather than across it - so it is read as a distance from the top,
  // turned the way everything drawn here is turned.
  int16_t along = (int16_t)(SCREEN_W - 1 - (((high & 0x0F) << 8) | low));

  if (fingers == 0) {
    if (down) {
      lift();
    }
    return;
  }
  if (!down) {
    down = true;
    fired = false;
    from = along;
    return;
  }
  if (fired) {
    return;
  }

  // Reported on the way past rather than on the way off: the page turns under
  // the finger that asked for it, which is both what a swipe feels like
  // everywhere else and one less thing to depend on the controller mentioning.
  int16_t by = (int16_t)(along - from);
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
  // The controller is held in reset out of power-on and answers nothing until
  // it is let go, which looks the same as it not being there.
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(10);
  digitalWrite(TP_RESET, HIGH);
  delay(60);

  busTake();
  Wire.beginTransmission(TOUCH);
  present = Wire.endTransmission() == 0;
  busGive();
  Serial.printf("touch: %s\n", present ? "CST816S ready" : "not answering");
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
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), onReport, FALLING);
}

bool touchTapped() {
  if (!tapped) {
    return false;
  }
  tapped = false;
  return true;
}

Swipe touchSwiped() {
  Swipe was = went;
  went = Swipe::None;
  return was;
}
