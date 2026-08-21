#pragma once

#include <stdint.h>

// A face - two eyes and a mouth - and nothing else. It drifts around the glass,
// and every five seconds it takes on an expression and every five after that
// lets it go.
void faceBegin();
void faceStep(float dt);

// Draws, and reports the band of framebuffer rows it touched so the flush can
// leave the rest of the panel alone.
void faceDraw(uint16_t *fb, int16_t *rowFrom, int16_t *rowTo);
