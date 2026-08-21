#include "face.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "board.h"
#include "text.h"

// Every shape here is a signed distance rather than a span of pixels, so a
// pixel's coverage falls out of the distance and the curves come out smooth
// without a second pass over them. An eye and a squint then differ by a number
// rather than by a branch - though a heart and a rounded box do not, which is
// what the cross-fade is for.
namespace {

constexpr float PI_F = 3.14159265f;
constexpr float GLOW_RADIUS = 8.0f;
constexpr float GLOW_INV = 1.0f / GLOW_RADIUS;
constexpr float GLOW_GAIN = 0.5f;

// How far from its home the face may drift.
constexpr float ROAM = 26.0f;
constexpr float EYE_GAP = 46.0f;
// A little above the middle, which is where a face looks like it is looking at
// you rather than sitting in a box. The drift is small because the panel is
// round: with the mouth below the eyes the face reaches 129 pixels from its own
// centre, and 180 is where the glass stops.
constexpr float HOME_Y = 156.0f;
constexpr float EYE_RISE = 28.0f;
constexpr float MOUTH_DROP = 52.0f;

constexpr float BLEND_SECONDS = 0.35f;

enum class Mood : uint8_t { Neutral, Happy, Surprised, Angry, Dead, Tired };

// Constants, but sinf and cosf are not free and the shapes below are asked for
// them thirty thousand times a frame. Filled once, in faceBegin.
struct Trig {
  float archSin, archCos;
  float browSin, browCos;
};
Trig trig;

// What a tap steps through, neutral included so there is a way back to it.
constexpr Mood CYCLE[] = {Mood::Neutral, Mood::Happy,  Mood::Surprised,
                          Mood::Angry,   Mood::Tired,  Mood::Dead};
constexpr uint8_t CYCLE_COUNT = sizeof(CYCLE) / sizeof(CYCLE[0]);

struct State {
  float x, y;          // where the eyes are
  float tx, ty;        // where they are drifting to
  float vx, vy;
  float clock;         // seconds since boot, for the float and the jitter
  float held;          // seconds the current mood has been worn
  float blend;         // 0 while settled, counts up through a change
  Mood mood;
  Mood was;
  uint8_t next;        // which expression comes round next
  float paintedX, paintedY;  // where the eyes were last drawn, so only that
                             // much of the panel has to be put back to black
  char label[12];      // what the typewriter has put on the panel so far
  float lookX, lookY;      // where it is looking, eased
  float lookTX, lookTY;    // and where it has decided to look next
  float lookIn;            // seconds until it looks somewhere else
  uint8_t taps;        // taps inside the current window
  float tapWindow;     // when that window opened
  float crazy;         // seconds left of losing it, zero the rest of the time
  float flip;          // seconds until the next expression while it is loose
  float blinkIn;       // seconds until the next blink
  float blinking;      // seconds left of this one
};

const char *moodName(Mood mood) {
  switch (mood) {
    case Mood::Happy:
      return "HAPPY";
    case Mood::Surprised:
      return "SURPRISED";
    case Mood::Angry:
      return "ANGRY";
    case Mood::Dead:
      return "DEAD";
    case Mood::Tired:
      return "TIRED";
    case Mood::Neutral:
    default:
      return "NEUTRAL";
  }
}

State me;

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float dot2(float x, float y) { return x * x + y * y; }
inline float frand(float lo, float hi) {
  return lo + (hi - lo) * (float)random(0, 10001) * 0.0001f;
}

inline float sdRoundBox(float px, float py, float hx, float hy, float r) {
  float qx = fabsf(px) - hx + r;
  float qy = fabsf(py) - hy + r;
  float ax = qx > 0.0f ? qx : 0.0f;
  float ay = qy > 0.0f ? qy : 0.0f;
  float inner = qx > qy ? qx : qy;
  return sqrtf(ax * ax + ay * ay) + (inner < 0.0f ? inner : 0.0f) - r;
}

// A rounded box whose corner radius can never outrun the half-extent it is
// rounding. Squashing an eye - a blink, a wake - drives the height below the
// radius, and sdRoundBox is only a distance while the radius fits inside it;
// past that the ends come out in points.
inline float sdEye(float px, float py, float hx, float hy, float r) {
  float limit = hx < hy ? hx : hy;
  return sdRoundBox(px, py, hx, hy, r < limit ? r : limit);
}

// A rounded box with one radius at the top and another at the bottom, so an eye
// can be domed above and squarer below. Screen y runs down, so the top corners
// are the negative ones.
inline float sdRoundBoxTB(float px, float py, float hx, float hy, float rTop, float rBottom) {
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
  return sqrtf(ax * ax + ay * ay) + (inner < 0.0f ? inner : 0.0f) - r;
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
float mouthShape(Mood mood, float x, float y) {
  const Mouth &m = MOUTHS[(uint8_t)mood];
  return sdRoundBoxTB(x, y, m.hx, m.hy, m.rTop, m.rBottom);
}

// One eye of one expression, in its own space, with the sign of `side` telling
// it which of the pair it is so anything slanted mirrors instead of repeating.
float eyeShape(Mood mood, float x, float y, float side, float squeeze) {
  switch (mood) {
    case Mood::Happy:
      // Neutral's blob, domed over the top and squared off underneath.
      return sdRoundBoxTB(x, y, 28.0f, 29.0f * squeeze, 28.0f, 9.0f);
    case Mood::Tired: {
      // A lid comes down onto a bottom edge that stays where it is. Scaling the
      // height alone takes the eye in from the top and the bottom at once,
      // which reads as an eye getting smaller rather than as one closing - and
      // that is the whole difference between sleepy and shrinking.
      constexpr float FULL = 29.0f;
      float half = FULL * squeeze;
      float ly = y - (FULL - half);

      // Rounded over the top, not squared, and not tilted at all. A flat edge
      // above a squat eye is a brow and reads as cross; a slant either way is
      // an emotion of its own, outward into sadness and inward into anger.
      // Tired is none of those - it is just slack.
      return sdRoundBoxTB(x, ly, 28.0f, half, 15.0f, 24.0f);
    }
    case Mood::Surprised:
      return sdEye(x, y, 34.0f, 40.0f * squeeze, 34.0f);
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
      constexpr float ARM = 20.0f;
      constexpr float THICK = 8.5f;
      float a = sdSegment(x, y, -ARM, -ARM, ARM, ARM) - THICK;
      float b = sdSegment(x, y, -ARM, ARM, ARM, -ARM) - THICK;
      return fminf(a, b);
    }
    case Mood::Neutral:
    default:
      return sdEye(x, y, 29.0f, 38.0f * squeeze, 23.0f);
  }
}

// Coverage of one eye, cross-faded while an expression is changing. Two shapes
// are only ever evaluated during the third of a second that takes.
// Interpolating the distances rather than the coverage is what makes this a
// morph: the boundary walks from one shape to the other and the eye deforms
// through the in-between. Fading the coverage instead would ghost one shape out
// while the other came up underneath it.
float coverFrom(float d) {
  float cover = clamp01(0.5f - d);
  float glow = d < GLOW_RADIUS ? clamp01(1.0f - d * GLOW_INV) : 0.0f;
  glow = glow * glow * GLOW_GAIN * (1.0f - cover);
  return cover + glow * 0.55f;
}

float mouthCoverage(float x, float y, float mix) {
  float d = mouthShape(me.mood, x, y);
  if (mix < 1.0f) {
    float was = mouthShape(me.was, x, y);
    d = was + (d - was) * mix;
  }
  return coverFrom(d);
}

float eyeCoverage(float x, float y, float side, float squeeze, float mix) {
  float d = eyeShape(me.mood, x, y, side, squeeze);
  if (mix < 1.0f) {
    float was = eyeShape(me.was, x, y, side, squeeze);
    d = was + (d - was) * mix;
  }
  return coverFrom(d);
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
  me.blend = 1.0f;
  me.mood = Mood::Neutral;
  me.was = Mood::Neutral;
  me.next = 1;  // neutral is where it starts, so a tap moves it on
  me.lookX = me.lookY = me.lookTX = me.lookTY = 0.0f;
  me.lookIn = 1.0f;
  me.taps = 0;
  me.tapWindow = 0.0f;
  me.crazy = 0.0f;
  me.flip = 0.0f;
  me.blinkIn = 2.0f;
  me.blinking = 0.0f;
  pickTarget();
}

void faceStep(float dt) {
  me.clock += dt;
  me.blend = clamp01(me.blend + dt / BLEND_SECONDS);

  me.held += dt;

  if (me.crazy > 0.0f) {
    me.crazy -= dt;
    me.flip -= dt;
    if (me.flip <= 0.0f) {
      // Snapped rather than morphed. A third of a second of easing between two
      // shapes is a long time when the whole point is that it cannot settle.
      me.flip = 0.10f;
      // From the set that is actually in use. Reaching across the whole enum
      // drags back the two expressions that were taken out of it.
      me.mood = CYCLE[random(0, CYCLE_COUNT)];
      me.was = me.mood;
      me.blend = 1.0f;
      me.held = 1.0f;
    }
    if (me.crazy <= 0.0f) {
      // And then it is over, and it is embarrassed about it.
      me.crazy = 0.0f;
      // The window starts again from here, so calming down and being set off
      // by the next single tap cannot happen.
      me.tapWindow = me.clock;
      me.taps = 0;
      me.was = me.mood;
      me.mood = Mood::Neutral;
      me.next = 1;
      me.blend = 0.0f;
      me.held = 0.0f;
      pickTarget();
    }
  }

  // A critically damped spring: it arrives without ringing, and the stiffness
  // alone is the difference between drifting over and darting.
  // Flung about while it is loose, and it changes its mind about where before
  // it ever arrives.
  const float k = me.crazy > 0.0f ? 70.0f : 9.0f;
  const float damping = 2.0f * sqrtf(k);
  me.vx += ((me.tx - me.x) * k - me.vx * damping) * dt;
  me.vy += ((me.ty - me.y) * k - me.vy * damping) * dt;
  me.x += me.vx * dt;
  me.y += me.vy * dt;

  float dx = me.tx - me.x;
  float dy = me.ty - me.y;
  if (dx * dx + dy * dy < (me.crazy > 0.0f ? 900.0f : 16.0f)) {
    pickTarget();
  }

  // Drifting on its own reads as floating. What reads as looking at things is
  // the shape of the movement: eyes snap to somewhere and hold there, mostly
  // nearby with the occasional long look off to one side. Easing them evenly
  // between points is a camera pan, not a creature.
  me.lookIn -= dt;
  if (me.lookIn <= 0.0f) {
    long roll = random(0, 100);
    if (me.crazy > 0.0f) {
      me.lookTX = frand(-24.0f, 24.0f);
      me.lookTY = frand(-13.0f, 13.0f);
      me.lookIn = frand(0.05f, 0.14f);
    } else if (me.mood == Mood::Tired) {
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

void faceProd() {
  // Nothing gets through while it is loose. Counting taps during a tantrum only
  // ever extends it, and letting one land would step the expression underneath
  // the flickering where it would not be seen anyway.
  if (me.crazy > 0.0f) {
    return;
  }

  // Five taps inside five seconds and it loses it. The window is rolling rather
  // than fixed, so a slow prod every four seconds never adds up to a tantrum.
  if (me.clock - me.tapWindow > 5.0f) {
    me.tapWindow = me.clock;
    me.taps = 0;
  }
  me.taps++;
  if (me.taps >= 5) {
    me.taps = 0;
    me.crazy = 4.5f;
    me.flip = 0.0f;
    return;
  }

  me.was = me.mood;
  me.blend = 0.0f;
  me.held = 0.0f;
  me.mood = CYCLE[me.next];
  me.next = (uint8_t)((me.next + 1) % CYCLE_COUNT);
}

void faceDraw(uint16_t *fb, int16_t *rowFrom, int16_t *rowTo) {
  // The float: a slow rise and fall that never quite repeats, plus a shiver
  // while it is excited.
  float floatY = sinf(me.clock * 1.5f) * 4.0f + sinf(me.clock * 0.61f) * 2.5f;
  float floatX = sinf(me.clock * 0.83f) * 3.0f;
  float centreX = me.x + floatX;
  float centreY = me.y + floatY;

  float squeeze = 1.0f;
  if (me.crazy > 0.0f) {
    // Shaking with it, on two frequencies that do not divide into one another
    // so it never falls into a rhythm.
    centreX += sinf(me.clock * 47.0f) * 7.0f + sinf(me.clock * 31.0f) * 4.0f;
    centreY += sinf(me.clock * 53.0f) * 6.0f + sinf(me.clock * 29.0f) * 3.5f;
  }
  if (me.mood == Mood::Tired && me.crazy <= 0.0f) {
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

  constexpr float REACH_X = 38.0f;
  constexpr float REACH_Y = 42.0f;
  constexpr float SPAN_X = 34.0f;   // the widest any eye gets
  constexpr float SPAN_Y = 41.0f;   // and the tallest
  constexpr float MOUTH_REACH_X = 22.0f;
  constexpr float MOUTH_REACH_Y = 22.0f;
  constexpr float MOUTH_SPAN_X = 19.0f;  // the widest the mouth gets
  constexpr float MOUTH_SPAN_Y = 19.0f;  // and the tallest
  constexpr float HALF_W = EYE_GAP + REACH_X + GLOW_RADIUS + 2.0f;

  float top = centreY - EYE_RISE - REACH_Y - GLOW_RADIUS - 2.0f;
  float bottom = centreY + MOUTH_DROP + MOUTH_REACH_Y + GLOW_RADIUS + 2.0f;

  // Only what the last frame painted can be holding anything, and clearing that
  // beats memsetting 253KB of PSRAM sixty times a second.
  float wasTop = me.paintedY - EYE_RISE - REACH_Y - GLOW_RADIUS - 2.0f;
  float wasBottom = me.paintedY + MOUTH_DROP + MOUTH_REACH_Y + GLOW_RADIUS + 2.0f;
  clearBox(fb, me.paintedX - HALF_W, wasTop, me.paintedX + HALF_W, wasBottom);
  clearBox(fb, centreX - HALF_W, top, centreX + HALF_W, bottom);
  float dirtyTop = wasTop < top ? wasTop : top;
  float dirtyBottom = wasBottom > bottom ? wasBottom : bottom;
  me.paintedX = centreX;
  me.paintedY = centreY;

  float eyeY = centreY - EYE_RISE + me.lookY * 0.5f;
  // The two do not travel together: the eye on the side being looked toward
  // carries a little more of the movement, and each carries a slow wobble of
  // its own.
  for (float side = -1.0f; side < 2.0f; side += 2.0f) {
    float eyeX = centreX + side * EYE_GAP + me.lookX * (0.5f + side * 0.12f) +
                 sinf(me.clock * 2.3f + (side > 0.0f ? 1.7f : 0.0f)) * 1.1f;
    int16_t x0 = (int16_t)(eyeX - REACH_X - GLOW_RADIUS);
    int16_t x1 = (int16_t)(eyeX + REACH_X + GLOW_RADIUS);
    int16_t y0 = (int16_t)(eyeY - REACH_Y - GLOW_RADIUS);
    int16_t y1 = (int16_t)(eyeY + REACH_Y + GLOW_RADIUS);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
    if (y1 > SCREEN_H - 1) y1 = SCREEN_H - 1;

    for (int16_t y = y0; y <= y1; y++) {
      float ly = (float)y + 0.5f - eyeY;
      // Past the tallest shape plus its glow there is nothing on this row, and
      // that is most of the rows near the top and bottom of the box.
      if (fabsf(ly) - SPAN_Y * squeeze > GLOW_RADIUS) {
        continue;
      }
      uint16_t *row = boardRow(fb, y);
      for (int16_t x = x1; x >= x0; x--) {
        float lx = (float)x + 0.5f - eyeX;
        if (fabsf(lx) - SPAN_X > GLOW_RADIUS) {
          continue;
        }
        float v = clamp01(eyeCoverage(lx, ly, side, squeeze, mix));
        if (v <= 0.004f) {
          continue;
        }
        uint8_t level = (uint8_t)(v * 255.0f);
        row[boardX(x)] =
            boardColour((uint16_t)(((level & 0xF8) << 8) | ((level & 0xFC) << 3) | (level >> 3)));
      }
    }
  }

  float mouthY = centreY + MOUTH_DROP;
  {
    int16_t x0 = (int16_t)(centreX - MOUTH_REACH_X - GLOW_RADIUS);
    int16_t x1 = (int16_t)(centreX + MOUTH_REACH_X + GLOW_RADIUS);
    int16_t y0 = (int16_t)(mouthY - MOUTH_REACH_Y - GLOW_RADIUS);
    int16_t y1 = (int16_t)(mouthY + MOUTH_REACH_Y + GLOW_RADIUS);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
    if (y1 > SCREEN_H - 1) y1 = SCREEN_H - 1;

    for (int16_t y = y0; y <= y1; y++) {
      float ly = (float)y + 0.5f - mouthY;
      if (fabsf(ly) - MOUTH_SPAN_Y > GLOW_RADIUS) {
        continue;
      }
      uint16_t *row = boardRow(fb, y);
      for (int16_t x = x1; x >= x0; x--) {
        float lx = (float)x + 0.5f - centreX;
        if (fabsf(lx) - MOUTH_SPAN_X > GLOW_RADIUS) {
          continue;
        }
        float v = clamp01(mouthCoverage(lx, ly, mix));
        if (v <= 0.004f) {
          continue;
        }
        uint8_t level = (uint8_t)(v * 255.0f);
        row[boardX(x)] =
            boardColour((uint16_t)(((level & 0xF8) << 8) | ((level & 0xFC) << 3) | (level >> 3)));
      }
    }
  }

  // The label is redrawn only when it says something new, so a settled face
  // leaves those rows alone and the flush never has to carry them.
  char wanted[12] = {0};
  {
    const char *name = me.crazy > 0.0f ? "AAAAAA" : moodName(me.mood);
    // One letter every twentieth of a second, so the name arrives as it is
    // typed rather than all at once under a face that is still changing.
    uint8_t shown = (uint8_t)(me.held / 0.05f);
    uint8_t i = 0;
    while (i < shown && i < sizeof(wanted) - 1 && name[i]) {
      wanted[i] = name[i];
      i++;
    }
  }
  if (strncmp(wanted, me.label, sizeof(wanted)) != 0) {
    clearBox(fb, 0.0f, 292.0f, SCREEN_W - 1, 312.0f);
    textDraw(fb, wanted, (int16_t)SCREEN_R, 296, 2, boardColour(0xFFFF));
    memcpy(me.label, wanted, sizeof(wanted));
    if (292.0f < dirtyTop) dirtyTop = 292.0f;
    if (312.0f > dirtyBottom) dirtyBottom = 312.0f;
  }

  // Screen rows run the other way to framebuffer rows, so the band is flipped
  // as well as clamped.
  int16_t sTop = dirtyTop < 0.0f ? 0 : (int16_t)dirtyTop;
  int16_t sBottom = dirtyBottom > SCREEN_H - 1 ? SCREEN_H - 1 : (int16_t)dirtyBottom;
  *rowFrom = (int16_t)(SCREEN_H - 1 - sBottom);
  *rowTo = (int16_t)(SCREEN_H - 1 - sTop);
}
