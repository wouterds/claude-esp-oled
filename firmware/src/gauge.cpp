#include "gauge.h"

#include <Preferences.h>

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>

#include <string.h>

#include "board.h"
#include "text.h"
#include "usage.h"

namespace {

// Out where the glass is about to stop, and a pixel short of where it could be.
// The bar follows the edge because it is a piece of a circle concentric with it,
// not because it is bent to fit. This is the centreline: ink reaches this plus
// HALF_THICK, and its coverage a pixel past that again, so the standoff from the
// edge is smaller than this number makes it look and is meant to be there.
constexpr float RADIUS = 173.0f * SCENE;
constexpr float HALF_THICK = 3.5f * SCENE;
// Either side of the horizontal. Short reaches about three quarters of the way
// down the panel: its bottom end lands on row 317, which is past where the
// address and the clock are written, but by then the arc has curved in to a
// hundred and six pixels off the middle and the longest line down there reaches
// ninety - so it goes over those rows without going through anything. With the
// address gone both ends go out together and the bar stays symmetric about the
// horizontal, which is what it looks like it ought to be.
// The AMOLED board gets a tenth more of the short one: the line at the bottom
// is the same size of type on a bigger glass, so it claims less of the circle
// and the caps have further to go before they crowd it. By eye, as ever.
constexpr float SWEEP_SHORT = SCENE > 1.0f ? 1.0f : 0.91f;
constexpr float SWEEP_TOP = 1.13f;
constexpr float SWEEP_BOTTOM = 1.13f;
// How long the bars take to pull in behind the numbers, and to come back out
// again. Both ends of that ride this one figure, so they stay each other's
// reverse rather than drifting apart.
constexpr float REACH_S = 0.53f;

constexpr int16_t BOX_X0 = 0;
// Far enough in to hold the ends. An arc that reaches down to five past seven
// has its cap at x=131, nowhere near the x=80 that held the short one - and a
// box that stops short of it does not shorten the bar, it guillotines it.
//
// This is the box the bars are built inside, wiped inside and flushed from, so
// on a larger panel it has to grow with the arc it holds or it does all three
// to a bar that is longer than it: the ends are never drawn, and the rows past
// it are never cleared, so whatever the bar last put there stays on the glass.
constexpr int16_t BOX_X1 = (int16_t)(115 * SCENE);
constexpr int16_t BOX_Y0 = (int16_t)(18 * SCENE);
constexpr int16_t BOX_Y1 = (int16_t)(342 * SCENE);

constexpr uint16_t TRACK = 0x4A49;

// Where the two numbers go when they are asked for: level with the middle of
// the glass and tucked inside their own bar. Held off the bar rather than
// scaled from the middle: the glyphs are the same size on either glass, and
// scaled, they floated away from the bar they name. Thirty-nine in from the
// bar's centreline - which on the LCD board is the 46 it always was.
constexpr int16_t FIGURE_X = (int16_t)(SCREEN_R - RADIUS + 39.0f);
// The top of the glyphs, because that is what the font is given - measure the
// box around them from their middle and it comes up four rows short at the
// bottom, which is exactly the part of a digit that survives being cleared.
// Seven above the middle, and unscaled below it, because the type is.
constexpr int16_t FIGURE_TOP = (int16_t)(SCREEN_R - 7.0f);
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
//
// It goes up with the area rather than the radius: a bar on a larger panel is
// both longer and thicker. Reached, the cap does not warn - it stops adding
// pixels, and the bar simply ends where the count ran out.
struct Pixel {
  int16_t x;
  int16_t y;
  uint16_t colour;
};
constexpr uint16_t SIDE_PIXELS = (uint16_t)(6000 * SCENE * SCENE);

// Off the glass, coming onto it, on it.
enum class Phase : uint8_t { Hidden, Fading, Live };

Pixel *pixels = nullptr;
uint16_t count[2] = {0, 0};
// The box each side's pixels actually land in. A bar is a thin arc against one
// edge of the glass and the face's box is a fifth of the panel across, so on
// almost every frame the two do not meet at all - and the cheapest way to put
// back the part of a bar that was painted over is to notice, in two comparisons,
// that none of it was.
int16_t edgeX0[2] = {0, 0};
int16_t edgeX1[2] = {0, 0};
int16_t edgeY0[2] = {0, 0};
int16_t edgeY1[2] = {0, 0};
Phase phase = Phase::Hidden;
float alpha = 0.0f;
// Nought while the address has the bottom of the glass, one when it does not.
// Kept twice: the plain count of the way there, and that count smoothed, since
// a length that starts and stops at full speed reads as a jump with a delay in
// the middle of it.
float reachT = 0.0f;
float reach = 0.0f;
// The numbers' own way onto the glass, kept the same way and for the same
// reason - and kept apart from the bars' because the two only agree once there
// is something to show.
float figuresT = 0.0f;
float figuresLevel = 0.0f;
// How much of their length they are currently allowed. Only ever below one
// while they are arriving.
float grow = 0.0f;
// What the button was last left saying, which outlives the power, and what is
// actually up - which needs a reading to put in it and so is not the same
// thing. Keeping them apart is what lets the choice survive a boot: the first
// frames have no numbers yet, and a single flag would be turned off by that and
// never turned back.
Preferences store;
bool wanted = false;
bool figures = false;
bool figuresMoved = false;
float shown[2] = {0.0f, 0.0f};
// What each side still has to put on the panel, and whose turn it is when both
// do. A bar's rebuild and its sends are most of a frame between them, so two
// bars walking at once take a frame each rather than sharing one - thirty
// times a second apiece, which a bar cannot be seen to do, in place of the face
// dropping to twenty for as long as they move.
bool owed[2] = {false, false};
// And whether what it owes is all of itself: its shape has changed, so what is
// on the glass has to come off first.
bool reshaped[2] = {false, false};
uint8_t turn = 0;
// The reading each side was last sent for, which is also the one its number
// says: the two sides take turns, and a number redrawn for a reading its bar
// has not been sent for yet is a number the next flush of the face sends half
// of.
float sent[2] = {0.0f, 0.0f};
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

// Where a pixel stops being all bar and where it stops being any, as distances
// from the middle of the stroke: the edge is a pixel wide and centred on it.
constexpr float ALL_IN = HALF_THICK - 0.5f;
constexpr float ALL_OUT = HALF_THICK + 0.5f;

// How much of a pixel the arc covers. The point is turned so the bisector
// points along y, and from there it is symmetric: inside the aperture the
// nearest thing is the circle, outside it the nearer end.
//
// The distance is only taken on the edge. A root is a library call on this
// chip and a bar is mostly its own middle, where the answer is one whatever the
// distance comes to - and with one root for the track and another for the fill
// on every pixel, a bar on its way somewhere cost three frames for each one it
// drew. The squares say which side of the edge a pixel is on for nothing.
float arcCover(float px, float py, const Arc &a) {
  float qx = fabsf(px * a.by - py * a.bx);
  float qy = px * a.bx + py * a.by;
  if (a.c * qx > a.s * qy) {
    float ex = qx - a.s * RADIUS;
    float ey = qy - a.c * RADIUS;
    float d2 = ex * ex + ey * ey;
    if (d2 <= ALL_IN * ALL_IN) {
      return 1.0f;
    }
    return d2 >= ALL_OUT * ALL_OUT ? 0.0f : ALL_OUT - sqrtf(d2);
  }
  // A turn keeps a length, so the point's own will do.
  float r2 = px * px + py * py;
  if (r2 >= (RADIUS - ALL_IN) * (RADIUS - ALL_IN) && r2 <= (RADIUS + ALL_IN) * (RADIUS + ALL_IN)) {
    return 1.0f;
  }
  if (r2 <= (RADIUS - ALL_OUT) * (RADIUS - ALL_OUT) ||
      r2 >= (RADIUS + ALL_OUT) * (RADIUS + ALL_OUT)) {
    return 0.0f;
  }
  return ALL_OUT - fabsf(sqrtf(r2) - RADIUS);
}

// Whether a pixel of the track is in the part of it that is coloured in. Only
// asked of a pixel the track covers, and the fill is a piece of the track: so
// between the fill's ends the answer is already yes, and past one it is whether
// the pixel is inside that end's cap - neither of which wants a root.
bool inFill(float px, float py, const Arc &a) {
  float qx = fabsf(px * a.by - py * a.bx);
  float qy = px * a.bx + py * a.by;
  if (a.c * qx <= a.s * qy) {
    return true;
  }
  float ex = qx - a.s * RADIUS;
  float ey = qy - a.c * RADIUS;
  return ex * ex + ey * ey < ALL_OUT * ALL_OUT;
}

// Teal at nothing, orange halfway, and #ED0040 at full - the same red the frame
// rate's badge wears, so one red means one thing. It arrives a shade brighter
// than that: the lift at the bottom of this puts the strongest channel back at
// full scale, which is a pure scale and leaves the hue exactly where it was. All
// of them near the top of what the panel can do, because they are being read off
// a backlight that never fully goes out and a dull colour on that is a grey one. The whole
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
                           {1.0f, 237.0f, 0.0f, 64.0f}};
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

