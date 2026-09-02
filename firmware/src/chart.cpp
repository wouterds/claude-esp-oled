#include "chart.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "market.h"
#include "shape.h"
#include "text.h"

namespace {

// The network and charge figures end at 27, so this page starts below them and
// never has to know they are there.
constexpr int16_t OWNED_TOP = (int16_t)(36 * SCENE);

constexpr int16_t NAME_Y = (int16_t)(46 * SCENE);
constexpr int16_t NAME_SCALE = 2;
constexpr int16_t PRICE_Y = (int16_t)(82 * SCENE);
constexpr int16_t PRICE_SCALE = 4;
constexpr int16_t MOVE_Y = (int16_t)(136 * SCENE);
constexpr int16_t MOVE_SCALE = 3;
constexpr int16_t SPAN_Y = (int16_t)(292 * SCENE);
constexpr int16_t SPAN_SCALE = 2;

// Wide enough to be a chart and inside the circle at every row it covers: at
// its lowest the glass is 151 either side of the middle and this reaches 124.
constexpr float PLOT_X0 = 56.0f * SCENE;
constexpr float PLOT_X1 = 304.0f * SCENE;
constexpr float PLOT_Y0 = 176.0f * SCENE;
constexpr float PLOT_Y1 = 278.0f * SCENE;
// Half the stroke. A price line wants to read as one line rather than as the
// pixels it is made of, and under a pixel and a half it stops.
constexpr float STROKE = 1.6f * SCENE;
// Room above and below the line, so a week that only went one way does not draw
// itself along the very edge of its own box.
constexpr float HEADROOM = 0.08f;
// How far under the line the wash reaches before it is gone. Not to the floor:
// a fill that fades over the whole height reads as a shape, and over a third of
// it reads as the line having weight.
constexpr float WASH = 0.45f;
constexpr float WASH_ALPHA = 0.30f;

// Three of them, chasing. It says the same thing a word would and does not have
// to be read.
constexpr float DOT_R = 4.5f * SCENE;
constexpr float DOT_GAP = 20.0f * SCENE;
constexpr uint32_t DOT_CYCLE_MS = 1200;
constexpr int16_t DOT_TOP = (int16_t)(PRICE_Y + 2 * SCENE);
constexpr int16_t DOT_BOTTOM = (int16_t)(PRICE_Y + 2 * SCENE + 2 * DOT_R + 4);

constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t GREY = 0x8410;
constexpr uint16_t FAINT = 0x4A49;
// The system green and red, near enough as five bits of each reach them. The
// line wears whichever the figure does, so the two are one statement rather
// than two that happen to agree.
constexpr uint16_t UP = 0x368B;
constexpr uint16_t DOWN = 0xFA27;

uint32_t shownRev = 0;
int16_t shownScreen = -1;
bool fresh = true;

// Grouped in threes. A price is read at a glance and six digits in a row is not
// a glance - and how many places it is worth carrying depends on the price:
// eight decimal places of a hundred thousand is noise, and none at all of
// something under a dollar is nothing.
void money(char *out, size_t size, float value, bool dollars) {
  const char *sign = dollars ? "$" : "";
  char digits[24];
  if (value >= 1000.0f) {
    snprintf(digits, sizeof(digits), "%.0f", value);
  } else if (value >= 1.0f) {
    snprintf(digits, sizeof(digits), "%.2f", value);
  } else {
    snprintf(digits, sizeof(digits), "%.4f", value);
  }

  const char *point = strchr(digits, '.');
  size_t whole = point ? (size_t)(point - digits) : strlen(digits);
  size_t at = 0;
  out[0] = '\0';
  for (const char *c = sign; *c && at + 1 < size; c++) {
    out[at++] = *c;
  }
  for (size_t i = 0; i < strlen(digits) && at + 1 < size; i++) {
    // Before a digit whose distance from the point is a multiple of three, and
    // never before the first one.
    if (i > 0 && i < whole && (whole - i) % 3 == 0) {
      out[at++] = ',';
    }
    if (at + 1 < size) {
      out[at++] = digits[i];
    }
  }
  out[at] = '\0';
}

// The panel is scanned the other way up, so a range of scene rows is the
// mirror of it. Both ends swap with the flip, which is why they cross over.
inline int16_t bandFrom(int16_t to) { return (int16_t)(SCREEN_H - 1 - to); }
inline int16_t bandTo(int16_t from) { return (int16_t)(SCREEN_H - 1 - from); }

// textDraw already takes the middle and already allows for the last glyph
// carrying no gap after it, so the middle of the glass is all this has to say.
void centred(uint16_t *fb, const char *s, int16_t top, int16_t scale, uint16_t ink) {
  if (!s || !s[0]) {
    return;
  }
  textDraw(fb, s, (int16_t)SCREEN_R, top, scale, boardColour(ink));
}

void clearOwned(uint16_t *fb, int16_t from, int16_t to) {
  for (int16_t y = from; y <= to; y++) {
    memset(boardRow(fb, y), 0, (size_t)SCREEN_W * 2);
  }
}

float atX(const Market &m, uint8_t i) {
  return m.count < 2 ? PLOT_X0
                     : PLOT_X0 + (PLOT_X1 - PLOT_X0) * (float)i / (float)(m.count - 1);
}

float atY(const Market &m, uint8_t i) {
  float span = m.high - m.low;
  // A week that never moved is a line through the middle rather than a divide
  // by nothing.
  if (span <= 0.0f) {
    return (PLOT_Y0 + PLOT_Y1) * 0.5f;
  }
  float pad = span * HEADROOM;
  float low = m.low - pad;
  float high = m.high + pad;
  float t = (m.points[i] - low) / (high - low);
  return PLOT_Y1 - t * (PLOT_Y1 - PLOT_Y0);
}

// One pass per column with only the segments that can reach it. Written that
// way rather than segment by segment because plot puts a colour down instead of
// blending it, so a segment's faint edge drawn second would rub out the solid
// pixel its neighbour had already put there.
void drawPlot(uint16_t *fb, const Market &m, uint16_t ink) {
  if (m.count < 2) {
    return;
  }
  int16_t from = (int16_t)(PLOT_X0 - STROKE - 1);
  int16_t to = (int16_t)(PLOT_X1 + STROKE + 1);
  float step = (PLOT_X1 - PLOT_X0) / (float)(m.count - 1);

  for (int16_t x = from; x <= to; x++) {
    float px = (float)x + 0.5f;
    int16_t mid = (int16_t)((px - PLOT_X0) / step);
    int16_t first = mid - 1 < 0 ? 0 : mid - 1;
    int16_t last = mid + 1 > m.count - 2 ? m.count - 2 : mid + 1;
    if (first > last) {
      continue;
    }

    float top = PLOT_Y1;
    float bottom = PLOT_Y0;
    for (int16_t i = first; i <= last; i++) {
      float a = atY(m, (uint8_t)i);
      float b = atY(m, (uint8_t)(i + 1));
      top = fminf(top, fminf(a, b));
      bottom = fmaxf(bottom, fmaxf(a, b));
    }

    // Where the line actually is at this column, which is what the wash under
    // it hangs from. Off the one segment that covers the column rather than the
    // three the stroke is measured against.
    int16_t own = mid < 0 ? 0 : (mid > m.count - 2 ? (int16_t)(m.count - 2) : mid);
    float ax = atX(m, (uint8_t)own);
    float bx = atX(m, (uint8_t)(own + 1));
    float along = bx > ax ? clamp01((px - ax) / (bx - ax)) : 0.0f;
    float lit = atY(m, (uint8_t)own) +
                (atY(m, (uint8_t)(own + 1)) - atY(m, (uint8_t)own)) * along;
    // Drawn before the line and only under it, so the line goes over the wash
    // rather than through it.
    if (x >= (int16_t)PLOT_X0 && x <= (int16_t)PLOT_X1) {
      float reach = (PLOT_Y1 - lit) * WASH;
      for (int16_t y = (int16_t)lit; y <= (int16_t)(lit + reach); y++) {
        float fell = reach > 0.0f ? ((float)y - lit) / reach : 1.0f;
        plot(fb, x, y, WASH_ALPHA * (1.0f - fell), ink);
      }
    }

    for (int16_t y = (int16_t)(top - STROKE - 1); y <= (int16_t)(bottom + STROKE + 1); y++) {
      float py = (float)y + 0.5f;
      float near = 1e9f;
      for (int16_t i = first; i <= last; i++) {
        near = fminf(near, sdSegment(px, py, atX(m, (uint8_t)i), atY(m, (uint8_t)i),
                                     atX(m, (uint8_t)(i + 1)), atY(m, (uint8_t)(i + 1))));
      }
      float cover = 0.5f - (near - STROKE);
      if (cover > 0.02f) {
        plot(fb, x, y, cover, ink);
      }
    }
  }
}

void drawDots(uint16_t *fb) {
  float turn = (float)(millis() % DOT_CYCLE_MS) / (float)DOT_CYCLE_MS;
  float cy = (float)DOT_TOP + DOT_R;
  for (int8_t i = -1; i <= 1; i++) {
    float cx = (float)SCREEN_R + (float)i * DOT_GAP;
    // A travelling pulse rather than three that blink together, which reads as
    // one thing working instead of three things flashing.
    float phase = turn - (float)(i + 1) * 0.15f;
    float wave = sinf(phase * 2.0f * (float)M_PI);
    float lit = 0.18f + 0.82f * (wave > 0.0f ? wave : 0.0f);
    for (int16_t y = (int16_t)(cy - DOT_R - 1); y <= (int16_t)(cy + DOT_R + 1); y++) {
      for (int16_t x = (int16_t)(cx - DOT_R - 1); x <= (int16_t)(cx + DOT_R + 1); x++) {
        float dx = (float)x + 0.5f - cx;
        float dy = (float)y + 0.5f - cy;
        float cover = 0.5f - (sqrtf(dx * dx + dy * dy) - DOT_R);
        if (cover > 0.02f) {
          plot(fb, x, y, cover * lit, WHITE);
        }
      }
    }
  }
}

void drawAll(uint16_t *fb, const Market &m, bool coin) {
  clearOwned(fb, OWNED_TOP, (int16_t)(SCREEN_H - 1));
  centred(fb, m.name, NAME_Y, NAME_SCALE, GREY);

  if (!m.ready) {
    // Nothing to say yet, and the name above is as much as is known. What is
    // wrong is worth saying; what is merely not here yet is not.
    if (m.failed) {
      centred(fb, "NO DATA", PRICE_Y, MOVE_SCALE, FAINT);
    } else {
      drawDots(fb);
    }
    return;
  }

  uint16_t ink = m.change < 0.0f ? DOWN : UP;
  char said[24];
  // A dollar in front of a coin and nothing in front of an index: an index is
  // quoted in its own points and never was a number of dollars.
  money(said, sizeof(said), m.price, coin);
  centred(fb, said, PRICE_Y, PRICE_SCALE, WHITE);

  snprintf(said, sizeof(said), "%+.2f%%", (double)m.change);
  centred(fb, said, MOVE_Y, MOVE_SCALE, ink);

  drawPlot(fb, m, ink);
  centred(fb, m.span, SPAN_Y, SPAN_SCALE, FAINT);
}

}  // namespace

void chartForget() { fresh = true; }

void chartStep(uint16_t *fb, uint8_t screen) {
  Market m;
  if (!marketAt(screen, &m)) {
    return;
  }
  uint32_t rev = marketRevision();
  bool all = fresh || screen != shownScreen || rev != shownRev;
  fresh = false;
  shownScreen = (int16_t)screen;
  shownRev = rev;

  if (all) {
    drawAll(fb, m, screen < MARKET_COINS);
    boardFlushRows(bandFrom((int16_t)(SCREEN_H - 1)), bandTo(OWNED_TOP));
    return;
  }
  // Nothing has changed and there are no figures yet, so the pulse is the only
  // thing on here that moves - and its own rows are all it moves in.
  if (!m.ready && !m.failed) {
    clearOwned(fb, DOT_TOP, DOT_BOTTOM);
    drawDots(fb);
    boardFlushRows(bandFrom(DOT_BOTTOM), bandTo(DOT_TOP));
  }
}
