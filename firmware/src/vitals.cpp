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
constexpr int16_t TITLE_Y = (int16_t)(48 * SCENE);
constexpr int16_t TITLE_SCALE = 2;

// One size of letter on either panel. The bigger glass was given a size up on
// the grounds that the type does not scale with it, and what that actually did
// was fill the page with letters and leave the bars and the chart - the two
// things worth looking at - squeezed in around them.
constexpr int16_t ROW_SCALE = 2;
constexpr float PLOT_Y0 = 68.0f * SCENE;
constexpr float PLOT_Y1 = 144.0f * SCENE;
// The band holds the readings that are in it and nothing else - the lowest at
// the floor, the highest at the ceiling - so what it draws is the shape of the
// change rather than the size of the number. Anchored at nought instead, a box
// wandering between seven and eleven per cent has its whole day squeezed into
// the top twentieth of the band and reads as a flat line.
//
// What that cannot say is how much load it is. The row below says it, against
// the whole hundred, and so does the colour off the one ramp - so the chart is
// free to be about the shape.
//
// A window that never moved still has to be a band rather than a rule, so it is
// never given less than this to hold, and never opened right to the edges. It is
// not a small number: closed right down onto a box wandering between seven and
// eleven per cent, four points of noise fill the whole band and every twitch
// reads as an event.
constexpr float PLOT_SPAN = 12.0f;
constexpr float PLOT_HEADROOM = 0.12f;
// The trace, drawn as a stroke of its own the way the market charts draw theirs -
// a solid width of full colour with the wash hung under it, rather than the top
// of the wash left to stand in for a line. Under a pixel and a half it stops
// reading as one line and starts reading as the pixels it is made of.
constexpr float PLOT_LINE = 2.2f * SCENE;
// The most the wash under the trace ever comes to. It hangs off the trace rather
// than off the band, so every column has the same fill directly beneath it -
// which is what the market charts do, and taken off the band instead a column
// reading low gets a darker fill than one reading high for no reason anybody
// could name.
constexpr float PLOT_WASH = 0.40f;
// The band the trace is clipped out of, and its own rows are all that go to the
// panel between readings.
constexpr int16_t PLOT_TOP = (int16_t)PLOT_Y0 - 2;
constexpr int16_t PLOT_BOTTOM = (int16_t)PLOT_Y1 + 2;

// How fast the band closes back in on the readings once a tall one has walked
// off the end, per frame. It opens at once and no slower: a band that eases open
// clips the very reading it is making room for, and what that draws is a flat
// top on the one shape worth looking at. Closing there is nothing to clip, so it
// can take its time and the chart settles instead of snapping.
constexpr float BAND_EASE = 0.04f;
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
constexpr int16_t ROW_Y = (int16_t)(158 * SCENE);
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
float lowNow = 0.0f;
float highNow = 0.0f;
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

// Catmull-Rom through the readings, at a fractional index between them. It
// passes through every one of them rather than near them, so only the corners
// between readings are rounded - which is rounding, not smoothing, and the
// difference matters when what is drawn is what the box was actually doing.
float smoothAt(const Nuc &n, float at) {
  int16_t last = (int16_t)n.count - 1;
  int16_t i = (int16_t)floorf(at);
  float t = at - (float)i;
  float p0 = n.load[within((int16_t)(i - 1), last)];
  float p1 = n.load[within(i, last)];
  float p2 = n.load[within((int16_t)(i + 1), last)];
  float p3 = n.load[within((int16_t)(i + 2), last)];
  float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
  float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
  float c = -0.5f * p0 + 0.5f * p2;
  float out = ((a * t + b) * t + c) * t + p1;
  // Held between the two readings it sits between. Left to itself this overshoots
  // either side of a step - a spike above a peak and a dip under it, neither of
  // which the box ever did - and on a trace of one machine's load those read as
  // readings rather than as the arithmetic they are.
  float low = fminf(p1, p2);
  float high = fmaxf(p1, p2);
  return out < low ? low : (out > high ? high : out);
}

// The area under the trace, one column of pixels at a time. Walked rather than
// stroked: there is no distance to take a square root of and nothing is drawn
// over anything, which is what makes it cheap enough to put back every frame.
void drawLoad(uint16_t *fb, const Nuc &n, float slide) {
  if (n.count < 2) {
    return;
  }
  float x0 = 0.0f;
  float x1 = (float)(SCREEN_W - 1);
  // A reading's width is fixed rather than shared out among however many there
  // are, so the trace scrolls at one speed from the first reading to the last
  // and a window still filling grows leftward instead of rescaling under the eye.
  float step = (x1 - x0) / (float)(NUC_POINTS - 1);
  float band = PLOT_Y1 - PLOT_Y0;
  float span = highNow - lowNow;
  uint16_t ink = gaugeColour(percentOf(n.load[n.count - 1]));

  for (int16_t x = (int16_t)x0; x <= (int16_t)x1; x++) {
    float px = (float)x + 0.5f;
    // Where the newest reading sits, less how far the trace has slid since it
    // landed. Beyond the oldest there is no history, so there is no trace.
    float at = (float)(n.count - 1) + slide - (x1 - px) / step;
    if (at < 0.0f) {
      continue;
    }
    float top = PLOT_Y1 - clamp01((smoothAt(n, at) - lowNow) / span) * band;
    float drop = PLOT_Y1 - top;

    for (int16_t y = (int16_t)top - 1; y <= (int16_t)PLOT_Y1; y++) {
      float under = (float)y + 0.5f - top;
      if (under < -0.5f) {
        continue;
      }
      float left = 1.0f - (drop > 0.0f ? under / drop : 1.0f);
      // Full colour to the width of the stroke and a pixel to fade out over,
      // then the wash the rest of the way down.
      plot(fb, x, y, clamp01(under + 0.5f),
           mix(mix(0x0000, ink, PLOT_WASH * left * left), ink,
               clamp01(PLOT_LINE - under + 0.5f)));
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
  float low = n.count ? n.load[0] : 0.0f;
  float high = low;
  for (uint8_t i = 1; i < n.count; i++) {
    low = fminf(low, n.load[i]);
    high = fmaxf(high, n.load[i]);
  }
  // Never less than a band to hold, and never right to the edges of it.
  float middle = (low + high) * 0.5f;
  float half = fmaxf((high - low) * (0.5f + PLOT_HEADROOM), PLOT_SPAN * 0.5f);
  low = middle - half;
  high = middle + half;
  Row rows[ROWS];
  rowsOf(n, rows);
  if (fresh) {
    lowNow = low;
    highNow = high;
    for (uint8_t i = 0; i < ROWS; i++) {
      shown[i] = (rows[i].fill > 0.0f) ? clamp01(rows[i].fill) : 0.0f;
    }
  }
  lowNow = low < lowNow ? low : lowNow + (low - lowNow) * BAND_EASE;
  highNow = high > highNow ? high : highNow + (high - highNow) * BAND_EASE;

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
    float want = (rows[i].fill > 0.0f) ? clamp01(rows[i].fill) : 0.0f;
    float was = shown[i];
    shown[i] += (want - shown[i]) * BAR_EASE;
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