// How far the bar reaches above the horizontal and below it. Not the same as
// each other any more, so the arc is bisected off-centre by however much they
// differ.
void sweep(float &up, float &down) {
  up = (SWEEP_SHORT + (SWEEP_TOP - SWEEP_SHORT) * reach) * grow;
  down = (SWEEP_SHORT + (SWEEP_BOTTOM - SWEEP_SHORT) * reach) * grow;
}

// The highest row a fill of this much reaches. The fill grows from the bottom
// end upward and is one colour throughout, so a reading that moves changes
// every row from here down and none above it.
int16_t fillTop(float percent) {
  float up;
  float down;
  sweep(up, down);
  float above = (up + down) * clamp01(percent / 100.0f) - down;
  return (int16_t)(SCREEN_R - RADIUS * sinf(above) - ALL_OUT - 2.0f);
}

void build(uint8_t side, float percent) {
  bool right = side == 1;
  count[side] = 0;
  edgeX0[side] = SCREEN_W;
  edgeX1[side] = -1;
  edgeY0[side] = SCREEN_H;
  edgeY1[side] = -1;
  uint16_t filled = colourAt(percent);
  float fraction = clamp01(percent / 100.0f);
  // Most of a bar is fully covered, and those pixels are one of two colours.
  const uint16_t whole[2] = {shade(TRACK, 1.0f), shade(filled, 1.0f)};

  float up;
  float down;
  sweep(up, down);
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
      // The fill is the same bar with part of it coloured in, not a second
      // shape laid over the first. Taken as its own distance the coverage falls
      // through nothing for the pixel either side of its cap - a black crack
      // across a bar that is solid underneath - so the silhouette stays the
      // track's throughout and only the colour crosses over.
      float coverage = arcCover(px, py, track);
      if (coverage <= 0.02f) {
        continue;
      }
      // The colour crosses on the fill's own edge rather than on a fade of it.
      // Both distances fall away together across the bar's width, so a fade
      // driven by one of them greys off every antialiased pixel down both long
      // edges of the fill - which leaves it fringed rather than filled, and the
      // fringe shifts along the curve as the bar bends.
      bool lit = fraction > 0.0f && inFill(px, py, fill);
      if (count[side] < SIDE_PIXELS) {
        int16_t at = right ? (int16_t)(SCREEN_W - 1 - x) : x;
        uint16_t colour = coverage >= 1.0f ? whole[lit] : shade(lit ? filled : TRACK, coverage);
        pixels[side * SIDE_PIXELS + count[side]++] = {at, y, colour};
        if (at < edgeX0[side]) edgeX0[side] = at;
        if (at > edgeX1[side]) edgeX1[side] = at;
        if (y < edgeY0[side]) edgeY0[side] = y;
        if (y > edgeY1[side]) edgeY1[side] = y;
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
void wipe(uint16_t *fb, uint8_t side) {
  for (int16_t y = BOX_Y0; y <= BOX_Y1; y++) {
    int16_t x0;
    int16_t x1;
    if (!ringSpan((float)y + 0.5f - SCREEN_R, x0, x1)) {
      continue;
    }
    size_t bytes = (size_t)(x1 - x0 + 1) * 2;
    memset(boardRow(fb, y) + boardX(side == 0 ? x1 : (int16_t)(SCREEN_W - 1 - x0)), 0, bytes);
  }
  clearFigures(fb);
}

// The ring cut into bands of rows, each only as wide as the piece of arc that
// crosses it. A bar is a dozen pixels thick and bows a hundred and forty across
// its length, so the one box that holds it is three quarters empty - and the
// wire carries about eight kilobytes a millisecond, which made a bar walking
// somewhere the most expensive thing this board ever did. Not many bands, all
// the same: each send costs most of a millisecond before its first pixel, and
// past eight the calls cost more than the rows they save.
constexpr uint8_t BANDS = 8;
constexpr int16_t BAND_ROWS = (BOX_Y1 - BOX_Y0 + 1 + BANDS - 1) / BANDS;
int16_t bandX0[BANDS];
int16_t bandX1[BANDS];

void bandsBegin() {
  for (uint8_t band = 0; band < BANDS; band++) {
    bandX0[band] = BOX_X1;
    bandX1[band] = BOX_X0;
    for (int16_t y = BOX_Y0 + band * BAND_ROWS; y < BOX_Y0 + (band + 1) * BAND_ROWS; y++) {
      int16_t x0;
      int16_t x1;
      if (y > BOX_Y1 || !ringSpan((float)y + 0.5f - SCREEN_R, x0, x1)) {
        continue;
      }
      if (x0 < bandX0[band]) bandX0[band] = x0;
      if (x1 > bandX1[band]) bandX1[band] = x1;
    }
  }
}

// A box of one side sent, given the way the left bar has it and turned for the
// flush - and mirrored first for the right one.
void sendBox(uint8_t side, int16_t x0, int16_t x1, int16_t y0, int16_t y1) {
  int16_t from = (int16_t)(SCREEN_H - 1 - y1);
  int16_t to = (int16_t)(SCREEN_H - 1 - y0);
  if (side == 0) {
    boardFlushRect((int16_t)(SCREEN_W - 1 - x1), (int16_t)(SCREEN_W - 1 - x0), from, to);
  } else {
    boardFlushRect(x0, x1, from, to);
  }
}

// One side rebuilt, put back and sent: its ring a band at a time from `top`
// down, and the box its number stands in. Nothing else of a side is ever
// written, so nothing else is owed.
void send(uint16_t *fb, uint8_t side, int16_t top) {
  build(side, shown[side]);
  sent[side] = shown[side];
  gaugeDraw(fb, side == 0 ? (int16_t)(SCREEN_W - 1 - BOX_X1) : BOX_X0,
            side == 0 ? (int16_t)(SCREEN_W - 1 - BOX_X0) : BOX_X1,
            (int16_t)(SCREEN_H - 1 - BOX_Y1), (int16_t)(SCREEN_H - 1 - BOX_Y0));
  for (uint8_t band = 0; band < BANDS; band++) {
    int16_t y0 = BOX_Y0 + band * BAND_ROWS;
    int16_t y1 = (int16_t)(y0 + BAND_ROWS - 1);
    if (y0 < top) y0 = top;
    if (y1 > BOX_Y1) y1 = BOX_Y1;
    if (y0 > y1 || bandX0[band] > bandX1[band]) {
      continue;
    }
    sendBox(side, bandX0[band], bandX1[band], y0, y1);
  }
  sendBox(side, FIGURE_X - FIGURE_HW, FIGURE_X + FIGURE_HW, FIGURE_TOP - 2,
          FIGURE_TOP + FIGURE_HEIGHT + 1);
}

}  // namespace

void gaugeBegin() {
  store.begin("gauge", false);
  wanted = store.getUChar("figures", 0) != 0;
  Serial.printf("gauge: figures %s\n", wanted ? "on" : "off");

  pixels = (Pixel *)heap_caps_malloc(sizeof(Pixel) * SIDE_PIXELS * 2, MALLOC_CAP_SPIRAM);
  if (!pixels) {
    Serial.println("gauges: no room");
    return;
  }
  bandsBegin();
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
    for (uint8_t side = 0; side < 2; side++) {
      wipe(fb, side);
      send(fb, side, BOX_Y0);
    }
    return;
  }

  // Left is the session window, right is the week. Empty until a read comes
  // back, and then they walk to it. Emptied again between accounts, so a swipe
  // is answered at once rather than when the read lands - and is answered at
  // all when the two accounts have spent the same.
  if (usageReady()) {
    bool between = usageSwitching();
    target[0] = between ? 0.0f : (float)usageSession();
    target[1] = between ? 0.0f : (float)usageWeekly();
  }

  // Long only while nothing else wants the bottom of the glass: the address has
  // it until a token does, and the countdown has it whenever the figures are up.
  bool show = wanted && usageReady();
  if (show != figures) {
    figures = show;
    figuresMoved = true;
  }
  bool reshape = figuresMoved;
  figuresMoved = false;

  float onto = figures ? 1.0f : 0.0f;
  if (figuresT != onto) {
    float by = dt / REACH_S;
    figuresT = onto > figuresT ? (figuresT + by > onto ? onto : figuresT + by)
                               : (figuresT - by < onto ? onto : figuresT - by);
    figuresLevel = figuresT * figuresT * (3.0f - 2.0f * figuresT);
  }
  float wanted = usageReady() && !figures ? 1.0f : 0.0f;
  if (reachT != wanted) {
    float by = dt / REACH_S;
    reachT = wanted > reachT ? (reachT + by > wanted ? wanted : reachT + by)
                             : (reachT - by < wanted ? wanted : reachT - by);
    reach = reachT * reachT * (3.0f - 2.0f * reachT);
    reshape = true;
  }

  float k = 1.0f - expf(-dt * APPROACH);
  for (uint8_t side = 0; side < 2; side++) {
    owed[side] |= reshape;
    reshaped[side] |= reshape;
    float gap = target[side] - shown[side];
    if (fabsf(gap) < ARRIVED) {
      if (shown[side] != target[side]) {
        shown[side] = target[side];
        owed[side] = true;
      }
      continue;
    }
    shown[side] += gap * k;
    owed[side] = true;
  }

  if (!owed[0] && !owed[1]) {
    return;
  }
  uint8_t side = owed[0] && owed[1] ? turn : (owed[0] ? 0 : 1);
  turn = side ^ 1;
  int16_t top = fillTop(shown[side] > sent[side] ? shown[side] : sent[side]);
  // A bar that just got shorter, or a number that just went away, leaves itself
  // behind otherwise.
  if (reshaped[side]) {
    wipe(fb, side);
    top = BOX_Y0;
  }
  send(fb, side, top);
  owed[side] = false;
  reshaped[side] = false;
}

