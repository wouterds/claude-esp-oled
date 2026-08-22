#include <Arduino.h>
#include <esp_system.h>
#include <string.h>

#include "audio.h"
#include "battery.h"
#include "board.h"
#include "status.h"
#include "touch.h"
#include "wifi.h"
#include "face.h"
#include "gauge.h"
#include "info.h"
#include "portal.h"
#include "usage.h"

static constexpr uint32_t FRAME_MS = 16;

static uint32_t lastFrame = 0;

// Two of them, a swipe apart: the face, and what it is running on. Neither knows
// the other exists - what turns the page is here.
enum class Page : uint8_t { Main, Info };
static Page page = Page::Main;

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

// Every page owns the whole panel, so the one being left has to be taken off it
// before the one arriving is drawn - and both of them go on with one full flush
// rather than by the bands they would normally send.
static void turnTo(Page to) {
  uint16_t *fb = boardFramebuffer();
  page = to;
  memset(fb, 0, (size_t)SCREEN_W * SCREEN_H * 2);
  if (to == Page::Info) {
    infoForget();
    infoStep(fb);
    return;
  }
  // The bars are a blit of pixels already worked out, and status redraws both
  // its bands when it is told the whole panel was painted over - which it was.
  gaugeDraw(fb);
  statusDraw(fb, 0, SCREEN_H - 1);
  boardFlush();
}

void setup() {
  Serial.begin(115200);
  Serial.printf("reset: %s\n", why(esp_reset_reason()));
  if (!boardBegin()) {
    while (true) {
      delay(1000);
    }
  }
  batteryBegin();
  touchBegin();
  audioBegin();
  wifiBegin();
  portalBegin();
  usageBegin();
  faceBegin();
  gaugeBegin();
  // Empty, and on the glass straight away. They have nothing to say until the
  // first read comes back, but an empty track says that better than nothing at
  // all does.
  gaugeReveal();
  lastFrame = millis();
}

void loop() {
  uint32_t now = millis();
  float dt = (float)(now - lastFrame) * 0.001f;
  lastFrame = now;
  if (dt > 0.05f) {
    dt = 0.05f;
  }

  // The cable landing is worth a sound: it is the one thing that happens to
  // this board without anybody touching it.
  static bool wasCharging = batteryRead().charging;
  bool charging = batteryRead().charging;
  if (charging && !wasCharging) {
    audioPlugged();
  }
  wasCharging = charging;

  touchStep();
  Swipe swipe = touchSwiped();
  // The other page comes in from the right the way a phone's does: dragged onto
  // the glass leftward, and pushed back off it the way it came.
  if (swipe == Swipe::Left && page == Page::Main) {
    turnTo(Page::Info);
  } else if (swipe == Swipe::Right && page == Page::Info) {
    turnTo(Page::Main);
  }

  if (page == Page::Info) {
    infoStep(boardFramebuffer());
    uint32_t idle = millis() - now;
    if (idle < FRAME_MS) {
      delay(FRAME_MS - idle);
    }
    return;
  }

  uint32_t t0 = micros();
  int16_t from = 0;
  int16_t to = SCREEN_H - 1;
  // Two taps close together and the numbers come up; two more and they go. A
  // single tap is what a sleeve does, so it is not asked to mean anything.
  static uint32_t firstTap = 0;
  if (touchTapped()) {
    if (firstTap != 0 && now - firstTap < 400) {
      gaugeFigures();
      firstTap = 0;
    } else {
      firstTap = now;
    }
  }
  gaugeStep(boardFramebuffer(), now);
  faceStep(dt);
  faceDraw(boardFramebuffer(), &from, &to);
  // The face clears a box wide enough to reach both bars, so they go back in
  // before the flush rather than being repaired after it.
  gaugeDraw(boardFramebuffer());
  uint32_t t1 = micros();
  boardFlushRows(from, to);
  statusDraw(boardFramebuffer(), from, to);
  uint32_t t2 = micros();

  // Where the frame actually goes. Guessing at this is how you optimise the
  // half that was already cheap.
  static uint32_t frames = 0;
  static uint32_t drawUs = 0;
  static uint32_t flushUs = 0;
  drawUs += t1 - t0;
  flushUs += t2 - t1;
  if (++frames == 60) {
    Serial.printf("frame: draw %lu us, flush %lu us, %lu fps\n", drawUs / frames,
                  flushUs / frames, 1000000UL / ((drawUs + flushUs) / frames));
    frames = drawUs = flushUs = 0;
  }

  // Sixty is past what the panel or the eye wants; the rest goes back.
  uint32_t spent = millis() - now;
  if (spent < FRAME_MS) {
    delay(FRAME_MS - spent);
  }
}
