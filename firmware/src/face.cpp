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
constexpr float GLOW_RADIUS = 13.0f;
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

constexpr float MOOD_SECONDS = 5.0f;
constexpr float BLEND_SECONDS = 0.35f;
constexpr float INTRO_SECONDS = 4.1f;

enum class Mood : uint8_t { Neutral, Happy, Surprised, Excited, Angry, Dead, Love };

// Constants, but sinf and cosf are not free and the shapes below are asked for
// them thirty thousand times a frame. Filled once, in faceBegin.
struct Trig {
  float archSin, archCos;
  float browSin, browCos;
};
Trig trig;

// Neutral is not in here: it is what every other one returns to.
constexpr Mood MOODS[] = {Mood::Happy,  Mood::Surprised, Mood::Excited,
                          Mood::Angry,  Mood::Dead,      Mood::Love};
constexpr uint8_t MOOD_COUNT = sizeof(MOODS) / sizeof(MOODS[0]);

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
  float blinkIn;       // seconds until the next blink
  float blinking;      // seconds left of this one
  float intro;         // seconds left of waking up, zero once it is awake
};

// A point on a scripted curve. Between two of them it eases rather than ramps,
// so nothing in the wake-up starts or stops abruptly.
struct Key {
  float at;
  float value;
};

// Heavy lids, one look that does not take, then open. Waking is the pause
// before the second attempt rather than the opening itself.
constexpr Key WAKE_LIDS[] = {{0.00f, 0.04f}, {0.55f, 0.05f}, {0.80f, 0.50f},
                             {1.00f, 0.08f}, {1.45f, 1.00f}};
// Then a look around, left first and held, because a head turning back too
// soon reads as a twitch rather than as looking at something.
constexpr Key WAKE_LOOK_X[] = {{1.45f, 0.0f},  {1.90f, -30.0f}, {2.40f, -30.0f},
                               {2.90f, 30.0f}, {3.40f, 30.0f},  {3.90f, 0.0f}};
constexpr Key WAKE_LOOK_Y[] = {{1.45f, 0.0f}, {2.00f, -12.0f}, {2.60f, 10.0f},
                               {3.20f, -8.0f}, {3.90f, 0.0f}};

template <uint8_t N>
float keyed(const Key (&keys)[N], float t) {
  if (t <= keys[0].at) {
    return keys[0].value;
  }
  for (uint8_t i = 1; i < N; i++) {
    if (t > keys[i].at) {
      continue;
    }
    float span = keys[i].at - keys[i - 1].at;
    float e = span > 0.0001f ? (t - keys[i - 1].at) / span : 1.0f;
    e = e * e * (3.0f - 2.0f * e);
    return keys[i - 1].value + (keys[i].value - keys[i - 1].value) * e;
  }
  return keys[N - 1].value;
}

