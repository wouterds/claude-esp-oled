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

constexpr int16_t NAME_Y = (int16_t)(52 * SCENE);
constexpr int16_t NAME_SCALE = 2;
constexpr int16_t PRICE_Y = (int16_t)(84 * SCENE);
constexpr int16_t PRICE_SCALE = 4;
constexpr int16_t MOVE_Y = (int16_t)(122 * SCENE);
constexpr int16_t MOVE_SCALE = 3;
// Where the countdown under the face sits, at the size it sits there - so the
// one line at the bottom of the glass is in the same place whichever page is
// up. Measured from the bottom edge rather than scaled from the top, which is
// how that line does it and why it needs no per-board nudge: fifty rows off the
// edge is fifty rows off the edge on either piece of glass.
constexpr int16_t SPAN_Y = (int16_t)(SCREEN_H - 50);
constexpr int16_t SPAN_SCALE = 2;

// The whole width. The glass is round, so the last twenty pixels of each end
// fall outside the circle at the rows this covers and are never seen - the line
// runs off both sides rather than stopping short of them, which is the point.
constexpr float PLOT_X0 = 0.0f;
constexpr float PLOT_X1 = (float)(SCREEN_W - 1);
constexpr float PLOT_Y0 = 166.0f * SCENE;
constexpr float PLOT_Y1 = 266.0f * SCENE;
// Sub-segments per gap between two prices. The line is drawn from these rather
// than from the prices themselves, which is what takes the corners off it.
constexpr uint8_t SUB = 4;
// Half the stroke, and the same as the load line on the box's page wears - one
// weight of line across the device. A price line wants to read as one line
// rather than as the pixels it is made of, and under a pixel it stops.
constexpr float STROKE = 1.15f * SCENE;
// Room above and below the line, so a week that only went one way does not draw
// itself along the very edge of its own box.
constexpr float HEADROOM = 0.08f;
// All the way to the baseline, and squared rather than cubed. The drop is what
// the wash is allowed to cover; the exponent is where inside that drop it
// actually spends its alpha, and cubed spends nearly all of it in the first
// fifth - the rest reaches the baseline at an alpha no panel resolves, so it
// reads as a halo under the line rather than as a fade. Squared moves the same
// alpha down into the middle of the drop, which is the part that shows.
constexpr float WASH = 1.0f;
constexpr float WASH_ALPHA = 0.30f;

// A ring with a quarter cut out of it, turning - the shape every loader has
// settled on, and the one that needs no legend. Small and a dark grey: it is
// standing in for a figure, not announcing itself.
constexpr uint8_t SPIN_STEPS = 14;
constexpr float SPIN_SWEEP = 4.712389f;
constexpr float SPIN_R = 13.0f * SCENE;
constexpr float SPIN_HALF = 1.6f * SCENE;
constexpr uint32_t SPIN_MS = 900;
constexpr int16_t SPIN_REACH = (int16_t)(SPIN_R + SPIN_HALF + 2);
constexpr int16_t SPIN_TOP = (int16_t)SCREEN_R - SPIN_REACH;
constexpr int16_t SPIN_BOTTOM = (int16_t)SCREEN_R + SPIN_REACH;

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

int16_t held(int16_t at, int16_t last) { return at < 0 ? 0 : (at > last ? last : at); }

float atY(const Market &m, uint8_t i) {
  float span = m.high - m.low;
  // A day that never moved is a line through the middle rather than a divide by
  // nothing.
  if (span <= 0.0f) {
    return (PLOT_Y0 + PLOT_Y1) * 0.5f;
  }
  float pad = span * HEADROOM;
  float low = m.low - pad;
  float high = m.high + pad;
  float t = (m.points[i] - low) / (high - low);
  return PLOT_Y1 - t * (PLOT_Y1 - PLOT_Y0);
}

// Catmull-Rom through the prices, at a fractional index between them. It passes
// through every one of them rather than near them, so the line still touches
// every price it was drawn from and only the corners between them are rounded -
// which is rounding, not smoothing, and the difference matters when the thing
// being drawn is what something cost.
float smoothAt(const Market &m, float at) {
  int16_t last = (int16_t)m.count - 1;
  int16_t i = (int16_t)floorf(at);
  float t = at - (float)i;
  float p0 = atY(m, (uint8_t)held((int16_t)(i - 1), last));
  float p1 = atY(m, (uint8_t)held(i, last));
  float p2 = atY(m, (uint8_t)held((int16_t)(i + 1), last));
  float p3 = atY(m, (uint8_t)held((int16_t)(i + 2), last));
  float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
  float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
  float c = -0.5f * p0 + 0.5f * p2;
  return ((a * t + b) * t + c) * t + p1;
}

