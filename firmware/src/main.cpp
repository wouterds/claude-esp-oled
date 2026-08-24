#include <Arduino.h>
#include <esp_system.h>
#include <string.h>

#include "audio.h"
#include "battery.h"
#include "board.h"
#include "bus.h"
#include "button.h"
#include "status.h"
#include "touch.h"
#include "wifi.h"
#include "face.h"
#include "gauge.h"
#include "info.h"
#include "outage.h"
#include "portal.h"
#include "text.h"
#include "usage.h"

static constexpr uint32_t FRAME_MS = 16;

static uint32_t lastFrame = 0;

// Two of them, a swipe apart: the face, and what it is running on. Neither knows
// the other exists - what turns the page is here.
enum class Page : uint8_t { Main, Info };
static Page page = Page::Main;

// The frame rate, put on the glass rather than into the log, for when the thing
// being watched is what the drawing costs. Bright pink and across the top of
// whatever is underneath: it is not part of any page and should not be taken
// for one. It stays until it is asked to go, or until the board is restarted.
static constexpr uint16_t PINK = 0xF8B2;
// The outage triangle sits with its middle sixty-three rows down from the top,
// so this puts the frame rate's middle sixty-three rows up from the bottom - the
// same mark read from the other end. It clears the bottom line of text, which
// starts at 307 on both pages, by a row and a half, and that is the whole of the
// room down there.
static constexpr int16_t FPS_TOP = 290;
static constexpr int16_t FPS_SCALE = 2;
static constexpr int16_t FPS_HALF = 64;
static bool counting = false;
static float fps = 0.0f;

static void countFrames(uint16_t *fb) {
  char said[12];
  snprintf(said, sizeof(said), "%u FPS", (unsigned)(fps + 0.5f));

  // Cleared and written again every frame. Whatever is underneath has just
  // drawn itself, and this has to be the last thing to reach the glass.
  int16_t from = FPS_TOP - 2;
  int16_t to = (int16_t)(FPS_TOP + 7 * FPS_SCALE + 1);
  int16_t x0 = (int16_t)(SCREEN_R - FPS_HALF);
  int16_t x1 = (int16_t)(SCREEN_R + FPS_HALF);
  for (int16_t y = from; y <= to; y++) {
    memset(boardRow(fb, y) + boardX(x1), 0, (size_t)(x1 - x0 + 1) * 2);
  }
  textDraw(fb, said, (int16_t)SCREEN_R, FPS_TOP, FPS_SCALE, boardColour(PINK));
  boardFlushRows((int16_t)(SCREEN_H - 1 - to), (int16_t)(SCREEN_H - 1 - from));
}

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
  busBegin();
  batteryBegin();
  touchBegin();
  buttonBegin();
  audioBegin();
  wifiBegin();
  portalBegin();
  usageBegin();
  outageBegin();
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
  // Smoothed, or the number is unreadable: it lands somewhere new every frame
  // and half of what it says is the frame it is being read on.
  if (dt > 0.0f) {
    fps = fps > 0.0f ? fps * 0.9f + (0.1f / dt) : 1.0f / dt;
  }

  // The cable landing is worth a sound: it is the one thing that happens to
  // this board without anybody touching it.
  static bool wasCharging = batteryRead().charging;
  bool charging = batteryRead().charging;
  if (charging && !wasCharging) {
    audioPlugged();
  }
  wasCharging = charging;

  Swipe swipe = touchSwiped();
  // The other page comes up from below, dragged onto the glass, and is pushed
  // back down off it the way it came.
  if (swipe == Swipe::Up && page == Page::Main) {
    turnTo(Page::Info);
  } else if (swipe == Swipe::Down && page == Page::Info) {
    turnTo(Page::Main);
  }

  // The one button the chip can read, and it means whatever page is in front of
  // it: the numbers on the face, the frame rate on the details. A press says so
  // once and unambiguously, which two taps on a glass this size never did - a
  // sleeve managed the first of them often enough to be a nuisance, and the pair
  // had to land on the same thing to count at all.
  if (buttonPressed()) {
    if (page == Page::Main) {
      gaugeFigures();
    } else {
      counting = !counting;
      // Whatever it was covering has to come back, and the page underneath is
      // the only thing that knows what was there.
      if (!counting) {
        turnTo(page);
      }
    }
  }

  if (page == Page::Info) {
    infoStep(boardFramebuffer());
    if (counting) {
      countFrames(boardFramebuffer());
    }
    uint32_t idle = millis() - now;
    if (idle < FRAME_MS) {
      delay(FRAME_MS - idle);
    }
    return;
  }

  uint32_t t0 = micros();
  int16_t from = 0;
  int16_t to = SCREEN_H - 1;
  gaugeStep(boardFramebuffer(), now);
  faceStep(dt);
  faceDraw(boardFramebuffer(), &from, &to);
  // The face clears a box wide enough to reach both bars, so they go back in
  // before the flush rather than being repaired after it.
  gaugeDraw(boardFramebuffer());
  uint32_t t1 = micros();
  boardFlushRows(from, to);
  statusDraw(boardFramebuffer(), from, to);
  if (counting) {
    countFrames(boardFramebuffer());
  }
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
