#pragma once

#include <stdint.h>

// A face - two eyes and a mouth - and nothing else. It drifts around the glass
// and wears whatever the two numbers on the edges of it add up to: pleased
// while there is room, cross near the ceiling, and gone at it.
void faceBegin();
void faceStep(float dt);

// A box of the framebuffer, the way boardFlushRect takes one.
struct FaceBox {
  int16_t x0, x1, from, to;
};

// Draws, and reports the two boxes of framebuffer it touched - the eyes, then
// the mouth - so the flush can leave the rest of the panel alone. The columns
// matter as much as the rows: the bars and the figures share these rows and
// never move.
void faceDraw(uint16_t *fb, FaceBox touched[2]);