// One pass per column with only the sub-segments that can reach it. Written
// that way rather than segment by segment because plot puts a colour down
// instead of blending it, so one faint edge drawn second would rub out the
// solid pixel its neighbour had already put there.
void drawPlot(uint16_t *fb, const Market &m, uint16_t ink) {
  if (m.count < 2) {
    return;
  }
  uint16_t subs = (uint16_t)(m.count - 1) * SUB;
  float step = (PLOT_X1 - PLOT_X0) / (float)subs;
  int16_t reach = (int16_t)(STROKE / step) + 2;

  for (int16_t x = (int16_t)PLOT_X0; x <= (int16_t)PLOT_X1; x++) {
    float px = (float)x + 0.5f;
    int16_t mid = (int16_t)((px - PLOT_X0) / step);
    int16_t first = held((int16_t)(mid - reach), (int16_t)(subs - 1));
    int16_t last = held((int16_t)(mid + reach), (int16_t)(subs - 1));

    float top = PLOT_Y1;
    float bottom = PLOT_Y0;
    for (int16_t i = first; i <= last + 1; i++) {
      float at = smoothAt(m, (float)i / (float)SUB);
      top = fminf(top, at);
      bottom = fmaxf(bottom, at);
    }

    // Where the line is at this column, which is what the wash under it hangs
    // from - off the curve rather than off the prices, so the two agree.
    float lit = smoothAt(m, (px - PLOT_X0) / step / (float)SUB);
    float drop = (PLOT_Y1 - lit) * WASH;
    for (int16_t y = (int16_t)lit; y <= (int16_t)(lit + drop); y++) {
      float fell = drop > 0.0f ? ((float)y - lit) / drop : 1.0f;
      float left = 1.0f - fell;
      plot(fb, x, y, WASH_ALPHA * left * left, ink);
    }

    for (int16_t y = (int16_t)(top - STROKE - 1); y <= (int16_t)(bottom + STROKE + 1); y++) {
      float py = (float)y + 0.5f;
      float near = 1e9f;
      for (int16_t i = first; i <= last; i++) {
        float ax = PLOT_X0 + step * (float)i;
        float bx = ax + step;
        near = fminf(near, sdSegment(px, py, ax, smoothAt(m, (float)i / (float)SUB), bx,
                                     smoothAt(m, (float)(i + 1) / (float)SUB)));
      }
      float cover = 0.5f - (near - STROKE);
      if (cover > 0.02f) {
        plot(fb, x, y, cover, ink);
      }
    }
  }
}

void drawSpinner(uint16_t *fb) {
  float cx = (float)SCREEN_R;
  float cy = (float)SCREEN_R;
  float turn = (float)(millis() % SPIN_MS) / (float)SPIN_MS * 2.0f * (float)M_PI;
  // Worked out once. There are fourteen of these and a thousand pixels to ask
  // about each one, and none of it varies across them.
  float ax[SPIN_STEPS + 1];
  float ay[SPIN_STEPS + 1];
  for (uint8_t i = 0; i <= SPIN_STEPS; i++) {
    float a = turn + SPIN_SWEEP * (float)i / (float)SPIN_STEPS;
    ax[i] = cx + cosf(a) * SPIN_R;
    ay[i] = cy + sinf(a) * SPIN_R;
  }

  for (int16_t y = SPIN_TOP; y <= SPIN_BOTTOM; y++) {
    for (int16_t x = (int16_t)cx - SPIN_REACH; x <= (int16_t)cx + SPIN_REACH; x++) {
      float px = (float)x + 0.5f - cx;
      float py = (float)y + 0.5f - cy;
      // Everything but the ring itself thrown out before the segments are asked
      // about, which is the difference between a thousand distances a frame and
      // fifty thousand.
      float out = sqrtf(px * px + py * py);
      if (fabsf(out - SPIN_R) > SPIN_HALF + 2.0f) {
        continue;
      }
      float near = 1e9f;
      for (uint8_t i = 0; i < SPIN_STEPS; i++) {
        near = fminf(near, sdSegment((float)x + 0.5f, (float)y + 0.5f, ax[i], ay[i], ax[i + 1],
                                     ay[i + 1]));
      }
      float cover = 0.5f - (near - SPIN_HALF);
      if (cover > 0.02f) {
        plot(fb, x, y, cover, FAINT);
      }
    }
  }
}

void drawAll(uint16_t *fb, const Market &m, bool coin) {
  clearOwned(fb, OWNED_TOP, (int16_t)(SCREEN_H - 1));
  centred(fb, m.name, NAME_Y, NAME_SCALE, GREY);

  // What is wrong is worth saying; what is merely not here yet is not, and gets
  // the ring instead. Figures without a line is its own state - the list gives
  // a price before the coin's own call gives it a week - and it gets both.
  if (!m.ready) {
    if (m.failed) {
      centred(fb, "NO DATA", PRICE_Y, MOVE_SCALE, FAINT);
    } else {
      drawSpinner(fb);
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

  if (m.count >= 2) {
    drawPlot(fb, m, ink);
    centred(fb, m.span, SPAN_Y, SPAN_SCALE, FAINT);
  } else if (!m.failed) {
    drawSpinner(fb);
  }
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
  if (m.count < 2 && !m.failed) {
    clearOwned(fb, SPIN_TOP, SPIN_BOTTOM);
    drawSpinner(fb);
    boardFlushRows(bandFrom(SPIN_BOTTOM), bandTo(SPIN_TOP));
  }
}
