#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <string.h>

#include "audio.h"
#include "battery.h"
#include "board.h"
#include "chart.h"
#include "bus.h"
#include "button.h"
#include "status.h"
#include "touch.h"
#include "wifi.h"
#include "face.h"
#include "gauge.h"
#include "info.h"
#include "load.h"
#include "market.h"
#include "net.h"
#include "nuc.h"
#include "outage.h"
#include "portal.h"
#include "settings.h"
#include "shot.h"
#include "usage.h"
#include "vitals.h"

// Sixty a second, in microseconds because it does not go into milliseconds: a
// whole 16 is 62.5 frames a second and a whole 17 is 58.8, and neither of them
// is the number anybody asked for.
static constexpr uint32_t FRAME_US = 16667;

static uint32_t lastFrame = 0;
static uint32_t nextFrame = 0;

// What a screenshot costs to ask for. Long enough that nothing reaches the one
// button by accident and nothing already on it is lengthened into this, and
// short enough to sit through with a finger on a board this size.
static constexpr uint32_t SHOT_HOLD_MS = 3000;

// The face, what it is running on over it, the two things about the board that
// are somebody's taste under it, the market screens one side of it and the box
// in the other room on the other. None knows the others exist - what turns the
// page is here.
enum class Page : uint8_t { Main, Info, Settings, Chart, Vitals };
static Page page = Page::Main;

// How far along the row from the face. The market screens are to its left - the
// indices first and the coins behind them - and the one step to its right is the
// box in the other room, so one line runs through all of them and a swipe drags
// it past. Up and down only mean anything from along the row: the details above
// a chart would be a page with no way back to it that anybody would guess.
static int8_t sideways = 0;

// Which page a place along the row is.
static Page pageAt(int8_t at) {
  if (at < 0) {
    return Page::Vitals;
  }
  return at == 0 ? Page::Main : Page::Chart;
}

// Which of the market screens that is, for the places along the row that are
// one. The indices are held after the coins, so the order they are walked in is
// not the order they are stored in.
static uint8_t screenOf(int8_t at) {
  return at <= (int8_t)MARKET_INDICES ? (uint8_t)(MARKET_COINS + at - 1)
                                      : (uint8_t)(at - MARKET_INDICES - 1);
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
    // The four that a reset nobody asked for actually comes back as. Left off,
    // every one of them reads "other", which is the same thing the log says for
    // a reset that is entirely ordinary - and telling a watchdog from a monitor
    // being plugged in is the whole reason the reason is printed.
    case ESP_RST_TASK_WDT:
      return "task watchdog";
    case ESP_RST_INT_WDT:
      return "interrupt watchdog";
    case ESP_RST_WDT:
      return "another watchdog";
    case ESP_RST_USB:
      return "usb peripheral";
    default:
      return "other";
  }
}

// Holds the loop to FRAME_US. Most of the wait goes through delay(), which hands
// the core to whatever else wants it; only the sub-millisecond remainder is spun
// on, because delayMicroseconds() does not yield and sixteen milliseconds of not
// yielding is a long time to hold a core for the sake of a rounding.
static void pace() {
  uint32_t now = micros();
  int32_t left = (int32_t)(nextFrame - now);
  if (left > 0) {
    if (left >= 1000) {
      delay((uint32_t)left / 1000);
    }
    delayMicroseconds((uint32_t)left % 1000);
  } else {
    // Behind. The next one is paced from here rather than from where this one
    // should have ended, so a slow frame costs a frame instead of being paid
    // back by a burst of fast ones.
    nextFrame = now;
  }
  nextFrame += FRAME_US;
}

