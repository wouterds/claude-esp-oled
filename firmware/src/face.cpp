#include "face.h"

#include <Arduino.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "audio.h"
#include "board.h"
#include "outage.h"
#include "usage.h"

// Every shape here is a signed distance rather than a span of pixels, so a
// pixel's coverage falls out of the distance and the curves come out smooth
// without a second pass over them. An eye and a squint then differ by a number
// rather than by a branch - though a heart and a rounded box do not, which is
// what the cross-fade is for.
namespace {

constexpr float PI_F = 3.14159265f;
constexpr float GLOW_RADIUS = 8.0f * SCENE;
constexpr float GLOW_INV = 1.0f / GLOW_RADIUS;
constexpr float GLOW_GAIN = 0.5f;

// How far from its home the face may drift.
constexpr float ROAM = 26.0f * SCENE;
constexpr float EYE_GAP = 46.0f * SCENE;
// The clear box the face paints inside runs from 80 above this to 84 below it,
// which is also what caps this at 197: the box's bottom is this plus ROAM plus
// 84, and the bottom line of text starts at 307. The drift is small because the
// panel is round: with the mouth below the eyes the face reaches 129 pixels from
// its own centre, and 180 is where the glass stops.
constexpr float HOME_Y = 196.0f * SCENE;
constexpr float EYE_RISE = 28.0f * SCENE;
constexpr float MOUTH_DROP = 52.0f * SCENE;

constexpr float BLEND_SECONDS = 0.35f;

enum class Mood : uint8_t { Neutral, Happy, Surprised, Angry, Dead, Tired };

// Constants, but sinf and cosf are not free and the shapes below are asked for
// them thirty thousand times a frame. Filled once, in faceBegin.
struct Trig {
  float archSin, archCos;
  float browSin, browCos;
};
Trig trig;

// Where the expressions come from. The face does not have moods of its own any
// more - it reads the two gauges, and these are the marks on them.
constexpr uint8_t DEAD_AT = 98;
constexpr uint8_t CROSS_AT = 80;
// Where the face starts reacting to the number at all. Between here and
// CROSS_AT it is startled by it; above CROSS_AT it is cross about it.
constexpr uint8_t WATCH_AT = 70;
constexpr uint8_t EASY_UNDER = 40;
// Where the gauge itself has gone full yellow. The face and the bar agree
// about what a number means or one of them is lying.
constexpr uint8_t WARM_AT = 60;
constexpr uint8_t STILL_POLLS = 5;
// Long enough to be seen and not so long it stops being an expression. It is a
// reaction to something happening, not a state it lives in.
constexpr float CHEER_S = 5.0f;
constexpr float STARTLE_S = 5.0f;
// A window rolling over does not creep back down, it falls.
constexpr uint8_t A_RESET = 10;

// The bands wear their face in flares over whatever is underneath rather than
// locking into it. A number that has been high for an hour is not news for an
// hour, and an expression that never changes stops being looked at - so the
// face says it, goes back to itself, and says it again a while later.
constexpr float FLARE_LO = 4.0f;
constexpr float FLARE_HI = 6.0f;
// How long between them, at the bottom of the bands and at the top. The gap
// closes as the number climbs, so the face gets visibly twitchier the worse it
// is without ever settling into one expression.
constexpr float GAP_FAR = 45.0f;
constexpr float GAP_NEAR = 20.0f;
// Either side of that gap, so the flares do not arrive on a metronome.
constexpr float GAP_JITTER = 0.25f;

// What a dead face has left. Nothing about it is in a hurry, and a quarter is
// slow enough to read as winding down without stopping dead - which would look
// like the panel had frozen rather than like the face had given up.
constexpr float DEAD_SLOW = 0.25f;

struct State {
  float x, y;          // where the eyes are
  float tx, ty;        // where they are drifting to
  float vx, vy;
  float clock;         // seconds since boot, for the float and the jitter
  float held;          // seconds the current mood has been worn
  uint8_t seen[2];     // what the gauges last said
  bool read;           // and whether they have ever said anything
  float cheer;         // seconds of good cheer still owed
  bool easy;           // whether both windows were last seen with room to spare
  float startle;       // seconds of surprise still owed
  float flare;         // seconds of the band's own face still owed
  float flareIn;       // and seconds until it wears it again
  Mood flareAs;        // which of the two this one is
  bool warm;           // whether either window was last seen in the yellow
  Outage alarm;        // what the status page last said, to catch it worsening
  float blend;         // 0 while settled, counts up through a change
  Mood mood;
  Mood was;
  // The box the last frame actually painted, so exactly that much of the panel
  // is put back to black - no more, and no less. A generous constant either
  // wipes rows nothing was ever drawn on, or comes up short when a look far to
  // one side carries an eye further out than the constant allowed for.
  float paintedL, paintedT, paintedR, paintedB;
  bool painted;
  float lookX, lookY;      // where it is looking, eased
  float lookTX, lookTY;    // and where it has decided to look next
  float lookIn;            // seconds until it looks somewhere else
  float blinkIn;       // seconds until the next blink
  float blinking;      // seconds left of this one
};

State me;

#define ALWAYS inline __attribute__((always_inline))

ALWAYS float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// The length of two legs that are both clamped at nought. Whenever a point is
// beside a box rather than off one of its corners, one leg is zero and the
// length is simply the other one, with no root at all - and better than a third
// of the pixels in an eye are like that.
//
// The corners do need one, and this chip's FPU has neither a root nor a divide:
// the toolchain emits a library call for both, and at one a pixel that call is
// most of what an eye costs. So they get a reciprocal root off the usual bit
// trick and two Newton steps - a dozen FPU instructions instead of the call.
// Measured against sqrtf across the whole range an eye can produce, the worst it
// is out by is 0.0003 of a pixel, and the coverage below quantises to a 255th.
ALWAYS float legs(float ax, float ay) {
  if (ax <= 0.0f) {
    return ay;
  }
  if (ay <= 0.0f) {
    return ax;
  }
  float sum = ax * ax + ay * ay;
  uint32_t bits;
  __builtin_memcpy(&bits, &sum, sizeof(bits));
  bits = 0x5f3759dfu - (bits >> 1);
  float root;
  __builtin_memcpy(&root, &bits, sizeof(root));
  root = root * (1.5f - 0.5f * sum * root * root);
  root = root * (1.5f - 0.5f * sum * root * root);
  return sum * root;
}
ALWAYS float dot2(float x, float y) { return x * x + y * y; }
inline float frand(float lo, float hi) {
  return lo + (hi - lo) * (float)random(0, 10001) * 0.0001f;
}

ALWAYS float sdRoundBox(float px, float py, float hx, float hy, float r) {
  float qx = fabsf(px) - hx + r;
  float qy = fabsf(py) - hy + r;
  float ax = qx > 0.0f ? qx : 0.0f;
  float ay = qy > 0.0f ? qy : 0.0f;
  float inner = qx > qy ? qx : qy;
  return legs(ax, ay) + (inner < 0.0f ? inner : 0.0f) - r;
}

// A rounded box whose corner radius can never outrun the half-extent it is
// rounding. Squashing an eye - a blink, a wake - drives the height below the
// radius, and sdRoundBox is only a distance while the radius fits inside it;
// past that the ends come out in points.
ALWAYS float sdEye(float px, float py, float hx, float hy, float r) {
  float limit = hx < hy ? hx : hy;
  return sdRoundBox(px, py, hx, hy, r < limit ? r : limit);
}

// A rounded box with one radius at the top and another at the bottom, so an eye
// can be domed above and squarer below. Screen y runs down, so the top corners
// are the negative ones.
ALWAYS float sdRoundBoxTB(float px, float py, float hx, float hy, float rTop, float rBottom) {
  float r = py < 0.0f ? rTop : rBottom;
  float limit = hx < hy ? hx : hy;
  if (r > limit) {
    r = limit;
  }
  float qx = fabsf(px) - hx + r;
  float qy = fabsf(py) - hy + r;
  float ax = qx > 0.0f ? qx : 0.0f;
  float ay = qy > 0.0f ? qy : 0.0f;
  float inner = qx > qy ? qx : qy;
  return legs(ax, ay) + (inner < 0.0f ? inner : 0.0f) - r;
}

inline float sdSegment(float px, float py, float ax, float ay, float bx, float by) {
  float pax = px - ax;
  float pay = py - ay;
  float bax = bx - ax;
  float bay = by - ay;
  float h = clamp01((pax * bax + pay * bay) / (bax * bax + bay * bay));
  return sqrtf(dot2(pax - bax * h, pay - bay * h));
}

// A blob with its own radius top and bottom. A smile is the shape being rounder
// underneath than above rather than a stripe bent into a curve - bending thins
// the ends, which is what made every earlier attempt read as a banana.
struct Mouth {
  float hx, hy;
  float rTop, rBottom;
};

// Thin bars, because a bent bar thick enough to look like a blob at rest looks
// like a wedge once it is bent. The roundness comes from the corner radius
// being the whole of the half-height: every one of these is a capsule.
// In the order the enum declares them, because that is what indexes this.
constexpr Mouth MOUTHS[] = {
    {15.0f, 7.0f, 7.0f, 7.0f},     // neutral, a flat stripe
    {16.0f, 11.0f, 4.0f, 11.0f},   // happy, squared on top and domed underneath
    {16.0f, 18.0f, 16.0f, 16.0f},  // surprised, an o
    {18.0f, 7.0f, 7.0f, 7.0f},     // angry, flat and wider than neutral
    {10.0f, 7.5f, 7.5f, 7.5f},     // dead, short and thick
    {12.0f, 6.5f, 6.5f, 6.5f},     // tired, a small even blob - a sag here
                                   // reads as a frown, and a frown is cross
};

// The blob is bent rather than cut. Taking a circle out of it leaves the ends
// of the crescent in points, which is the one thing a mouth made of a rounded
// blob should not have - bending keeps every side as round as it started.
ALWAYS float mouthShape(Mood mood, float x, float y) {
  const Mouth &m = MOUTHS[(uint8_t)mood];
  return sdRoundBoxTB(x, y, m.hx, m.hy, m.rTop, m.rBottom);
}

// How far one eye reaches from its own centre, per expression, before the glow.
// The box walked below is this rather than the widest of all of them, which on a
// face that is usually neutral is a good fifth of the pixels never looked at.
//
// `lid` says whether the lid above actually scales the shape. Neutral, happy and
// surprised are a height times the squeeze and nothing else. Angry is that too,
// but tilted, so squeezing it shrinks the rotated height and the reach across
// and down both come off it unevenly - and dead ignores the squeeze entirely.
// Neither of those two ever blinks, so both are simply left at full height.
//
// `slides` is tired, which is the one that does not close about its middle: the
// lid comes down onto a bottom edge that stays put, so the shape moves down the
// glass as it shrinks and a span measured about the centre misses the bottom of
// it. That was clipping the sleepy eye by eight pixels at the shut end of every
// cycle before this was written down.
struct Reach {
  float x;
  float y;
  bool lid;
  bool slides;
};

ALWAYS Reach eyeReach(Mood mood) {
  switch (mood) {
    case Mood::Happy:
      return {28.0f, 29.0f, true, false};
    case Mood::Surprised:
      return {34.0f, 40.0f, true, false};
    case Mood::Angry:
      return {35.0f, 30.0f, false, false};
    case Mood::Dead:
      return {29.0f, 29.0f, false, false};
    case Mood::Tired:
      return {28.0f, 29.0f, true, true};
    case Mood::Neutral:
    default:
      return {29.0f, 38.0f, true, false};
  }
}

// Four of the six expressions draw the eye as one blob, and these are the
// numbers that make it. They live here rather than inside eyeShape below so that
// the shape and the fast path in faceDraw read the same figures - two copies of
// them is two copies that drift.
//
// `slide` is how far down its own centre the shape sits, which is only ever
// tired: a lid comes down onto a bottom edge that stays where it is. Scaling the
// height alone would take the eye in from the top and the bottom at once, which
// reads as an eye getting smaller rather than as one closing, and that is the
// whole difference between sleepy and shrinking.
struct Blob {
  bool plain;
  float hx, hy;
  float rTop, rBottom;
  float slide;
};

ALWAYS Blob eyeBlob(Mood mood, float squeeze) {
  switch (mood) {
    case Mood::Happy:
      // Neutral's blob, domed over the top and squared off underneath.
      return {true, 28.0f, 29.0f * squeeze, 28.0f, 9.0f, 0.0f};
    case Mood::Surprised:
      return {true, 34.0f, 40.0f * squeeze, 34.0f, 34.0f, 0.0f};
    case Mood::Tired: {
      // Rounded over the top, not squared, and not tilted at all. A flat edge
      // above a squat eye is a brow and reads as cross; a slant either way is an
      // emotion of its own, outward into sadness and inward into anger. Tired is
      // none of those - it is just slack.
      constexpr float FULL = 29.0f * SCENE;
      float half = FULL * squeeze;
      return {true, 28.0f, half, 15.0f, 24.0f, FULL - half};
    }
    case Mood::Angry:
    case Mood::Dead:
      // Angry is tilted and dead is not a blob at all; both draw themselves.
      return {false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    case Mood::Neutral:
    default:
      // Anything new lands here and looks neutral, which is where it landed
      // before this was a table.
      return {true, 29.0f, 38.0f * squeeze, 23.0f, 23.0f, 0.0f};
  }
}

// What a row of one of those blobs already knows. Every term here comes off the
// row's own y and the frame's squeeze, so a row works it out once rather than
// once for each of the seventy-odd pixels along it.
struct BlobRow {
  float base;  // r - hx, so a pixel's own leg is |lx| + this
  float qy;
  float ay;
  float r;
};

ALWAYS BlobRow blobRow(const Blob &b, float ly) {
  float py = ly - b.slide;
  float r = py < 0.0f ? b.rTop : b.rBottom;
  float limit = b.hx < b.hy ? b.hx : b.hy;
  if (r > limit) {
    r = limit;
  }
  float qy = (py < 0.0f ? -py : py) - b.hy + r;
  BlobRow row;
  row.base = r - b.hx;
  row.qy = qy;
  row.ay = qy > 0.0f ? qy : 0.0f;
  row.r = r;
  return row;
}

// The same distance sdRoundBoxTB gives, with everything the row settled already
// settled.
ALWAYS float blobAt(const BlobRow &row, float lx) {
  float qx = (lx < 0.0f ? -lx : lx) + row.base;
  float ax = qx > 0.0f ? qx : 0.0f;
  float inner = qx > row.qy ? qx : row.qy;
  return legs(ax, row.ay) + (inner < 0.0f ? inner : 0.0f) - row.r;
}

// One eye of one expression, in its own space, with the sign of `side` telling
// it which of the pair it is so anything slanted mirrors instead of repeating.
ALWAYS float eyeShape(Mood mood, float x, float y, float side, float squeeze) {
  switch (mood) {
    case Mood::Angry: {
      // Just tilted, and shorter than it is wide. Cutting the top flat is the
      // obvious way to draw a brow and it leaves a hard edge across the one
      // shape that should be round the whole way round; the slant carries it
      // on its own.
      float c = trig.browCos;
      float s = trig.browSin * side;
      float rx = x * c - y * s;
      float ry = x * s + y * c;
      return sdEye(rx, ry, 29.0f, 19.0f * squeeze, 18.0f);
    }
    case Mood::Dead: {
      constexpr float ARM = 20.0f * SCENE;
      constexpr float THICK = 8.5f * SCENE;
      float a = sdSegment(x, y, -ARM, -ARM, ARM, ARM) - THICK;
      float b = sdSegment(x, y, -ARM, ARM, ARM, -ARM) - THICK;
      return fminf(a, b);
    }
    default: {
      Blob b = eyeBlob(mood, squeeze);
      return sdRoundBoxTB(x, y - b.slide, b.hx, b.hy, b.rTop, b.rBottom);
    }
  }
}

// Coverage of one eye, cross-faded while an expression is changing. Two shapes
// are only ever evaluated during the third of a second that takes.
// Interpolating the distances rather than the coverage is what makes this a
// morph: the boundary walks from one shape to the other and the eye deforms
// through the in-between. Fading the coverage instead would ghost one shape out
// while the other came up underneath it.
ALWAYS float coverFrom(float d) {
  float cover = clamp01(0.5f - d);
  float glow = d < GLOW_RADIUS ? clamp01(1.0f - d * GLOW_INV) : 0.0f;
  glow = glow * glow * GLOW_GAIN * (1.0f - cover);
  return cover + glow * 0.55f;
}

// A distance, turned into ink on one pixel. Every loop below ends this way.
ALWAYS void lay(uint16_t *row, int16_t x, float d) {
  float v = clamp01(coverFrom(d));
  if (v <= 0.004f) {
    return;
  }
  uint8_t level = (uint8_t)(v * 255.0f);
  row[boardX(x)] =
      boardColour((uint16_t)(((level & 0xF8) << 8) | ((level & 0xFC) << 3) | (level >> 3)));
}

ALWAYS float mouthShapeMixed(Mood mood, Mood before, float x, float y, float mix) {
  float d = mouthShape(mood, x, y);
  if (mix < 1.0f) {
    float was = mouthShape(before, x, y);
    d = was + (d - was) * mix;
  }
  return d;
}

// Interpolating the distances rather than the coverage is what makes this a
// morph: the boundary walks from one shape to the other and the eye deforms
// through the in-between. Fading the coverage instead would ghost one shape out
// while the other came up underneath it.
ALWAYS float eyeShapeMixed(Mood mood, Mood before, float x, float y, float side, float squeeze,
                           float mix) {
  float d = eyeShape(mood, x, y, side, squeeze);
  if (mix < 1.0f) {
    float was = eyeShape(before, x, y, side, squeeze);
    d = was + (d - was) * mix;
  }
  return d;
}

void clearBox(uint16_t *fb, float fx0, float fy0, float fx1, float fy1) {
  int16_t x0 = fx0 < 0.0f ? 0 : (int16_t)fx0;
  int16_t y0 = fy0 < 0.0f ? 0 : (int16_t)fy0;
  int16_t x1 = fx1 > SCREEN_W - 1 ? SCREEN_W - 1 : (int16_t)fx1;
  int16_t y1 = fy1 > SCREEN_H - 1 ? SCREEN_H - 1 : (int16_t)fy1;
  // Turned, so the span is still contiguous and still one memset a row.
  for (int16_t y = y0; y <= y1; y++) {
    memset(boardRow(fb, y) + boardX(x1), 0, (size_t)(x1 - x0 + 1) * 2);
  }
}

void pickTarget() {
  float angle = frand(0.0f, 2.0f * PI_F);
  // sqrt spreads the targets evenly over the disc instead of crowding them into
  // the middle, so it uses the whole panel rather than hovering.
  float radius = ROAM * sqrtf(frand(0.0f, 1.0f));
  me.tx = SCREEN_R + cosf(angle) * radius;
  me.ty = HOME_Y + sinf(angle) * radius;

  // The eyes go first. Deciding to be somewhere else and then looking at it on
  // the way is what separates drifting from being blown about.
  float dx = me.tx - me.x;
  float dy = me.ty - me.y;
  float len = sqrtf(dx * dx + dy * dy);
  if (len > 1.0f) {
    me.lookTX = dx / len * frand(11.0f, 20.0f);
    me.lookTY = dy / len * frand(5.0f, 9.0f);
    me.lookIn = frand(0.5f, 1.2f);
  }
}

}  // namespace

void faceBegin() {
  randomSeed(esp_random());
  trig.archSin = sinf(1.15f);
  trig.archCos = cosf(1.15f);
  trig.browSin = sinf(0.42f);
  trig.browCos = cosf(0.42f);
  me.x = me.tx = SCREEN_R;
  me.y = me.ty = HOME_Y;
  me.vx = me.vy = 0.0f;
  me.clock = 0.0f;
  me.held = 0.0f;
  me.seen[0] = me.seen[1] = 0;
  me.read = false;
  me.cheer = 0.0f;
  me.easy = false;
  me.startle = 0.0f;
  me.flare = 0.0f;
  me.flareIn = 0.0f;
  me.flareAs = Mood::Neutral;
  me.warm = false;
  me.alarm = Outage::Unknown;
  me.blend = 1.0f;
  me.painted = false;
  me.mood = Mood::Neutral;
  me.was = Mood::Neutral;
  me.lookX = me.lookY = me.lookTX = me.lookTY = 0.0f;
  me.lookIn = 1.0f;
  me.blinkIn = 2.0f;
  me.blinking = 0.0f;
  pickTarget();
}

// What the numbers add up to. Worst of the two decides it, because a face that
// reports the better half of bad news is not worth reading.
void settle(float dt) {
  // The status page answers on its own schedule and can land before the first
  // usage read does, so it is watched whether there are figures yet or not.
  // Worsening only, and Unknown settling into None at boot is not news.
  Outage alarm = outageLevel();
  if (alarm > me.alarm && alarm >= Outage::Partial) {
    me.startle = STARTLE_S;
  }
  me.alarm = alarm;

  // Whether the one thing that undoes a death has just happened. Set inside the
  // read below, where the drop is spotted, and read after it.
  bool revived = false;
  if (usageReady()) {
    uint8_t a = usageSession();
    uint8_t b = usageWeekly();
    bool dropped = me.read && (a + A_RESET <= me.seen[0] || b + A_RESET <= me.seen[1]);
    bool easy = a < EASY_UNDER && b < EASY_UNDER;
    bool warm = a >= WARM_AT || b >= WARM_AT;
    // Both of the good things are events rather than states: a window rolling
    // over, or opening its eyes to find there is room. Either is worth five
    // seconds and then it has been said.
    if (dropped || (easy && (!me.read || !me.easy))) {
      me.cheer = CHEER_S;
    }
    revived = dropped;
    // Climbing into the yellow is worth the same. Dropping back out of it says
    // nothing - it only re-arms this, so the next climb can say it again.
    if (warm && !me.warm) {
      me.startle = STARTLE_S;
    }
    me.seen[0] = a;
    me.seen[1] = b;
    me.easy = easy;
    me.warm = warm;
    me.read = true;
  }

  if (me.cheer > 0.0f) {
    me.cheer -= dt;
  }
  if (me.startle > 0.0f) {
    me.startle -= dt;
  }

  uint8_t worst = me.seen[0] > me.seen[1] ? me.seen[0] : me.seen[1];
  if (me.flare > 0.0f) {
    me.flare -= dt;
  }

  // Nothing to flare about under WATCH_AT, and nothing left to say at DEAD_AT -
  // the face has stopped pretending by then and wears the one expression.
  if (!me.read || worst < WATCH_AT || worst >= DEAD_AT) {
    me.flare = 0.0f;
    // Left due, so crossing into a band is itself worth a face rather than
    // something you wait most of a minute to hear about.
    me.flareIn = 0.0f;
  } else {
    me.flareIn -= dt;
    if (me.flareIn <= 0.0f) {
      me.flare = frand(FLARE_LO, FLARE_HI);
      me.flareAs = worst >= CROSS_AT ? Mood::Angry : Mood::Surprised;
      // Nought at the bottom of the bands and one at the top.
      float deep = (float)(worst - WATCH_AT) / (float)(DEAD_AT - WATCH_AT);
      float gap = GAP_FAR + (GAP_NEAR - GAP_FAR) * deep;
      me.flareIn = gap * frand(1.0f - GAP_JITTER, 1.0f + GAP_JITTER);
    }
  }

  // Dead is dead. Everything the ladder below weighs is a level, and no level
  // means anything to a face that has already spent the lot - the number
  // drifting back under the mark is not a recovery, it is the same window still
  // being spent. Only a window actually rolling over is news enough to undo it,
  // and that arrives as a fall rather than as a reading.
  if (me.mood == Mood::Dead && !revived) {
    return;
  }

  Mood want;
  if (worst >= DEAD_AT) {
    want = Mood::Dead;
  } else if (me.flare > 0.0f) {
    want = me.flareAs;
  } else if (me.startle > 0.0f) {
    // Over the states below it: a reaction to something that has just happened
    // outranks a description of how things have been.
    want = Mood::Surprised;
  } else if (usageStill() >= STILL_POLLS) {
    want = Mood::Tired;
  } else if (me.cheer > 0.0f) {
    want = Mood::Happy;
  } else {
    want = Mood::Neutral;
  }

  if (want != me.mood) {
    me.was = me.mood;
    me.blend = 0.0f;
    me.held = 0.0f;
    // On the way in only. It is the one change on this face worth hearing from
    // the other side of a room, and the only one you cannot undo by waiting.
    if (want == Mood::Dead) {
      audioDied();
    }
    me.mood = want;
  }
}

void faceStep(float dt) {
  settle(dt);
  me.blend = clamp01(me.blend + dt / BLEND_SECONDS);

  me.held += dt;

  // Everything below here moves; everything above it counts. Slowing the clock
  // rather than each of the drift, the float, the look and the gaps between
  // looks is what keeps them slowing together - one thing winding down instead
  // of four things that happen to be sluggish. The countdowns are left on real
  // seconds, or five seconds of surprise would quietly become twenty.
  if (me.mood == Mood::Dead) {
    dt *= DEAD_SLOW;
  }
  me.clock += dt;

  // A critically damped spring: it arrives without ringing, and the stiffness
  // alone is the difference between drifting over and darting.
  const float k = 9.0f;
  const float damping = 2.0f * sqrtf(k);
  me.vx += ((me.tx - me.x) * k - me.vx * damping) * dt;
  me.vy += ((me.ty - me.y) * k - me.vy * damping) * dt;
  me.x += me.vx * dt;
  me.y += me.vy * dt;

  float dx = me.tx - me.x;
  float dy = me.ty - me.y;
  if (dx * dx + dy * dy < 16.0f) {
    pickTarget();
  }

  // Drifting on its own reads as floating. What reads as looking at things is
  // the shape of the movement: eyes snap to somewhere and hold there, mostly
  // nearby with the occasional long look off to one side. Easing them evenly
  // between points is a camera pan, not a creature.
  me.lookIn -= dt;
  if (me.lookIn <= 0.0f) {
    long roll = random(0, 100);
    if (me.mood == Mood::Tired) {
      // Down and not far. Eyes that keep casting about are not sleepy ones.
      me.lookTX = frand(-6.0f, 6.0f);
      me.lookTY = frand(1.0f, 5.5f);
      me.lookIn = frand(1.6f, 3.4f);
    } else if (roll < 42) {
      // Straight at you, and held. Something that only ever looks past you is
      // not looking at anything - most of the time it should just be there.
      me.lookTX = frand(-2.5f, 2.5f);
      me.lookTY = frand(-1.5f, 1.5f);
      me.lookIn = frand(1.5f, 3.6f);
    } else if (roll < 76) {
      // Wherever it is heading.
      float dx = me.tx - me.x;
      float dy = me.ty - me.y;
      float len = sqrtf(dx * dx + dy * dy);
      if (len < 1.0f) {
        len = 1.0f;
      }
      me.lookTX = dx / len * frand(10.0f, 20.0f);
      me.lookTY = dy / len * frand(4.0f, 9.0f);
      me.lookIn = frand(0.6f, 1.7f);
    } else {
      // And now and then, something off past the edge of the glass.
      me.lookTX = frand(-21.0f, 21.0f);
      me.lookTY = frand(-9.0f, 9.0f);
      me.lookIn = frand(0.8f, 2.2f);
    }
  }
  float settle = 1.0f - expf(-dt * 26.0f);
  me.lookX += (me.lookTX - me.lookX) * settle;
  me.lookY += (me.lookTY - me.lookY) * settle;

  // Blinking is the cheapest thing on the panel and does the most work, but a
  // blink through an X or a heart would read as a glitch rather than as life.
  if (me.mood == Mood::Neutral || me.mood == Mood::Surprised) {
    me.blinking -= dt;
    if (me.blinking <= 0.0f) {
      me.blinkIn -= dt;
      if (me.blinkIn <= 0.0f) {
        me.blinking = 0.16f;
        me.blinkIn = frand(2.4f, 6.0f);
      }
    }
  } else {
    me.blinking = 0.0f;
  }
}



void faceDraw(uint16_t *fb, int16_t *rowFrom, int16_t *rowTo) {
  // The float: a slow rise and fall that never quite repeats, plus a shiver
  // while it is excited.
  float floatY = sinf(me.clock * 1.5f) * 4.0f + sinf(me.clock * 0.61f) * 2.5f;
  float floatX = sinf(me.clock * 0.83f) * 3.0f;
  float centreX = me.x + floatX;
  float centreY = me.y + floatY;

  float squeeze = 1.0f;
  if (me.mood == Mood::Tired) {
    // Losing the argument with sleep: the lids sink, catch themselves twice on
    // the way down and lose a little more each time, and then it jolts awake in
    // a quarter of a second and starts again. An even slide down and back reads
    // as a machine breathing; the failed recoveries are what make it a thing
    // trying to stay awake.
    float t = fmodf(me.clock, 9.0f);
    float open;
    if (t < 8.0f) {
      // Down, and only ever down. Anything that lifts on the way makes the
      // whole thing a rhythm, and a rhythm reads as breathing rather than as
      // losing a fight. Squared so it barely moves at first and goes quickest
      // at the end, which is how sleep actually arrives.
      float e = t * (1.0f / 8.0f);
      open = 1.0f - e * e;
    } else {
      // Then all at once. This is the one place the movement should be abrupt:
      // it is the catching-itself, and everything before it was the slide.
      open = clamp01((t - 8.0f) * (1.0f / 0.16f));
    }
    // It never gets all the way there. The floor leaves the eye a clear slit
    // rather than a line, so what you see is something catching itself just
    // before it goes under rather than something that has already gone.
    squeeze = 0.30f + 0.70f * open;
  }
  if (me.blinking > 0.0f) {
    squeeze = 0.06f + 0.94f * fabsf(cosf((me.blinking / 0.16f) * PI_F));
  }
  float mix = me.blend;
  // Read once. Off the global they are a load the compiler has to repeat for
  // every pixel; in a local it can lift the whole choice of shape out of the
  // loop instead of deciding it eighteen thousand times.
  const Mood mood = me.mood;
  const Mood before = me.was;

  // Whichever of the two shapes on the glass reaches furthest, since during a
  // change both of them are being drawn.
  Reach reach = eyeReach(mood);
  if (mix < 1.0f) {
    Reach other = eyeReach(before);
    if (other.x > reach.x) reach.x = other.x;
    if (other.y > reach.y) reach.y = other.y;
    reach.lid = reach.lid && other.lid;
    reach.slides = reach.slides || other.slides;
  }
  // Where the ink can be on a row, and how far either side of that it goes.
  float lidHalf = reach.lid ? reach.y * squeeze : reach.y;
  float lidMid = reach.slides ? reach.y - lidHalf : 0.0f;
  float spanX = reach.x + GLOW_RADIUS;

  // The mouth's own size, off the table it is drawn from rather than off the
  // biggest entry in it. A neutral mouth is seven pixels tall where the widest
  // is twenty-two, and the fifteen rows between them were being walked, cleared
  // and sent every frame for nothing.
  float mouthHX = MOUTHS[(uint8_t)mood].hx;
  float mouthHY = MOUTHS[(uint8_t)mood].hy;
  if (mix < 1.0f) {
    if (MOUTHS[(uint8_t)before].hx > mouthHX) mouthHX = MOUTHS[(uint8_t)before].hx;
    if (MOUTHS[(uint8_t)before].hy > mouthHY) mouthHY = MOUTHS[(uint8_t)before].hy;
  }

  float eyeY = centreY - EYE_RISE + me.lookY * 0.5f;
  float mouthY = centreY + MOUTH_DROP;
  // The two eyes do not travel together: the one on the side being looked toward
  // carries a little more of the movement, and each carries a slow wobble of its
  // own. Worked out here rather than in the loop, because where they land is
  // what says how much of the glass this frame is about to touch.
  float eyeX[2];
  for (uint8_t i = 0; i < 2; i++) {
    float side = i == 0 ? -1.0f : 1.0f;
    eyeX[i] = centreX + side * EYE_GAP + me.lookX * (0.5f + side * 0.12f) +
              sinf(me.clock * 2.3f + (side > 0.0f ? 1.7f : 0.0f)) * 1.1f;
  }

  // Exactly what this frame will paint, eyes and mouth together.
  float left = (eyeX[0] < eyeX[1] ? eyeX[0] : eyeX[1]) - spanX - 1.0f;
  float right = (eyeX[0] > eyeX[1] ? eyeX[0] : eyeX[1]) + spanX + 1.0f;
  float top = eyeY + lidMid - lidHalf - GLOW_RADIUS - 1.0f;
  float bottom = eyeY + lidMid + lidHalf + GLOW_RADIUS + 1.0f;
  float mouthL = centreX - mouthHX - GLOW_RADIUS - 1.0f;
  float mouthR = centreX + mouthHX + GLOW_RADIUS + 1.0f;
  float mouthT = mouthY - mouthHY - GLOW_RADIUS - 1.0f;
  float mouthB = mouthY + mouthHY + GLOW_RADIUS + 1.0f;
  if (mouthL < left) left = mouthL;
  if (mouthR > right) right = mouthR;
  if (mouthT < top) top = mouthT;
  if (mouthB > bottom) bottom = mouthB;

  // Clear the union of that and whatever the last frame left behind. Nothing
  // else owns these rows, so what is not cleared here stays on the glass.
  float dirtyTop = top;
  float dirtyBottom = bottom;
  float dirtyLeft = left;
  float dirtyRight = right;
  if (me.painted) {
    if (me.paintedT < dirtyTop) dirtyTop = me.paintedT;
    if (me.paintedB > dirtyBottom) dirtyBottom = me.paintedB;
    if (me.paintedL < dirtyLeft) dirtyLeft = me.paintedL;
    if (me.paintedR > dirtyRight) dirtyRight = me.paintedR;
  }
  clearBox(fb, dirtyLeft, dirtyTop, dirtyRight, dirtyBottom);
  me.paintedL = left;
  me.paintedT = top;
  me.paintedR = right;
  me.paintedB = bottom;
  me.painted = true;

  // Both shapes on the glass are plain blobs, so a row can settle its own half
  // of the distance once and the pixels along it only do their own half. Angry
  // is tilted and dead is a pair of strokes: in both, x and y are mixed before
  // the distance is taken, so there is no half a row can settle in advance and
  // they go the long way round.
  Blob blob = eyeBlob(mood, squeeze);
  Blob blobWas = eyeBlob(before, squeeze);
  bool byRow = blob.plain && (mix >= 1.0f || blobWas.plain);

  for (uint8_t i = 0; i < 2; i++) {
    float side = i == 0 ? -1.0f : 1.0f;
    int16_t x0 = (int16_t)(eyeX[i] - spanX) - 1;
    int16_t x1 = (int16_t)(eyeX[i] + spanX) + 1;
    int16_t y0 = (int16_t)(eyeY + lidMid - lidHalf - GLOW_RADIUS) - 1;
    int16_t y1 = (int16_t)(eyeY + lidMid + lidHalf + GLOW_RADIUS) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
    if (y1 > SCREEN_H - 1) y1 = SCREEN_H - 1;

    for (int16_t y = y0; y <= y1; y++) {
      float ly = (float)y + 0.5f - eyeY;
      uint16_t *row = boardRow(fb, y);
      // The box is the shape's own now, so there is nothing left on the row or
      // in the span that a test here would throw away.
      if (byRow) {
        BlobRow now = blobRow(blob, ly);
        if (mix >= 1.0f) {
          for (int16_t x = x1; x >= x0; x--) {
            lay(row, x, blobAt(now, (float)x + 0.5f - eyeX[i]));
          }
        } else {
          // Mid-change, so both are wanted and the distances are mixed - which
          // is what makes it a morph rather than one shape fading into another.
          BlobRow gone = blobRow(blobWas, ly);
          for (int16_t x = x1; x >= x0; x--) {
            float lx = (float)x + 0.5f - eyeX[i];
            float was = blobAt(gone, lx);
            lay(row, x, was + (blobAt(now, lx) - was) * mix);
          }
        }
        continue;
      }
      for (int16_t x = x1; x >= x0; x--) {
        float lx = (float)x + 0.5f - eyeX[i];
        lay(row, x, eyeShapeMixed(mood, before, lx, ly, side, squeeze, mix));
      }
    }
  }

  {
    int16_t x0 = (int16_t)mouthL;
    int16_t x1 = (int16_t)mouthR;
    int16_t y0 = (int16_t)mouthT;
    int16_t y1 = (int16_t)mouthB;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
    if (y1 > SCREEN_H - 1) y1 = SCREEN_H - 1;

    for (int16_t y = y0; y <= y1; y++) {
      float ly = (float)y + 0.5f - mouthY;
      uint16_t *row = boardRow(fb, y);
      // The box is the mouth's own now, so nothing here would be thrown away.
      for (int16_t x = x1; x >= x0; x--) {
        float lx = (float)x + 0.5f - centreX;
        lay(row, x, mouthShapeMixed(mood, before, lx, ly, mix));
      }
    }
  }

  // Screen rows run the other way to framebuffer rows, so the band is flipped
  // as well as clamped.
  int16_t sTop = dirtyTop < 0.0f ? 0 : (int16_t)dirtyTop;
  int16_t sBottom = dirtyBottom > SCREEN_H - 1 ? SCREEN_H - 1 : (int16_t)dirtyBottom;
  *rowFrom = (int16_t)(SCREEN_H - 1 - sBottom);
  *rowTo = (int16_t)(SCREEN_H - 1 - sTop);
}
