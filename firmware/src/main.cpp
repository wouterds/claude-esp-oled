#include <Arduino.h>
#include <esp_system.h>
#include <string.h>

#include "audio.h"
#include "battery.h"
#include "board.h"
#include "bus.h"
#include "status.h"
#include "touch.h"
#include "wifi.h"
#include "face.h"
#include "gauge.h"
#include "info.h"
#include "pokemon.h"
#include "portal.h"
#include "text.h"
#include "usage.h"

static constexpr uint32_t FRAME_MS = 16;

static uint32_t lastFrame = 0;

// A stack of them, a swipe apart: the face, what it is running on, and a shelf
// of bitmaps that is there because it is nice to have. None of them knows the
// others exist - what turns the page is here.
enum class Page : uint8_t { Main, Info, Pokemon };
static Page page = Page::Main;

// The frame rate, put on the glass rather than into the log, for when the thing
// being watched is what the drawing costs. Bright pink and across the top of
// whatever is underneath: it is not part of any page and should not be taken
// for one. It stays until it is asked to go, or until the board is restarted.
static constexpr uint16_t PINK = 0xF8B2;
static constexpr int16_t FPS_TOP = 8;
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
  if (to == Page::Pokemon) {
    pokemonOpen();
    pokemonStep(fb);
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
  // Each page comes up from below, dragged onto the glass, and is pushed back
  // down off it the way it came. Across the glass means something only where
  // there is a row of things to go along, which is one page of the three.
  if (swipe == Swipe::Up) {
    if (page == Page::Main) {
      turnTo(Page::Info);
    } else if (page == Page::Info) {
      turnTo(Page::Pokemon);
    }
  } else if (swipe == Swipe::Down) {
    if (page == Page::Pokemon) {
      turnTo(Page::Info);
    } else if (page == Page::Info) {
      turnTo(Page::Main);
    }
  } else if (swipe != Swipe::None && page == Page::Pokemon) {
    pokemonTurn(swipe == Swipe::Right);
  }

  // Two taps close together: on the face they put the numbers up, and on the
  // commit they put the frame rate up. A single tap is what a sleeve does, so it
  // is not asked to mean anything, and both taps have to land on the same thing.
  static uint32_t firstTap = 0;
  static int16_t firstAt = 0;
  if (touchTapped()) {
    int16_t at = touchTappedAt();
    if (firstTap != 0 && now - firstTap < 400) {
      if (page == Page::Main) {
        gaugeFigures();
      } else if (page == Page::Info && infoOnCommit(at) && infoOnCommit(firstAt)) {
        counting = !counting;
        // Whatever it was covering has to come back, and the page underneath is
        // the only thing that knows what was there.
        if (!counting) {
          turnTo(page);
        }
      }
      firstTap = 0;
    } else {
      firstTap = now;
      firstAt = at;
    }
  }

  if (page != Page::Main) {
    if (page == Page::Info) {
      infoStep(boardFramebuffer());
    } else {
      pokemonStep(boardFramebuffer());
    }
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