// Every page owns the whole panel, so the one being left has to be taken off it
// before the one arriving is drawn - and both of them go on with one full flush
// rather than by the bands they would normally send.
static void turnTo(Page to) {
  uint16_t *fb = boardFramebuffer();
  page = to;
  // The readings are only ever on the face. Off it the pollers stop asking, and
  // coming back to it after long enough asks again at once.
  netWatching(to == Page::Main);
  // And a market screen is read only while it is the one up, for the same
  // reason: what came back would be drawn on a page nobody is looking at. The
  // box in the other room is asked the same question and answers it the same
  // way.
  marketWatching(to == Page::Chart ? (int8_t)screenOf(sideways) : (int8_t)-1);
  nucWatching(to == Page::Vitals);
  memset(fb, 0, (size_t)SCREEN_W * SCREEN_H * 2);
  if (to == Page::Info) {
    infoForget();
    infoStep(fb);
    return;
  }
  if (to == Page::Settings) {
    settingsForget();
    settingsStep(fb);
    // The two figures at the top mean the same thing here as anywhere.
    statusBars(fb, true);
    return;
  }
  if (to == Page::Chart || to == Page::Vitals) {
    if (to == Page::Chart) {
      chartForget();
      chartStep(fb, screenOf(sideways));
    } else {
      vitalsForget();
      vitalsStep(fb);
    }
    statusBars(fb, true);
    // Both of those sent their own rows; this is for the ones between them,
    // which were cleared above and belong to nobody.
    boardFlush();
    return;
  }
  // The bars are a blit of pixels already worked out, and status redraws both
  // its bands when it is told the whole panel was painted over - which it was.
  gaugeDraw(fb, 0, SCREEN_W - 1, 0, SCREEN_H - 1);
  statusDraw(fb, 0, SCREEN_H - 1);
  boardFlush();
}

