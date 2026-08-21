#include "buddy.h"

#include <Arduino.h>
#include <esp_random.h>
#include <math.h>

#include "board.h"

namespace {

constexpr float PI_F = 3.14159265f;
constexpr float TWO_PI_F = 6.28318531f;
constexpr float HALF_PI_F = 1.57079633f;

constexpr float CENTRE = SCREEN_R;
// How far from the middle the face may sit. Its own reach is about 107 pixels
// and the glass stops at 180, so this is what is left - the panel is round and
// a face that wandered past this would lose a cheek to the bezel.
constexpr float ROAM = 67.0f;

float frand(float lo, float hi) {
  return lo + (hi - lo) * (float)random(0, 10001) * 0.0001f;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }

float mix(float a, float b, float t) { return a + (b - a) * t; }

void pickTarget(Buddy &b, float minTravel) {
  for (int tries = 0; tries < 8; tries++) {
    float angle = frand(0.0f, TWO_PI_F);
    // sqrt spreads the points evenly over the disc instead of crowding them
    // into the middle, so it uses the whole panel rather than hovering.
    float radius = ROAM * sqrtf(frand(0.0f, 1.0f));
    float nx = CENTRE + cosf(angle) * radius;
    float ny = CENTRE + sinf(angle) * radius;
    float dx = nx - b.x;
    float dy = ny - b.y;
    if (tries == 7 || sqrtf(dx * dx + dy * dy) >= minTravel) {
      b.tx = nx;
      b.ty = ny;
      return;
    }
  }
}

void enterAct(Buddy &b, Act act, uint32_t now) {
  b.act = act;
  b.actStart = now;
  switch (act) {
    case Act::Wander:
      pickTarget(b, 20.0f);
      b.actEnds = now + (uint32_t)frand(1100.0f, 2600.0f);
      break;
    case Act::Dart:
      pickTarget(b, 72.0f);
      b.actEnds = now + (uint32_t)frand(700.0f, 1050.0f);
      break;
    case Act::Hop:
      b.actEnds = now + (uint32_t)frand(640.0f, 900.0f);
      break;
    case Act::Look:
      b.actEnds = now + (uint32_t)frand(1100.0f, 2000.0f);
      break;
    case Act::Giggle:
      b.actEnds = now + (uint32_t)frand(900.0f, 1500.0f);
      break;
  }
}

Act nextAct(const Buddy &b) {
  if (b.act != Act::Wander) {
    return Act::Wander;
  }
  long roll = random(0, 100);
  if (roll < 26) {
    return Act::Dart;
  }
  if (roll < 50) {
    return Act::Hop;
  }
  if (roll < 76) {
    return Act::Look;
  }
  if (roll < 90) {
    return Act::Giggle;
  }
  return Act::Wander;
}

float actProgress(const Buddy &b, uint32_t now) {
  uint32_t span = b.actEnds - b.actStart;
  uint32_t elapsed = now - b.actStart;
  if (span == 0 || elapsed >= span) {
    return 1.0f;
  }
  return (float)elapsed / (float)span;
}

float blinkClose(const Buddy &b, uint32_t now) {
  uint32_t span = b.blinkDouble ? 420u : 190u;
  uint32_t elapsed = now - b.blinkStart;
  if (elapsed >= span) {
    return 0.0f;
  }
  float phase = (float)elapsed / (float)span;
  // Two humps for a double, one for a single - a blink is the cheapest thing
  // on the panel and the one that does most of the work.
  return b.blinkDouble ? fabsf(sinf(phase * TWO_PI_F)) : sinf(phase * PI_F);
}

}  // namespace

void buddyBegin(Buddy &b) {
  randomSeed(esp_random());
  b.x = b.tx = CENTRE;
  b.y = b.ty = CENTRE;
  b.vx = b.vy = 0.0f;
  b.lookX = b.lookY = 0.0f;
  b.mood = 0.5f;
  b.sway = 0.0f;
  b.blinkStart = 0;
  b.blinkDouble = false;
  b.blinkNext = millis() + 1400;
  enterAct(b, Act::Wander, millis());
}

void buddyUpdate(Buddy &b, float dt, uint32_t now) {
  if ((int32_t)(now - b.actEnds) >= 0) {
    enterAct(b, nextAct(b), now);
  }

  // A critically damped spring: it arrives without ringing, and the stiffness
  // on its own is the whole difference between drifting over and darting.
  float k = 14.0f;
  if (b.act == Act::Dart) {
    k = 58.0f;
  } else if (b.act == Act::Look) {
    k = 26.0f;
  } else if (b.act == Act::Giggle) {
    k = 34.0f;
  }
  float damping = 2.0f * sqrtf(k);
  b.vx += ((b.tx - b.x) * k - b.vx * damping) * dt;
  b.vy += ((b.ty - b.y) * k - b.vy * damping) * dt;
  b.x += b.vx * dt;
  b.y += b.vy * dt;

  float wantX = clampf((b.tx - b.x) * 0.09f, -8.0f, 8.0f);
  float wantY = clampf((b.ty - b.y) * 0.07f, -6.0f, 6.0f);
  if (b.act == Act::Look) {
    // Nothing to travel to, so the eyes go wandering on their own.
    wantX = cosf(b.sway * 2.3f) * 7.5f;
    wantY = sinf(b.sway * 1.7f) * 4.5f;
  }
  float ease = 1.0f - expf(-dt * 11.0f);
  b.lookX += (wantX - b.lookX) * ease;
  b.lookY += (wantY - b.lookY) * ease;

  float wantMood = 0.35f;
  if (b.act == Act::Giggle) {
    wantMood = 1.0f;
  } else if (b.act == Act::Dart) {
    wantMood = 0.75f;
  } else if (b.act == Act::Hop) {
    wantMood = 0.8f;
  }
  b.mood += (wantMood - b.mood) * (1.0f - expf(-dt * 3.5f));

  b.sway += dt;

  if ((int32_t)(now - b.blinkNext) >= 0) {
    b.blinkStart = now;
    b.blinkDouble = random(0, 100) < 18;
    b.blinkNext = now + (uint32_t)frand(2400.0f, 6200.0f);
  }
}

