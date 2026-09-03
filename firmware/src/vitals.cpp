#include "vitals.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "gauge.h"
#include "nuc.h"
#include "shape.h"
#include "text.h"

namespace {

// The network and charge figures end at 27, so this page starts below them and
// never has to know they are there.
constexpr int16_t OWNED_TOP = (int16_t)(36 * SCENE);

constexpr char TITLE[] = "NUC.WOUTERDS.COM";
constexpr int16_t TITLE_Y = (int16_t)(52 * SCENE);
constexpr int16_t TITLE_SCALE = 2;

// One size of letter on either panel. The bigger glass was given a size up on
// the grounds that the type does not scale with it, and what that actually did
// was fill the page with letters and leave the bars and the chart - the two
// things worth looking at - squeezed in around them.
constexpr int16_t ROW_SCALE = 2;
constexpr float PLOT_Y0 = 72.0f * SCENE;
constexpr float PLOT_Y1 = 142.0f * SCENE;
// The scale the trace is drawn against: nought at the floor and one of these at
// the ceiling. Fixed, and stepped, rather than closed onto whatever the window
// happens to hold - a band that follows its own readings is a band that moves
// under them, and then the whole chart is going up and down when only the line
// should be.
//
// A quarter is the first step because that is the one a box at rest lives in,
// and it leaves an idle wander of a few points as a few rows of movement rather
// than as a flat line along the bottom of the whole hundred.
constexpr float PLOT_SCALES[] = {25.0f, 50.0f, 100.0f};
constexpr uint8_t PLOT_SCALE_COUNT = sizeof(PLOT_SCALES) / sizeof(PLOT_SCALES[0]);
// How far under a step the readings have to fall before the scale drops back to
// it. Without the slack, a box sitting on a boundary flips the scale back and
// forth every second and the trace jumps with it.
constexpr float PLOT_SLACK = 0.85f;
// Half the stroke on the trace. Under a pixel and a half it stops reading as one
// line and starts reading as the pixels it is made of.
constexpr float PLOT_STROKE = 1.15f * SCENE;
// Sub-segments per gap between two readings. The stroke is measured as a distance
// to these rather than as a thickness counted straight down the glass, which is
// what keeps its width the same on a steep piece as on a flat one - and what
// takes the stair off the steep ones.
constexpr uint8_t PLOT_SUB = 6;
constexpr uint16_t PLOT_SEGMENTS = (uint16_t)(NUC_POINTS - 1) * PLOT_SUB;
// The most the wash ever comes to, directly under the trace, falling away to
// nothing at the foot of the band. Squared rather than straight, and a good way
// under half: a straight ramp puts most of its colour into the top of the fill,
// and what a high reading then draws under itself is a slab of green rather
// than a chart.
//
// Taken off the trace rather than off the band. Off the band it is a fixed ramp
// that the trace moves through, which sounds tidier and is not: with the scale
// fixed, a box at rest sits low enough that the whole fill lands in the dim end
// of that ramp and the chart comes out as a bare line.
constexpr float PLOT_WASH = 0.30f;
// The band the trace is clipped out of, and its own rows are all that go to the
// panel between readings.
constexpr int16_t PLOT_TOP = (int16_t)PLOT_Y0 - 2;
constexpr int16_t PLOT_BOTTOM = (int16_t)PLOT_Y1 + 2;

// How often the sliding trace is put back on the glass. Its band is a third of
// the panel and every one of these rows is sent again, so this is bandwidth
// rather than arithmetic - and past about this there is no smoothness left to
// buy.
constexpr uint32_t ANIMATE_MS = 33;
// How fast a bar slides onto a new reading, per animated frame - about a fifth
// of a second to arrive. Readings land about once a second, so anything slower
// than this is a bar that is still moving when the next one turns up.
constexpr float BAR_EASE = 0.18f;
// A bar that moved less than this is one the eye would not catch moving, and
// redrawing it costs the same as redrawing one that did.
constexpr float BAR_MOVED = 0.002f;

// Six rows of label, reading and bar, the load first and the chart above it.
// How far the last one can be pushed down is what settles the pitch: its bar
// reaches out to the margin, and the circle has come in to meet it there.
constexpr uint8_t ROWS = 6;
constexpr int16_t ROW_Y = (int16_t)(152 * SCENE);
constexpr int16_t ROW_PITCH = (int16_t)(24 * SCENE);
// How much of the width the rows leave at either end. The labels start here and
// the bars finish the same distance off the other rim - and whatever is left
// once the type has had its four glyphs of label and six of reading all goes to
// the bar, which is the part of the row that is read at a glance.
constexpr float MARGIN = 0.115f * (float)SCREEN_W;
// Not scaled, unlike the distances around it. The bar is read against the type
// beside it rather than against the glass, and the type is the same size on
// either panel - scaled, it grows into a slab next to letters that did not.
constexpr float BAR_HALF = 3.5f + (float)ROW_SCALE;


constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t GREY = 0x8410;
// The quietest type on the page.
constexpr uint16_t FAINT = 0x4A49;
// What the unfilled part of a bar wears.
constexpr uint16_t TRACK = 0x4A49;

uint32_t shownRev = 0;
uint32_t shownReadings = 0;
bool fresh = true;
// When the last reading landed and how long the one before it took to arrive.
// The poller has no cadence to read off - it asks again the moment the last
// answer lands - so the gap the trace slides across is measured rather than
// assumed, and a reading that comes late leaves it parked at the end of its
// slide rather than running on past where the next one will go.
uint32_t arrivedAt = 0;
float period = 1000.0f;
// Where the trace was in its slide when the last reading landed, in readings.
// Not nought: the readings all move one place along when one lands, so where the
// trace is moves with them. Reset to nought instead and a reading that arrives
// early drags the whole trace back a step, which is what the stutter was.
float base = 0.0f;
float ceilingNow = PLOT_SCALES[0];
uint32_t drewAt = 0;

struct Row {
  const char *label;
  float value;
  const char *how;
  float fill;
};

// Where each bar is drawn to, eased onto what was read rather than jumping to
// it. Kept here rather than worked out per frame because easing is a thing with
// a memory.
float shown[ROWS] = {0};

// The panel is scanned the other way up, so a range of scene rows is the mirror
// of it. Both ends swap with the flip, which is why they cross over.
inline int16_t bandFrom(int16_t to) { return (int16_t)(SCREEN_H - 1 - to); }
inline int16_t bandTo(int16_t from) { return (int16_t)(SCREEN_H - 1 - from); }

int16_t within(int16_t at, int16_t last) { return at < 0 ? 0 : (at > last ? last : at); }

// Not a comparison against nought at either end: a reading the box did not give
// is NaN, and NaN is neither over nor under anything.
uint8_t percentOf(float value) {
  if (!(value > 0.0f)) {
    return 0;
  }
  return value >= 100.0f ? 100 : (uint8_t)(value + 0.5f);
}

uint16_t mix(uint16_t from, uint16_t to, float by) {
  float k = clamp01(by);
  int r = (int)((from >> 11) & 0x1F);
  int g = (int)((from >> 5) & 0x3F);
  int b = (int)(from & 0x1F);
  r += (int)((float)((int)((to >> 11) & 0x1F) - r) * k);
  g += (int)((float)((int)((to >> 5) & 0x3F) - g) * k);
  b += (int)((float)((int)(to & 0x1F) - b) * k);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// A reading the box did not give. Three hyphens rather than a nought, which on
// every one of these lines is a plausible number.
void said(char *out, size_t size, float value, const char *how) {
  if (isnan(value)) {
    snprintf(out, size, "---");
    return;
  }
  snprintf(out, size, how, (double)value);
}

// textDraw takes the middle and already allows for the last glyph carrying no
// gap after it, so an edge is that middle plus half of what is about to go down.
int16_t inkWidth(const char *s, int16_t scale) {
  size_t n = strlen(s);
  return n ? (int16_t)(n * (size_t)textStep(scale) - (size_t)scale) : 0;
}

void centred(uint16_t *fb, const char *s, int16_t top, int16_t scale, uint16_t ink) {
  textDraw(fb, s, (int16_t)SCREEN_R, top, scale, boardColour(ink));
}

void leftAt(uint16_t *fb, const char *s, int16_t x, int16_t top, int16_t scale, uint16_t ink) {
  textDraw(fb, s, (int16_t)(x + inkWidth(s, scale) / 2), top, scale, boardColour(ink));
}

void rightAt(uint16_t *fb, const char *s, int16_t x, int16_t top, int16_t scale, uint16_t ink) {
  textDraw(fb, s, (int16_t)(x - inkWidth(s, scale) / 2), top, scale, boardColour(ink));
}

// Where a row's parts sit. Laid out from the type rather than from the glass:
// four glyphs of label and six of reading are the widest either ever gets, and
// where the bar starts is where those two have finished.
inline int16_t labelLeft() { return (int16_t)MARGIN; }
inline int16_t valueRight() { return (int16_t)(labelLeft() + 11 * textStep(ROW_SCALE)); }
inline float barLeft() { return (float)(valueRight() + textStep(ROW_SCALE)); }
inline float barRight() { return (float)SCREEN_W - MARGIN; }
inline int16_t rowTop(uint8_t i) { return (int16_t)(ROW_Y + ROW_PITCH * (int16_t)i); }
inline float rowMiddle(uint8_t i) {
  return (float)rowTop(i) + (float)(7 * ROW_SCALE) * 0.5f;
}

// One bar's own box, which is all that is cleared and sent while it is moving.
void clearBox(uint16_t *fb, int16_t x0, int16_t x1, int16_t from, int16_t to) {
  for (int16_t y = from; y <= to; y++) {
    uint16_t *row = boardRow(fb, y);
    for (int16_t x = x0; x <= x1; x++) {
      row[boardX(x)] = 0;
    }
  }
}

void clearOwned(uint16_t *fb, int16_t from, int16_t to) {
  for (int16_t y = from; y <= to; y++) {
    memset(boardRow(fb, y), 0, (size_t)SCREEN_W * 2);
  }
}

// A rounded track with the left of it filled.
//
// The fill is blended into the track rather than drawn over it: plot writes a
// colour instead of mixing one in, so a soft edge laid over the track fades to
// black and leaves a dark seam down the end of every one of these.
void bar(uint16_t *fb, float cx, float cy, float hx, float hy, float fill, uint16_t ink) {
  float part = (fill > 0.0f) ? clamp01(fill) : 0.0f;
  float r = fminf(hx, hy);
  float fhx = hx * part;
  float fcx = cx - hx + fhx;
  float fr = fminf(r, fminf(fhx, hy));

  for (int16_t y = (int16_t)(cy - hy) - 1; y <= (int16_t)(cy + hy) + 1; y++) {
    float py = (float)y + 0.5f;
    for (int16_t x = (int16_t)(cx - hx) - 1; x <= (int16_t)(cx + hx) + 1; x++) {
      float px = (float)x + 0.5f;
      float on = 0.5f - sdRoundBox(px - cx, py - cy, hx, hy, r);
      if (on <= 0.02f) {
        continue;
      }
      float lit = part > 0.0f ? clamp01(0.5f - sdRoundBox(px - fcx, py - cy, fhx, hy, fr))
                              : 0.0f;
      plot(fb, x, y, on, mix(TRACK, ink, lit));
    }
  }
}

// How steep the curve is as it passes a reading. Flat at a turning point, and
// never steeper than the sides around it will carry.
//
// This is what rounds the corners. Catmull-Rom was here first: it takes its
// slope from the neighbours either side, which at the top of a peak is not flat,
// so it ran up past the reading and had to be held back to it - and being held
// back is a corner. A tangent that goes flat where the readings turn rounds the
// peak instead, and never overshoots in the first place, so nothing has to be
// clamped afterwards.
float slopeAt(const Nuc &n, int16_t i, int16_t last) {
  float before = n.load[within(i, last)] - n.load[within((int16_t)(i - 1), last)];
  float after = n.load[within((int16_t)(i + 1), last)] - n.load[within(i, last)];
  if (before * after <= 0.0f) {
    return 0.0f;
  }
  float slope = (before + after) * 0.5f;
  float limit = 3.0f * fminf(fabsf(before), fabsf(after));
  return slope > limit ? limit : (slope < -limit ? -limit : slope);
}

// A cubic through two readings that leaves each one at the slope above, so it
// passes through every reading rather than near it and rounds only what is
// between them.
float smoothAt(const Nuc &n, float at) {
  int16_t last = (int16_t)n.count - 1;
  int16_t i = within((int16_t)floorf(at), (int16_t)(last - 1));
  float t = clamp01(at - (float)i);
  float p1 = n.load[within(i, last)];
  float p2 = n.load[within((int16_t)(i + 1), last)];
  float m1 = slopeAt(n, i, last);
  float m2 = slopeAt(n, (int16_t)(i + 1), last);
  float t2 = t * t;
  float t3 = t2 * t;
  return (2.0f * t3 - 3.0f * t2 + 1.0f) * p1 + (t3 - 2.0f * t2 + t) * m1 +
         (3.0f * t2 - 2.0f * t3) * p2 + (t3 - t2) * m2;
}

// Where the curve sits at every sub-step, in panel rows. Worked out once a frame
// rather than per pixel: there are a couple of hundred of these and several
// thousand pixels that would otherwise ask for each one again.
float rows[PLOT_SEGMENTS + 1];

// The wash under the trace and the trace over it. One pass per column with only
// the sub-segments that can reach that column asked about - written that way
// rather than segment by segment because plot puts a colour down instead of
// blending it, so one faint edge drawn second would rub out the solid pixel its
// neighbour had already put there.
void drawLoad(uint16_t *fb, const Nuc &n, float slide) {
  if (n.count < 2) {
    return;
  }
  // A reading's width is fixed rather than shared out among however many there
  // are, so the trace scrolls at one speed from the first reading to the last
  // and a window still filling grows leftward instead of rescaling under the eye.
  float step = (float)(SCREEN_W - 1) / (float)(NUC_POINTS - 1);
  float sub = step / (float)PLOT_SUB;
  float band = PLOT_Y1 - PLOT_Y0;
  uint16_t ink = gaugeColour(percentOf(n.load[n.count - 1]));

  uint16_t segments = (uint16_t)(n.count - 1) * PLOT_SUB;
  for (uint16_t k = 0; k <= segments; k++) {
    float value = smoothAt(n, (float)k / (float)PLOT_SUB);
    rows[k] = PLOT_Y1 - clamp01(value / ceilingNow) * band;
  }

  // Where the oldest reading sits, less how far the trace has slid since the
  // newest one landed.
  float from = (float)(SCREEN_W - 1) - ((float)(n.count - 1) + slide) * step;
  int16_t reach = (int16_t)(PLOT_STROKE / sub) + 2;
  int16_t last = (int16_t)segments - 1;

  for (int16_t x = 0; x < SCREEN_W; x++) {
    float px = (float)x + 0.5f;
    float at = (px - from) / sub;
    // Before the oldest reading there is no history, so there is no trace.
    if (at < 0.0f || at > (float)segments) {
      continue;
    }
    int16_t mid = (int16_t)at;
    int16_t first = within((int16_t)(mid - reach), last);
    int16_t upto = within((int16_t)(mid + reach), last);

    // The wash hangs off where the trace is at this column, off a ramp that runs
    // down the band rather than down from the trace.
    int16_t i = within(mid, last);
    float lit = rows[i] + (rows[i + 1] - rows[i]) * clamp01(at - (float)i);
    float drop = PLOT_Y1 - lit;
    for (int16_t y = (int16_t)lit; y <= (int16_t)PLOT_Y1; y++) {
      float left = 1.0f - (drop > 0.0f ? ((float)y + 0.5f - lit) / drop : 1.0f);
      plot(fb, x, y, 1.0f, mix(0x0000, ink, PLOT_WASH * left * left));
    }

    float top = PLOT_Y1;
    float bottom = PLOT_Y0;
    for (int16_t k = first; k <= upto + 1; k++) {
      top = fminf(top, rows[k]);
      bottom = fmaxf(bottom, rows[k]);
    }
    for (int16_t y = (int16_t)(top - PLOT_STROKE - 1); y <= (int16_t)(bottom + PLOT_STROKE + 1);
         y++) {
      float py = (float)y + 0.5f;
      float near = 1e9f;
      for (int16_t k = first; k <= upto; k++) {
        float ax = from + sub * (float)k;
        near = fminf(near, sdSegment(px, py, ax, rows[k], ax + sub, rows[k + 1]));
      }
      // A distance rather than a thickness counted down the glass, so the stroke
      // is the same width however steep the piece is and its edge comes out
      // smooth without a second pass over it.
      float cover = 0.5f - (near - PLOT_STROKE);
      if (cover > 0.02f) {
        plot(fb, x, y, cover, ink);
      }
    }
  }
}

void rowsOf(const Nuc &n, Row *out) {
  // Temperatures against the hundred degrees that is the end of them, which is
  // the scale the box's own page puts them on.
  out[0] = {"CPU", n.cpu, "%.1f%%", n.cpu * 0.01f};
  out[1] = {"CORE", n.coreTemp, "%.0fC", n.coreTemp * 0.01f};
  out[2] = {"NVME", n.nvmeTemp, "%.0fC", n.nvmeTemp * 0.01f};
  out[3] = {"MEM", n.memory, "%.1f%%", n.memory * 0.01f};
  out[4] = {"DISK", n.disk, "%.1f%%", n.disk * 0.01f};
  out[5] = {"PWR", n.power, "%.1fW", n.power / n.powerPeak};
}

void drawRowBar(uint16_t *fb, uint8_t i, float fill) {
  bar(fb, (barLeft() + barRight()) * 0.5f, rowMiddle(i), (barRight() - barLeft()) * 0.5f,
      BAR_HALF, fill, gaugeColour(percentOf(fill * 100.0f)));
}

void drawAll(uint16_t *fb, const Nuc &n, float slide) {
  clearOwned(fb, OWNED_TOP, (int16_t)(SCREEN_H - 1));
  centred(fb, TITLE, TITLE_Y, TITLE_SCALE, GREY);

  char text[24];
  drawLoad(fb, n, slide);
  // What is wrong is worth saying; what is merely not here yet is not, and the
  // empty bars say that better than a word would.
  if (!n.ready && n.failed) {
    centred(fb, "NO DATA", (int16_t)(0.5f * (PLOT_Y0 + PLOT_Y1) - 3.5f * (float)ROW_SCALE),
            ROW_SCALE, FAINT);
  }

  Row rows[ROWS];
  rowsOf(n, rows);
  for (uint8_t i = 0; i < ROWS; i++) {
    leftAt(fb, rows[i].label, labelLeft(), rowTop(i), ROW_SCALE, GREY);
    said(text, sizeof(text), rows[i].value, rows[i].how);
    rightAt(fb, text, valueRight(), rowTop(i), ROW_SCALE, WHITE);
    // Off what the bar has eased to rather than what was read, so the full
    // redraw and the frames that only move a bar agree about where it is.
    drawRowBar(fb, i, shown[i]);
  }
}

}  // namespace

void vitalsForget() { fresh = true; }

void vitalsStep(uint16_t *fb) {
  uint32_t now = millis();
  // How far the trace has slid since the last reading landed, in readings.
  float slide = arrivedAt ? base + (float)(now - arrivedAt) / period : 0.0f;
  slide = slide > 1.0f ? 1.0f : (slide < -1.0f ? -1.0f : slide);

  uint32_t rev = nucRevision();
  uint32_t taken = nucReadings();
  if (taken != shownReadings) {
    shownReadings = taken;
    // What the next slide has to last. Bounded either side of anything a round
    // trip plausibly takes, so one reading lost to a stall does not stretch the
    // slide out to a crawl for the ones after it.
    if (arrivedAt) {
      float gap = (float)(now - arrivedAt);
      if (gap > 60.0f && gap < 8000.0f) {
        period = period * 0.75f + gap * 0.25f;
      }
    }
    // Every reading moved one place along, so the trace starts this slide one
    // place further on rather than back at the beginning of it. Off the count of
    // readings rather than the revision: a read that failed moves the revision
    // and adds nothing to the history, and sliding on for that is the trace
    // stepping backwards for a reading that never came.
    base = slide - 1.0f;
    arrivedAt = now;
    slide = base;
  }

  Nuc n;
  nucLatest(&n);
  float peak = 0.0f;
  for (uint8_t i = 0; i < n.count; i++) {
    peak = fmaxf(peak, n.load[i]);
  }
  // The smallest step that holds the tallest reading. Up the moment one needs
  // it, and back down only once they have fallen well clear of the step below.
  float want = PLOT_SCALES[PLOT_SCALE_COUNT - 1];
  for (uint8_t i = 0; i < PLOT_SCALE_COUNT; i++) {
    if (peak <= PLOT_SCALES[i]) {
      want = PLOT_SCALES[i];
      break;
    }
  }
  if (want < ceilingNow && peak > want * PLOT_SLACK) {
    want = ceilingNow;
  }
  ceilingNow = want;
  Row bars[ROWS];
  rowsOf(n, bars);
  if (fresh) {
    for (uint8_t i = 0; i < ROWS; i++) {
      shown[i] = (bars[i].fill > 0.0f) ? clamp01(bars[i].fill) : 0.0f;
    }
  }

  if (fresh || rev != shownRev) {
    fresh = false;
    shownRev = rev;
    drewAt = now;
    drawAll(fb, n, slide);
    boardFlushRows(bandFrom((int16_t)(SCREEN_H - 1)), bandTo(OWNED_TOP));
    return;
  }
  // Nothing else on the page has moved, so nothing else is cleared or sent.
  if (now - drewAt < ANIMATE_MS) {
    return;
  }
  drewAt = now;
  clearOwned(fb, PLOT_TOP, PLOT_BOTTOM);
  drawLoad(fb, n, slide);
  boardFlushRows(bandFrom(PLOT_BOTTOM), bandTo(PLOT_TOP));

  // Each bar eases onto its reading, and only the ones that actually moved are
  // cleared and sent - a box apiece, because the rest of the row has not.
  int16_t x0 = (int16_t)barLeft() - 1;
  int16_t x1 = (int16_t)barRight() + 1;
  for (uint8_t i = 0; i < ROWS; i++) {
    float target = (bars[i].fill > 0.0f) ? clamp01(bars[i].fill) : 0.0f;
    float was = shown[i];
    shown[i] += (target - shown[i]) * BAR_EASE;
    if (fabsf(shown[i] - was) < BAR_MOVED) {
      continue;
    }
    int16_t from = (int16_t)(rowMiddle(i) - BAR_HALF) - 1;
    int16_t to = (int16_t)(rowMiddle(i) + BAR_HALF) + 1;
    clearBox(fb, x0, x1, from, to);
    drawRowBar(fb, i, shown[i]);
    boardFlushRect(x0, x1, bandFrom(to), bandTo(from));
  }
}
