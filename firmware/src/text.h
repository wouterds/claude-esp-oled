#pragma once

#include <stdint.h>

// Uppercase, digits, a full stop and a percent, five by seven, centred on x.
// Enough to name a mood, an address and a charge level and nothing more - a font
// is a lot of flash to spend on a handful of words.
//
// The size is in halves, so 2 is actual size and 3 is half again as big. Whole
// numbers alone left nothing between a font too small to read across a desk and
// one that takes half the panel.
void textDraw(uint16_t *fb, const char *s, int16_t centreX, int16_t top, int16_t halves,
              uint16_t colour);

// How wide one glyph and the gap after it come out at that size.
int16_t textStep(int16_t halves);
