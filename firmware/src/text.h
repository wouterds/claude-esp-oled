#pragma once

#include <stdint.h>

// Digits and a full stop, which is an address and nothing else. They are not a
// bitmap: each one is a handful of strokes measured in pixels and filled by
// distance, so the curves are curves and the edges are soft. A five by seven
// blown up to twice size is legible and looks like a cheap clock; this is nine
// pixels tall and looks drawn.
void textDraw(uint16_t *fb, const char *s, int16_t centreX, int16_t centreY, uint16_t colour);

// How far apart their centres sit.
int16_t textStep();