void gaugeReveal() {
  if (phase == Phase::Hidden) {
    phase = Phase::Fading;
    lastStep = millis();
  }
}

void gaugeFigures() {
  wanted = !wanted;
  // Straight to the store rather than on a timer. It is one byte per press of a
  // button somebody has to reach for, not one per position of a slider.
  store.putUChar("figures", wanted ? 1 : 0);
  Serial.printf("gauge: figures %s\n", wanted ? "on" : "off");
}

float gaugeFiguresLevel() { return figuresLevel; }

uint16_t gaugeColour(uint8_t percent) { return colourAt((float)percent); }

void gaugeDraw(uint16_t *fb, int16_t x0, int16_t x1, int16_t from, int16_t to) {
  // The box comes in screen coordinates and the pixels were worked out in the
  // scene's, so the box is turned once here rather than every pixel being turned
  // to meet it.
  const int16_t yLo = (int16_t)(SCREEN_H - 1 - to);
  const int16_t yHi = (int16_t)(SCREEN_H - 1 - from);
  const int16_t xLo = (int16_t)(SCREEN_W - 1 - x1);
  const int16_t xHi = (int16_t)(SCREEN_W - 1 - x0);
  for (uint8_t side = 0; side < 2; side++) {
    // Nothing of this bar is in the box, so none of its pixels are worth
    // walking - which is the usual answer, and the whole saving.
    if (edgeX1[side] < xLo || edgeX0[side] > xHi || edgeY1[side] < yLo ||
        edgeY0[side] > yHi) {
      continue;
    }
    const Pixel *p = pixels + side * SIDE_PIXELS;
    for (uint16_t i = 0; i < count[side]; i++) {
      if (p[i].y < yLo || p[i].y > yHi || p[i].x < xLo || p[i].x > xHi) {
        continue;
      }
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
    snprintf(said, sizeof(said), "%u%%", (unsigned)(sent[side] + 0.5f));
    int16_t at = side == 1 ? (int16_t)(SCREEN_W - 1 - FIGURE_X) : FIGURE_X;
    textDraw(fb, said, at, FIGURE_TOP, 2, boardColour(0xFFFF));
  }
}