const char *moodName(Mood mood) {
  switch (mood) {
    case Mood::Happy:
      return "HAPPY";
    case Mood::Surprised:
      return "SURPRISED";
    case Mood::Excited:
      return "EXCITED";
    case Mood::Angry:
      return "ANGRY";
    case Mood::Dead:
      return "DEAD";
    case Mood::Love:
      return "IN LOVE";
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

inline float sdSegment(float px, float py, float ax, float ay, float bx, float by) {
  float pax = px - ax;
  float pay = py - ay;
  float bax = bx - ax;
  float bay = by - ay;
  float h = clamp01((pax * bax + pay * bay) / (bax * bax + bay * bay));
  return sqrtf(dot2(pax - bax * h, pay - bay * h));
}

// An arc of radius ra and thickness rb opening by `aperture` either side of +y.
// y runs down, so negating it turns the smile of a mouth into the arch of a
// happy eye.
inline float sdArc(float px, float py, float sinA, float cosA, float ra, float rb) {
  px = fabsf(px);
  if (cosA * px > sinA * py) {
    return sqrtf(dot2(px - sinA * ra, py - cosA * ra)) - rb;
  }
  return fabsf(sqrtf(px * px + py * py) - ra) - rb;
}

// Inigo Quilez's heart: point at the origin, lobes above it, y running up.
inline float sdHeart(float px, float py) {
  px = fabsf(px);
  if (py + px > 1.0f) {
    return sqrtf(dot2(px - 0.25f, py - 0.75f)) - 0.35355339f;
  }
  float m = 0.5f * (px + py > 0.0f ? px + py : 0.0f);
  float best = fminf(dot2(px, py - 1.0f), dot2(px - m, py - m));
  return sqrtf(best) * (px - py < 0.0f ? -1.0f : 1.0f);
}

// The mouth is one rounded blob throughout, with a circle taken out of it: from
// above it leaves the crescent of a smile, from below the arch of a frown, and
// with no circle at all it is the blob itself. One shape, so it deforms between
// expressions rather than being swapped out.
struct Mouth {
  float w, h, r;
  float curve;  // how far the ends lift: up smiles, down frowns, zero is a blob
};

// Thin bars, because a bent bar thick enough to look like a blob at rest looks
// like a wedge once it is bent. The roundness comes from the corner radius
// being the whole of the half-height: every one of these is a capsule.
constexpr Mouth MOUTHS[] = {
    {15.0f, 4.5f, 4.5f, 3.0f},     // neutral, a bar with the hint of a smile
    {25.0f, 5.0f, 5.0f, 15.0f},    // happy
    {12.0f, 13.0f, 12.0f, 0.0f},   // surprised, an o
    {21.0f, 9.5f, 9.5f, 12.0f},    // excited, open and grinning
    {21.0f, 5.0f, 5.0f, -12.0f},   // angry
    {19.0f, 4.0f, 4.0f, 0.0f},     // dead, flat
    {21.0f, 5.5f, 5.5f, 13.0f},    // in love
};

// The blob is bent rather than cut. Taking a circle out of it leaves the ends
// of the crescent in points, which is the one thing a mouth made of a rounded
// blob should not have - bending keeps every side as round as it started.
float mouthShape(Mood mood, float x, float y) {
  const Mouth &m = MOUTHS[(uint8_t)mood];
  float lift = m.curve * (x * x) / (m.w * m.w);
  return sdRoundBox(x, y + lift, m.w, m.h, m.r);
}

// One eye of one expression, in its own space, with the sign of `side` telling
// it which of the pair it is so anything slanted mirrors instead of repeating.
float eyeShape(Mood mood, float x, float y, float side, float squeeze) {
  switch (mood) {
    case Mood::Happy:
      // An arch, drawn upside down from a smile.
      return sdArc(x, -y + 10.0f, trig.archSin, trig.archCos, 26.0f, 7.5f);
    case Mood::Surprised:
      return sdRoundBox(x, y, 34.0f, 40.0f * squeeze, 34.0f);
    case Mood::Excited: {
      // A round eye with a glint taken out of it. Both boundaries are circles,
      // so nothing here ends in a corner, and the hole reads as a highlight
      // because the eye is the only lit thing on the panel.
      float d = sdRoundBox(x, y, 31.0f, 34.0f * squeeze, 31.0f);
      float gx = x + 11.0f;
      float gy = y + 12.0f;
      return fmaxf(d, -(sqrtf(gx * gx + gy * gy) - 8.5f));
    }
    case Mood::Angry: {
      // Just tilted, and shorter than it is wide. Cutting the top flat is the
      // obvious way to draw a brow and it leaves a hard edge across the one
      // shape that should be round the whole way round; the slant carries it
      // on its own.
      float c = trig.browCos;
      float s = trig.browSin * side;
      float rx = x * c - y * s;
      float ry = x * s + y * c;
      return sdRoundBox(rx, ry, 29.0f, 19.0f * squeeze, 18.0f);
    }
    case Mood::Dead: {
      constexpr float ARM = 20.0f;
      constexpr float THICK = 8.5f;
      float a = sdSegment(x, y, -ARM, -ARM, ARM, ARM) - THICK;
      float b = sdSegment(x, y, -ARM, ARM, ARM, -ARM) - THICK;
      return fminf(a, b);
    }
    case Mood::Love: {
      // Narrower than it is tall, because the heart is naturally squat and at
      // equal scales it reads as a shield.
      constexpr float SX = 26.0f;
      constexpr float SY = 34.0f;
      return sdHeart(x / SX, -y / SY + 0.6f) * SX;
    }
    case Mood::Neutral:
    default:
      return sdRoundBox(x, y, 29.0f, 38.0f * squeeze, 23.0f);
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
  me.next = 0;
  me.blinkIn = 2.0f;
  me.blinking = 0.0f;
  me.intro = INTRO_SECONDS;
  pickTarget();
}

void faceStep(float dt) {
  me.clock += dt;
  me.blend = clamp01(me.blend + dt / BLEND_SECONDS);

  // Nothing has a mood yet while it is still working out where it is.
  if (me.intro > 0.0f) {
    me.intro -= dt;
    if (me.intro > 0.0f) {
      return;
    }
    me.intro = 0.0f;
    me.held = 0.0f;
  }

  me.held += dt;
  if (me.held >= MOOD_SECONDS) {
    me.held = 0.0f;
    me.was = me.mood;
    me.blend = 0.0f;
    if (me.mood == Mood::Neutral) {
      me.mood = MOODS[me.next];
      me.next = (uint8_t)((me.next + 1) % MOOD_COUNT);
    } else {
      me.mood = Mood::Neutral;
    }
  }

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
  if (me.mood == Mood::Excited) {
    floatX += sinf(me.clock * 31.0f) * 2.5f;
    floatY += sinf(me.clock * 27.0f) * 2.0f;
  }
  float centreX = me.x + floatX;
  float centreY = me.y + floatY;

  float squeeze = 1.0f;
  if (me.blinking > 0.0f) {
    squeeze = 0.06f + 0.94f * fabsf(cosf((me.blinking / 0.16f) * PI_F));
  }
  if (me.intro > 0.0f) {
    float woke = INTRO_SECONDS - me.intro;
    squeeze = keyed(WAKE_LIDS, woke);
    centreX += keyed(WAKE_LOOK_X, woke);
    centreY += keyed(WAKE_LOOK_Y, woke);
  }
  float mix = me.blend;

  constexpr float REACH_X = 38.0f;
  constexpr float REACH_Y = 48.0f;
  constexpr float SPAN_X = 34.0f;   // the widest any eye gets
  constexpr float SPAN_Y = 43.0f;   // and the tallest
  constexpr float MOUTH_REACH_X = 30.0f;
  constexpr float MOUTH_REACH_Y = 32.0f;
  constexpr float MOUTH_SPAN_X = 28.0f;  // the widest the mouth gets
  constexpr float MOUTH_SPAN_Y = 27.0f;  // and the tallest once it is bent
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

  float eyeY = centreY - EYE_RISE;
  for (float side = -1.0f; side < 2.0f; side += 2.0f) {
    float eyeX = centreX + side * EYE_GAP;
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
  if (me.intro <= 0.0f) {
    const char *name = moodName(me.mood);
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
