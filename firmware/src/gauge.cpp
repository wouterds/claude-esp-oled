#include "gauge.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>

#include <string.h>

#include "board.h"
#include "text.h"
#include "usage.h"

namespace {

// Out where the glass is about to stop. The bar follows the edge because it is
// a piece of a circle concentric with it, not because it is bent to fit.
constexpr float RADIUS = 174.0f;
constexpr float HALF_THICK = 3.5f;
// Either side of the horizontal. Short is three quarters of the panel's height,
// which is as far as an arc goes before its bottom end curves back in to where
// the address is written. With the address gone there is nothing down there to
// avoid, so both ends go out together and the bar stays symmetric about the
// horizontal - which is what it looks like it ought to be.
constexpr float SWEEP_SHORT = 0.87f;
constexpr float SWEEP_TOP = 1.13f;
constexpr float SWEEP_BOTTOM = 1.13f;
constexpr float REACH_S = 0.8f;

constexpr int16_t BOX_X0 = 0;
// Far enough in to hold the ends. An arc that reaches down to five past seven
// has its cap at x=131, nowhere near the x=80 that held the short one - and a
// box that stops short of it does not shorten the bar, it guillotines it.
constexpr int16_t BOX_X1 = 115;
constexpr int16_t BOX_Y0 = 18;
constexpr int16_t BOX_Y1 = 342;

constexpr uint16_t TRACK = 0x4A49;

// Where the two numbers go when they are asked for: level with the middle of
// the glass and tucked inside their own bar.
constexpr int16_t FIGURE_X = 46;
// The top of the glyphs, because that is what the font is given - measure the
// box around them from their middle and it comes up four rows short at the
// bottom, which is exactly the part of a digit that survives being cleared.
constexpr int16_t FIGURE_TOP = 173;
constexpr int16_t FIGURE_HEIGHT = 14;
constexpr int16_t FIGURE_HW = 28;

// How fast a bar closes on the number it was given. Exponential rather than a
// timed ease, so a bar already on its way somewhere just bends.
constexpr float APPROACH = 7.0f;
// How long they take to appear. Short: they are furniture, not an event.
constexpr float FADE_S = 0.35f;
// Under this and it is there. Without it a bar rebuilds itself for ever over
// hundredths of a percent nobody can see.
constexpr float ARRIVED = 0.15f;

// What is left when the shapes have been resolved: every pixel a bar puts on
// the glass, with the colour it ends up. About twenty-four hundred each, and
// the cap is only there so a mistake cannot run away with PSRAM. A side has its
// own half of the buffer because either of them can be rebuilt on its own.
struct Pixel {
  int16_t x;
  int16_t y;
  uint16_t colour;
};
constexpr uint16_t SIDE_PIXELS = 6000;

// Off the glass, coming onto it, on it.
enum class Phase : uint8_t { Hidden, Fading, Live };

Pixel *pixels = nullptr;
uint16_t count[2] = {0, 0};
Phase phase = Phase::Hidden;
float alpha = 0.0f;
// Nought while the address has the bottom of the glass, one when it does not.
// Kept twice: the plain count of the way there, and that count smoothed, since
// a length that starts and stops at full speed reads as a jump with a delay in
// the middle of it.
float reachT = 0.0f;
float reach = 0.0f;
// How much of their length they are currently allowed. Only ever below one
// while they are arriving.
float grow = 0.0f;
bool figures = false;
bool figuresMoved = false;
float shown[2] = {0.0f, 0.0f};
float target[2] = {0.0f, 0.0f};
uint32_t lastStep = 0;

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// An arc of RADIUS with round ends, bisected by a direction and reaching an
// aperture either side of it - both handed in already resolved, because working
// them out per pixel is four trig calls to describe a shape that has not moved.
struct Arc {
  float bx, by, s, c;
};

Arc arcAt(float bisector, float aperture) {
  return {cosf(bisector), sinf(bisector), sinf(aperture), cosf(aperture)};
}

// The point is turned so the bisector points along y, and from there it is
// symmetric: inside the aperture the nearest thing is the circle, outside it
// the nearer end.
float sdArc(float px, float py, const Arc &a) {
  float qx = fabsf(px * a.by - py * a.bx);
  float qy = px * a.bx + py * a.by;
  if (a.c * qx > a.s * qy) {
    float ex = qx - a.s * RADIUS;
    float ey = qy - a.c * RADIUS;
    return sqrtf(ex * ex + ey * ey) - HALF_THICK;
  }
  return fabsf(sqrtf(qx * qx + qy * qy) - RADIUS) - HALF_THICK;
}

// Teal at nothing, orange halfway, a warm rose red at full - all of them near
// the top of what the panel can do, because they are being read off a backlight
// that never fully goes out and a dull colour on that is a grey one. The whole
// fill takes one colour off this: the bar says how much by how long it is, and
// says it again by what colour it is, rather than fading along its own length.
uint16_t colourAt(float percent) {
  // The stops are not evenly spaced, because the reading is not: a bar at a
  // seventh of its window has plenty of room and should look like it, and the
  // teal it starts at is the one the battery goes when it is charging. Half way
  // is where it starts to yellow, three fifths is full yellow, seven tenths has
  // gone warm without losing any of it, and amber waits until four fifths - by
  // which point there is something to say.
  struct Stop {
    float at, r, g, b;
  };
  constexpr Stop RAMP[] = {{0.0f, 0.0f, 255.0f, 180.0f},
                           {0.5f, 215.0f, 255.0f, 40.0f},
                           {0.6f, 255.0f, 255.0f, 0.0f},
                           {0.7f, 255.0f, 182.0f, 0.0f},
                           {0.8f, 255.0f, 168.0f, 0.0f},
                           {1.0f, 255.0f, 48.0f, 64.0f}};
  constexpr uint8_t COUNT = sizeof(RAMP) / sizeof(RAMP[0]);

  float t = clamp01(percent / 100.0f);
  uint8_t lo = 0;
  while (lo + 2 < COUNT && t > RAMP[lo + 1].at) {
    lo++;
  }
  float k = (t - RAMP[lo].at) / (RAMP[lo + 1].at - RAMP[lo].at);
  k = clamp01(k);
  float r = RAMP[lo].r + (RAMP[lo + 1].r - RAMP[lo].r) * k;
  float g = RAMP[lo].g + (RAMP[lo + 1].g - RAMP[lo].g) * k;
  float b = RAMP[lo].b + (RAMP[lo + 1].b - RAMP[lo].b) * k;
  // Pinned back to the top of the range. Mixing two colours channel by channel
  // dips through something duller than either of them, and on a panel whose
  // black is a lit backlight, dull is the one thing a colour cannot afford.
  float most = r > g ? (r > b ? r : b) : (g > b ? g : b);
  if (most > 1.0f) {
    float lift = 255.0f / most;
    r *= lift;
    g *= lift;
    b *= lift;
  }
  return (uint16_t)(((uint16_t)(r * 31.0f / 255.0f) << 11) |
                    ((uint16_t)(g * 63.0f / 255.0f) << 5) | (uint16_t)(b * 31.0f / 255.0f));
}

uint16_t shade(uint16_t colour, float coverage) {
  coverage *= alpha;
  uint16_t r = (uint16_t)(((colour >> 11) & 0x1F) * coverage);
  uint16_t g = (uint16_t)(((colour >> 5) & 0x3F) * coverage);
  uint16_t b = (uint16_t)((colour & 0x1F) * coverage);
  return boardColour((uint16_t)((r << 11) | (g << 5) | b));
}

// The fill grows from the bottom end upward, so its own arc is the piece
// between that end and however far along it has got.
// Where the ring crosses one row, which is the only part of it worth looking
// at: every row of the box is 360 wide and holds a dozen pixels of arc.
bool ringSpan(float py, int16_t &x0, int16_t &x1) {
  constexpr float OUTER = RADIUS + HALF_THICK + 1.0f;
  constexpr float INNER = RADIUS - HALF_THICK - 1.0f;
  float across = OUTER * OUTER - py * py;
  if (across <= 0.0f) {
    return false;
  }
  float notch = INNER * INNER - py * py;
  x0 = (int16_t)(SCREEN_R - sqrtf(across));
  x1 = (int16_t)(SCREEN_R - (notch > 0.0f ? sqrtf(notch) : 0.0f)) + 1;
  if (x0 < BOX_X0) {
    x0 = BOX_X0;
  }
  if (x1 > BOX_X1) {
    x1 = BOX_X1;
  }
  return x0 <= x1;
}

void build(uint8_t side, float percent) {
  bool right = side == 1;
  count[side] = 0;
  uint16_t filled = colourAt(percent);
  float fraction = clamp01(percent / 100.0f);

  // Ends measured from the horizontal, and no longer the same as each other, so
  // the arc is bisected off-centre by however much they differ.
  float up = (SWEEP_SHORT + (SWEEP_TOP - SWEEP_SHORT) * reach) * grow;
  float down = (SWEEP_SHORT + (SWEEP_BOTTOM - SWEEP_SHORT) * reach) * grow;
  float span = up + down;
  Arc track = arcAt((float)M_PI + (up - down) * 0.5f, span * 0.5f);
  Arc fill = arcAt((float)M_PI - down + span * fraction * 0.5f, span * fraction * 0.5f);

  for (int16_t y = BOX_Y0; y <= BOX_Y1; y++) {
    float py = (float)y + 0.5f - SCREEN_R;
    int16_t x0;
    int16_t x1;
    if (!ringSpan(py, x0, x1)) {
      continue;
    }

    for (int16_t x = x0; x <= x1; x++) {
      float px = (float)x + 0.5f - SCREEN_R;
      // One shape, drawn twice: the right hand bar is the left one mirrored,
      // and mirroring the point is cheaper than carrying two of everything.
      float d = sdArc(px, py, track);
      uint16_t colour = TRACK;
      if (fraction > 0.0f) {
        float over = sdArc(px, py, fill);
        if (over < 0.5f) {
          d = over;
          colour = filled;
        }
      }
      float coverage = 0.5f - d;
      if (coverage <= 0.02f) {
        continue;
      }
      if (count[side] < SIDE_PIXELS) {
        pixels[side * SIDE_PIXELS + count[side]++] = {
            right ? (int16_t)(SCREEN_W - 1 - x) : x, y, shade(colour, clamp01(coverage))};
      }
    }
  }
}

// Four characters' worth either side of where they are centred, on both edges.
// Cheap enough to do every frame they are up, which is what keeps a 100% that
// has become a 9% from leaving its other two digits behind.
void clearFigures(uint16_t *fb) {
  for (int16_t y = FIGURE_TOP - 2; y <= FIGURE_TOP + FIGURE_HEIGHT + 1; y++) {
    if (y < 0 || y >= SCREEN_H) {
      continue;
    }
    uint16_t *line = boardRow(fb, y);
    size_t bytes = (size_t)(FIGURE_HW * 2 + 1) * 2;
    memset(line + boardX(FIGURE_X + FIGURE_HW), 0, bytes);
    memset(line + boardX(SCREEN_W - 1 - FIGURE_X + FIGURE_HW), 0, bytes);
  }
}

// A bar that has just got shorter leaves its old ends behind, and nothing else
// on the panel is ever going to clear them. Only the ring gets cleared, not the
// box around it: at this width the box would take the address with it.
void wipe(uint16_t *fb) {
  for (int16_t y = BOX_Y0; y <= BOX_Y1; y++) {
    int16_t x0;
    int16_t x1;
    if (!ringSpan((float)y + 0.5f - SCREEN_R, x0, x1)) {
      continue;
    }
    uint16_t *line = boardRow(fb, y);
    size_t bytes = (size_t)(x1 - x0 + 1) * 2;
    memset(line + boardX(x1), 0, bytes);
    memset(line + boardX(SCREEN_W - 1 - x0), 0, bytes);
  }
  clearFigures(fb);
}

}  // namespace

