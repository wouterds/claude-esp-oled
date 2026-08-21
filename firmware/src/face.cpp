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
constexpr float GLOW_GAIN = 0.5f;

// How far from the middle the eyes may drift. Their own reach is about 106
// pixels at the widest expression and the glass stops at 180, so this is what
// is left - the panel is round and anything past this loses a corner to it.
constexpr float ROAM = 66.0f;
constexpr float EYE_GAP = 52.0f;

constexpr float MOOD_SECONDS = 5.0f;
constexpr float BLEND_SECONDS = 0.35f;
constexpr float INTRO_SECONDS = 4.1f;

enum class Mood : uint8_t { Neutral, Happy, Surprised, Excited, Angry, Dead, Love };

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

// One eye of one expression, in its own space, with the sign of `side` telling
// it which of the pair it is so anything slanted mirrors instead of repeating.
float eyeShape(Mood mood, float x, float y, float side, float squeeze) {
  switch (mood) {
    case Mood::Happy: {
      // An arch, drawn upside down from a smile.
      float a = 1.15f;
      return sdArc(x, -y + 10.0f, sinf(a), cosf(a), 26.0f, 7.5f);
    }
    case Mood::Surprised:
      return sdRoundBox(x, y, 34.0f, 40.0f * squeeze, 34.0f);
    case Mood::Excited:
      return sdRoundBox(x, y, 32.0f, 42.0f * squeeze, 30.0f);
    case Mood::Angry: {
      // Tilted, then cut flat across the top in its own frame - which puts the
      // cut on a slant on the glass, and a slant is the whole of an angry eye.
      float a = 0.42f * side;
      float c = cosf(a);
      float s = sinf(a);
      // Dropped before it is cut, so what survives the cut sits on the centre
      // rather than below it.
      float oy = y + 12.0f;
      float rx = x * c - oy * s;
      float ry = x * s + oy * c;
      float d = sdRoundBox(rx, ry, 30.0f, 36.0f * squeeze, 16.0f);
      return fmaxf(d, -8.0f - ry);
    }
    case Mood::Dead: {
      float arm = 21.0f;
      float a = sdSegment(x, y, -arm, -arm, arm, arm) - 5.5f;
      float b = sdSegment(x, y, -arm, arm, arm, -arm) - 5.5f;
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
float eyeCoverage(float x, float y, float side, float squeeze, float mix) {
  float d = eyeShape(me.mood, x, y, side, squeeze);
  float cover = clamp01(0.5f - d);
  float glow = d < GLOW_RADIUS ? clamp01(1.0f - d / GLOW_RADIUS) : 0.0f;
  glow = glow * glow * GLOW_GAIN * (1.0f - cover);

  if (mix < 1.0f) {
    float e = eyeShape(me.was, x, y, side, squeeze);
    float wasCover = clamp01(0.5f - e);
    float wasGlow = e < GLOW_RADIUS ? clamp01(1.0f - e / GLOW_RADIUS) : 0.0f;
    wasGlow = wasGlow * wasGlow * GLOW_GAIN * (1.0f - wasCover);
    cover = wasCover + (cover - wasCover) * mix;
    glow = wasGlow + (glow - wasGlow) * mix;
  }
  return cover + glow * 0.55f;
}

void pickTarget() {
  float angle = frand(0.0f, 2.0f * PI_F);
  // sqrt spreads the targets evenly over the disc instead of crowding them into
  // the middle, so it uses the whole panel rather than hovering.
  float radius = ROAM * sqrtf(frand(0.0f, 1.0f));
  me.tx = SCREEN_R + cosf(angle) * radius;
  me.ty = SCREEN_R + sinf(angle) * radius;
}

}  // namespace

void faceBegin() {
  randomSeed(esp_random());
  me.x = me.tx = SCREEN_R;
  me.y = me.ty = SCREEN_R;
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

void faceDraw(uint16_t *fb) {
  memset(fb, 0, (size_t)SCREEN_W * SCREEN_H * 2);

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

  // Generous enough for the widest expression and the glow around it. The eyes
  // are small against the panel, so this is a fraction of it either way.
  constexpr int16_t REACH_X = 46;
  constexpr int16_t REACH_Y = 58;
  for (float side = -1.0f; side < 2.0f; side += 2.0f) {
    float eyeX = centreX + side * EYE_GAP;
    int16_t x0 = (int16_t)(eyeX - REACH_X - GLOW_RADIUS);
    int16_t x1 = (int16_t)(eyeX + REACH_X + GLOW_RADIUS);
    int16_t y0 = (int16_t)(centreY - REACH_Y - GLOW_RADIUS);
    int16_t y1 = (int16_t)(centreY + REACH_Y + GLOW_RADIUS);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
    if (y1 > SCREEN_H - 1) y1 = SCREEN_H - 1;

    for (int16_t y = y0; y <= y1; y++) {
      uint16_t *row = fb + (int32_t)y * SCREEN_W;
      float ly = (float)y + 0.5f - centreY;
      for (int16_t x = x0; x <= x1; x++) {
        float lx = (float)x + 0.5f - eyeX;
        float v = clamp01(eyeCoverage(lx, ly, side, squeeze, mix));
        if (v <= 0.004f) {
          continue;
        }
        uint8_t level = (uint8_t)(v * 255.0f);
        uint16_t pixel = (uint16_t)(((level & 0xF8) << 8) | ((level & 0xFC) << 3) | (level >> 3));
        if (pixel > row[x]) {
          row[x] = pixel;
        }
      }
    }
  }

  // Nothing to name until it has woken up and settled on something.
  if (me.intro <= 0.0f) {
    textDraw(fb, moodName(me.mood), SCREEN_R, 296, 2, 0xFFFF);
  }
}