void setup() {
  Serial.begin(115200);
  // Unplugged, a write that does not fit is thrown away and returns. Plugged
  // into a host that has stopped reading - a Mac with the port enumerated and
  // no monitor on it - the same write blocks 100ms a go, twenty times over, and
  // whichever task printed stops dead for two seconds. Nought means short
  // writes instead: a truncated log line is cheaper than a frozen panel.
  Serial.setTxTimeoutMs(0);
  Serial.printf("reset: %s\n", why(esp_reset_reason()));
  // Before the panel, which comes up at whatever this says.
  settingsBegin();
  if (!boardBegin(settingsBrightness())) {
    while (true) {
      delay(1000);
    }
  }
  busBegin();
  netBegin();
  loadBegin();
  batteryBegin();
  touchBegin();
  buttonBegin();
  shotBegin();
  audioBegin();
  audioVolume(settingsVolume());
  wifiBegin();
  portalBegin();
  usageBegin();
  outageBegin();
  marketBegin();
  nucBegin();
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

  // Both pages, because what it measures is the whole board rather than the page
  // in front of it.
  loadStep();
  // Both pages as well: a change made just before the page was left still has
  // to reach the flash.
  settingsKeep();

  // Asked every frame whichever page is up, so a pair of taps on one page is
  // not waiting to land on the next.
  int16_t tapAcross = 0;
  int16_t tapAlong = 0;
  if (touchDoubleTapped(&tapAcross, &tapAlong) && page == Page::Info) {
    infoTapped(tapAcross, tapAlong);
  }
  if (touchTapped(&tapAcross, &tapAlong) && page == Page::Info) {
    infoSingleTapped(tapAcross, tapAlong);
  }

  Swipe swipe = touchSwiped();
  // The details are pushed up over the face and pulled back down off it; the
  // sliders are pulled down onto the face and pushed back up off it. One page
  // either side, and each leaves the way it came.
  //
  // Not while a finger is on a slider. A drag along a track wanders up and down
  // as well, and that wander is otherwise the swipe that takes the page away
  // from under the finger setting it.
  // The cat is the same: while it has the glass it goes when it is asked the
  // way it came, not when the glass is brushed.
  if (settingsHolding() || infoFullscreen()) {
    swipe = Swipe::None;
  }
  // Anywhere along the row, not just the face: the details and the sliders are
  // above and below all of it. Coming back down comes back to where you left
  // rather than to the middle - the position along the row is kept, so a swipe
  // up from a chart and a swipe back down is where you started and not a
  // journey home.
  bool row = page == Page::Main || page == Page::Chart || page == Page::Vitals;
  Page back = pageAt(sideways);
  if (swipe == Swipe::Up && row) {
    turnTo(Page::Info);
  } else if (swipe == Swipe::Down && page == Page::Info) {
    turnTo(back);
  } else if (swipe == Swipe::Down && row) {
    turnTo(Page::Settings);
  } else if (swipe == Swipe::Up && page == Page::Settings) {
    turnTo(back);
  } else if ((swipe == Swipe::Left || swipe == Swipe::Right) && row) {
    // A finger dragged right takes the row right with it, which brings what was
    // off the left edge onto the glass - so the markets, which are the left of
    // this row, are what a swipe right arrives at, and the box in the other room
    // is what a swipe left does.
    int8_t to = (int8_t)(sideways + (swipe == Swipe::Right ? 1 : -1));
    // The row ends rather than wrapping: a list that comes back round to where
    // it started gives no clue how far along it you are. One end is the last
    // market screen and the other is the one screen the other side of the face.
    int8_t end = (int8_t)(MARKET_INDICES + MARKET_COINS);
    if (to > end) {
      to = end;
    } else if (to < -1) {
      to = -1;
    }
    if (to != sideways) {
      sideways = to;
      turnTo(pageAt(sideways));
    }
  }

  // Held rather than pressed, and asked first: the release that ends a hold is
  // swallowed, so the press below never sees the one that took a screenshot.
  //
  // The sound goes before the encode, and finishes before it. What it
  // acknowledges is that the press landed, and the encode stops the glass for a
  // second - which with no answer at all is how a held button reads as a board
  // that has hung.
  //
  // Waited on rather than left to overlap. The notes are rendered on the core
  // the radio is on and fetched through the flash, and the encode walks over
  // PSRAM hard enough to stall that - the two share one controller on this
  // chip. Asked for and left to it, the shutter came out of the speaker in
  // pieces or not at all.
  if (buttonHeld(SHOT_HOLD_MS)) {
    audioShuttered();
    audioWait();
    if (!shotTake()) {
      audioErrored();
    }
  }

  // The one button the chip can read, and the numbers on the face are the only
  // thing it means. A press says so once and unambiguously, which two taps on a
  // glass this size never did - a sleeve managed the first of them often enough
  // to be a nuisance, and the pair had to land on the same thing to count at all.
  if (buttonPressed() && page == Page::Main) {
    gaugeFigures();
  }

  if (page != Page::Main) {
    if (page == Page::Info) {
      infoStep(boardFramebuffer());
    } else if (page == Page::Settings) {
      settingsStep(boardFramebuffer());
      statusBars(boardFramebuffer(), false);
    } else if (page == Page::Vitals) {
      vitalsStep(boardFramebuffer());
      statusBars(boardFramebuffer(), false);
    } else {
      chartStep(boardFramebuffer(), screenOf(sideways));
      statusBars(boardFramebuffer(), false);
    }
    pace();
    return;
  }

  uint32_t t0 = micros();
  int16_t from = 0;
  int16_t to = SCREEN_H - 1;
  int16_t colFrom = 0;
  int16_t colTo = SCREEN_W - 1;
  gaugeStep(boardFramebuffer(), now);
  faceStep(dt);
  faceDraw(boardFramebuffer(), &from, &to, &colFrom, &colTo);
  // Whatever of the bars the face just cleared goes back in before the flush
  // rather than being repaired after it - and only that much of them, since the
  // rest of the panel is neither cleared nor sent.
  gaugeDraw(boardFramebuffer(), colFrom, colTo, from, to);
  uint32_t t1 = micros();
  boardFlushRect(colFrom, colTo, from, to);
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
    Serial.printf("heap: internal %u free (min %u, largest %u)\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    frames = drawUs = flushUs = 0;
  }

  // Sixty is past what the panel or the eye wants; the rest goes back.
  pace();
}
