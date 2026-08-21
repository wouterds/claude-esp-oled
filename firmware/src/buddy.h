#pragma once

#include <stdint.h>

#include "face.h"

// What it is doing right now. Wander is the resting state; the rest interrupt
// it, run once and hand it back. Nothing here is ever chosen twice in a row by
// design - the pauses between antics are what make the antics read as choices.
enum class Act : uint8_t { Wander, Dart, Hop, Look, Giggle };

struct Buddy {
  float x, y;              // where it is, in panel pixels
  float tx, ty;            // where it wants to be
  float vx, vy;

  Act act;
  uint32_t actStart, actEnds;

  float lookX, lookY;      // eased, so the eyes arrive before the rest of it
  float mood;              // 0 calm, 1 delighted

  uint32_t blinkStart, blinkNext;
  bool blinkDouble;

  float sway;              // seconds since boot, drives breath and bob
};

void buddyBegin(Buddy &b);
void buddyUpdate(Buddy &b, float dt, uint32_t now);

// The face it wears this instant. Pure: hand it the same buddy and the same
// clock and it draws the same thing.
FaceParams buddyFace(const Buddy &b, uint32_t now);