FaceParams buddyFace(const Buddy &b, uint32_t now) {
  FaceParams p;
  float t = b.sway;
  float progress = actProgress(b, now);

  // Squash is positive for wide and short, negative for tall and thin.
  float hop = 0.0f;
  float squash = 0.0f;
  if (b.act == Act::Hop) {
    if (progress < 0.22f) {
      // The crouch before it. Without the anticipation a hop reads as the face
      // merely being somewhere higher.
      squash = sinf((progress / 0.22f) * PI_F) * 0.16f;
    } else if (progress < 0.85f) {
      float e = (progress - 0.22f) / 0.63f;
      hop = -sinf(e * PI_F) * 46.0f;
      squash = -0.10f * sinf(e * PI_F);
    } else {
      squash = sinf(((progress - 0.85f) / 0.15f) * PI_F) * 0.20f;
    }
  }

  float jitterX = 0.0f;
  float jitterY = 0.0f;
  if (b.act == Act::Giggle) {
    float envelope = sinf(progress * PI_F);
    jitterX = sinf(t * 34.0f) * 5.5f * envelope;
    jitterY = -fabsf(sinf(t * 17.0f)) * 7.0f * envelope;
  }

  p.breath = 1.0f + sinf(t * 2.1f) * 0.018f;
  p.x = b.x + jitterX;
  p.y = b.y + sinf(t * 1.6f) * 3.2f + hop + jitterY;
  p.tilt = sinf(t * 0.9f) * 0.045f;
  if (b.act == Act::Look) {
    p.tilt += sinf(t * 1.3f) * 0.09f;
  }

  if (b.act == Act::Hop) {
    p.stretchAngle = squash >= 0.0f ? 0.0f : HALF_PI_F;
    p.stretch = 1.0f + fabsf(squash);
  } else {
    float speed = sqrtf(b.vx * b.vx + b.vy * b.vy);
    p.stretchAngle = speed > 8.0f ? atan2f(b.vy, b.vx) : 0.0f;
    p.stretch = 1.0f + clampf(speed * 0.0013f, 0.0f, 0.20f);
  }

  // A dart starts with a jolt: eyes wide and the mouth a round o, easing back
  // into the smile it was wearing over the first third of the move.
  float surprise = b.act == Act::Dart ? clamp01(1.0f - progress * 2.6f) : 0.0f;

  p.eyeGap = 42.0f;
  p.eyeW = 22.0f + surprise * 4.0f;
  p.eyeH = 30.0f + surprise * 5.0f;
  p.eyeY = -24.0f;
  p.eyeRadius = 17.0f;
  p.lookX = b.lookX;
  p.lookY = b.lookY;
  p.brow = 0.0f;
  p.happy = b.mood * b.mood * 0.95f * (1.0f - surprise);

  float close = blinkClose(b, now);
  p.eyeH = mix(p.eyeH, 3.0f, close);
  p.happy *= 1.0f - close;
  // The rounding cannot outrun the half-extent it rounds, and a blink drives
  // the height straight through it.
  float maxRadius = p.eyeW < p.eyeH ? p.eyeW : p.eyeH;
  if (p.eyeRadius > maxRadius) {
    p.eyeRadius = maxRadius;
  }

  float glee = b.mood;
  p.mouthY = 44.0f;
  p.mouthR = 22.0f + glee * 6.0f;
  p.mouthT = 6.5f + glee * 2.0f;
  p.mouthOpen = 0.80f + glee * 0.45f;
  p.mouthSplit = 0.0f;
  if (b.act == Act::Giggle) {
    // Two lobes about their own radius apart, so they meet in a cusp. Left at
    // the smile's radius they would sit inside one another and read as one
    // wide mouth that had gone slightly wrong.
    p.mouthSplit = 14.0f * glee;
    p.mouthR = mix(p.mouthR, 15.0f, glee);
    p.mouthT = mix(p.mouthT, 6.0f, glee);
    p.mouthOpen = mix(p.mouthOpen, 1.50f, glee);
  }
  if (surprise > 0.0f) {
    p.mouthR = mix(p.mouthR, 9.0f, surprise);
    p.mouthT = mix(p.mouthT, 9.0f, surprise);
    p.mouthOpen = mix(p.mouthOpen, PI_F, surprise);
    p.mouthSplit *= 1.0f - surprise;
  }

  return p;
}
