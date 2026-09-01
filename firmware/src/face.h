#pragma once

#include <stdint.h>

// A face - two eyes and a mouth - and nothing else. It drifts around the glass
// and wears whatever the two numbers on the edges of it add up to: pleased
// while there is room, cross near the ceiling, and gone at it.
void faceBegin();
void faceStep(float dt);

// Draws, and reports the box of framebuffer rows and columns it touched so the
// flush can leave the rest of the panel alone. The columns matter as much as
// the rows: the bars and the figures share these rows and never move.
void faceDraw(uint16_t *fb, int16_t *rowFrom, int16_t *rowTo, int16_t *colFrom, int16_t *colTo);
