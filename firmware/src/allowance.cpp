#include "allowance.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "board.h"
#include "gauge.h"
#include "text.h"
#include "usage.h"

namespace {

// A pair of them, either side of the middle. Wide enough apart that the two
// countdowns under them do not meet, which is what sets the spacing rather than
// the bars themselves.
constexpr float BAR_HW = 26.0f;
constexpr float BAR_HH = 74.0f;
constexpr float BAR_Y = 128.0f;
constexpr float LEFT_X = 118.0f;
constexpr float RIGHT_X = 242.0f;
// The top of what is spent is rounded, but nothing like as round as the track:
// given the track's own radius the fill reads as a ball sitting in a tube
// rather than as a level in one.
constexpr float SPENT_R = 8.0f;

constexpr int16_t PERCENT_TOP = 216;
constexpr int16_t PERCENT_SCALE = 3;
constexpr int16_t LABEL_TOP = 246;
constexpr int16_t CLOCK_TOP = 272;
constexpr int16_t SMALL_SCALE = 2;

// The same track the arcs are drawn on, so an empty bar here and an empty arc
// there are the same grey.
constexpr uint16_t TRACK = 0x4A49;
constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t GREY = 0x8410;

struct Shown {
  bool ready;
  uint8_t session;
  uint8_t weekly;
  uint32_t sessionLeft;
  uint32_t weeklyLeft;
};

Shown shown = {false, 255, 255, 0, 0};
bool fresh = true;

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

void plot(uint16_t *fb, int16_t x, int16_t y, float coverage, uint16_t colour) {
  if (coverage <= 0.02f || x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) {
    return;
  }
  coverage = clamp01(coverage);
  uint16_t r = (uint16_t)(((colour >> 11) & 0x1F) * coverage);
  uint16_t g = (uint16_t)(((colour >> 5) & 0x3F) * coverage);
  uint16_t b = (uint16_t)((colour & 0x1F) * coverage);
  boardRow(fb, y)[boardX(x)] = boardColour((uint16_t)((r << 11) | (g << 5) | b));
}

float sdRoundBox(float px, float py, float hx, float hy, float r) {
  float limit = hx < hy ? hx : hy;
  if (r > limit) {
    r = limit;
  }
  float qx = fabsf(px) - hx + r;
  float qy = fabsf(py) - hy + r;
  float ax = qx > 0.0f ? qx : 0.0f;
  float ay = qy > 0.0f ? qy : 0.0f;
  float inner = qx > qy ? qx : qy;
  return sqrtf(ax * ax + ay * ay) + (inner < 0.0f ? inner : 0.0f) - r;
}

// A track with however much of it is spent standing in the bottom. The track is
// a pill, and the fill is cut to it - on its own, a fill an inch high is a
// full-width sliver that hangs out past the round bottom of the tube holding
// it.
void bar(uint16_t *fb, float cx, uint8_t percent) {
  float top = BAR_HH - 2.0f * BAR_HH * (percent / 100.0f);
  float spentHH = (BAR_HH - top) * 0.5f;
  float spentY = (BAR_HH + top) * 0.5f;
  uint16_t colour = gaugeColour(percent);

  for (int16_t y = (int16_t)(BAR_Y - BAR_HH - 2); y <= (int16_t)(BAR_Y + BAR_HH + 2); y++) {
    for (int16_t x = (int16_t)(cx - BAR_HW - 2); x <= (int16_t)(cx + BAR_HW + 2); x++) {
      float px = (float)x + 0.5f - cx;
      float py = (float)y + 0.5f - BAR_Y;
      float track = sdRoundBox(px, py, BAR_HW, BAR_HH, BAR_HW);
      plot(fb, x, y, 0.5f - track, TRACK);
      if (percent > 0) {
        float spent = sdRoundBox(px, py - spentY, BAR_HW, spentHH, SPENT_R);
        plot(fb, x, y, 0.5f - (spent > track ? spent : track), colour);
      }
    }
  }
}

// HH:MM:SS, with the hours running as wide as they have to - a week out is a
// hundred and sixty-odd of them.
void clockOf(char *out, size_t size, uint32_t ms) {
  uint32_t seconds = ms / 1000;
  snprintf(out, size, "%02u:%02u:%02u", (unsigned)(seconds / 3600),
           (unsigned)((seconds / 60) % 60), (unsigned)(seconds % 60));
}

void column(uint16_t *fb, float cx, const char *label, uint8_t percent, uint32_t left,
            bool ready) {
  bar(fb, cx, ready ? percent : 0);
  textDraw(fb, label, (int16_t)cx, LABEL_TOP, SMALL_SCALE, boardColour(GREY));
  if (!ready) {
    return;
  }

  char said[6];
  snprintf(said, sizeof(said), "%u%%", (unsigned)percent);
  textDraw(fb, said, (int16_t)cx, PERCENT_TOP, PERCENT_SCALE, boardColour(gaugeColour(percent)));

  // A window nothing has been spent in has no reset to name.
  if (left > 0) {
    char clock[12];
    clockOf(clock, sizeof(clock), left);
    textDraw(fb, clock, (int16_t)cx, CLOCK_TOP, SMALL_SCALE, boardColour(WHITE));
  }
}

}  // namespace

void allowanceForget() { fresh = true; }

void allowanceStep(uint16_t *fb) {
  bool ready = usageReady();
  uint8_t session = usageSession();
  uint8_t weekly = usageWeekly();
  // Compared as seconds rather than as milliseconds, which is what holds this
  // page to one repaint a second instead of one a frame.
  uint32_t sessionLeft = usageSessionResetsIn() / 1000;
  uint32_t weeklyLeft = usageWeeklyResetsIn() / 1000;

  bool changed = fresh || ready != shown.ready || session != shown.session ||
                 weekly != shown.weekly || sessionLeft != shown.sessionLeft ||
                 weeklyLeft != shown.weeklyLeft;
  if (!changed) {
    return;
  }
  fresh = false;
  shown = {ready, session, weekly, sessionLeft, weeklyLeft};

  memset(fb, 0, (size_t)SCREEN_W * SCREEN_H * 2);
  column(fb, LEFT_X, "5 HOURS", session, sessionLeft * 1000, ready);
  column(fb, RIGHT_X, "WEEKLY", weekly, weeklyLeft * 1000, ready);
  if (!ready) {
    textDraw(fb, "NO USAGE YET", (int16_t)SCREEN_R, PERCENT_TOP, SMALL_SCALE, boardColour(GREY));
  }
  boardFlush();
}