void gaugeBegin() {
  pixels = (Pixel *)heap_caps_malloc(sizeof(Pixel) * SIDE_PIXELS * 2, MALLOC_CAP_SPIRAM);
  if (!pixels) {
    Serial.println("gauges: no room");
    return;
  }
  lastStep = millis();
}

// The rows the bars live in are nowhere near the ones the face moves through,
// so a bar on its way somewhere has to put itself on the panel.
void gaugeStep(uint16_t *fb, uint32_t now) {
  if (!pixels || phase == Phase::Hidden) {
    return;
  }
  float dt = (float)(now - lastStep) * 0.001f;
  lastStep = now;

  if (phase == Phase::Fading) {
    alpha += dt / FADE_S;
    if (alpha >= 1.0f) {
      alpha = 1.0f;
      phase = Phase::Live;
    }
    // Out of the middle as they come up, rather than appearing full length.
    grow = alpha * alpha * (3.0f - 2.0f * alpha);
    wipe(fb);
    build(0, shown[0]);
    build(1, shown[1]);
    gaugeDraw(fb);
    boardFlushRows((int16_t)(SCREEN_H - 1 - BOX_Y1), (int16_t)(SCREEN_H - 1 - BOX_Y0));
    return;
  }

  // Left is the session window, right is the week. Empty until a read comes
  // back, and then they walk to it.
  if (usageReady()) {
    target[0] = (float)usageSession();
    target[1] = (float)usageWeekly();
  }

  // Long once the address has stopped needing the bottom of the glass.
  if (!usageReady() && figures) {
    figures = false;
    figuresMoved = true;
  }
  bool reshape = figuresMoved;
  figuresMoved = false;
  float wanted = usageReady() ? 1.0f : 0.0f;
  if (reachT != wanted) {
    float by = dt / REACH_S;
    reachT = wanted > reachT ? (reachT + by > wanted ? wanted : reachT + by)
                             : (reachT - by < wanted ? wanted : reachT - by);
    reach = reachT * reachT * (3.0f - 2.0f * reachT);
    reshape = true;
  }

  bool moved = false;
  float k = 1.0f - expf(-dt * APPROACH);
  for (uint8_t side = 0; side < 2; side++) {
    float gap = target[side] - shown[side];
    if (fabsf(gap) < ARRIVED) {
      if (shown[side] != target[side]) {
        shown[side] = target[side];
        moved = true;
      }
      continue;
    }
    shown[side] += gap * k;
    moved = true;
  }

  if (!moved && !reshape) {
    return;
  }
  // A bar that just got shorter, or a number that just went away, leaves itself
  // behind otherwise.
  if (reshape) {
    wipe(fb);
  }
  build(0, shown[0]);
  build(1, shown[1]);
  gaugeDraw(fb);
  boardFlushRows((int16_t)(SCREEN_H - 1 - BOX_Y1), (int16_t)(SCREEN_H - 1 - BOX_Y0));
}

void gaugeReveal() {
  if (phase == Phase::Hidden) {
    phase = Phase::Fading;
    lastStep = millis();
  }
}

void gaugeFigures() {
  figures = !figures;
  figuresMoved = true;
}

void gaugeDraw(uint16_t *fb) {
  for (uint8_t side = 0; side < 2; side++) {
    const Pixel *p = pixels + side * SIDE_PIXELS;
    for (uint16_t i = 0; i < count[side]; i++) {
      boardRow(fb, p[i].y)[boardX(p[i].x)] = p[i].colour;
    }
  }
  if (!figures) {
    return;
  }
  clearFigures(fb);
  // Inside the arc at its widest, which is the one part of the panel that is
  // already wiped and put back every frame.
  for (uint8_t side = 0; side < 2; side++) {
    char said[6];
    snprintf(said, sizeof(said), "%u%%", (unsigned)(shown[side] + 0.5f));
    int16_t at = side == 1 ? (int16_t)(SCREEN_W - 1 - FIGURE_X) : FIGURE_X;
    textDraw(fb, said, at, FIGURE_TOP, 2, boardColour(0xFFFF));
  }
}
