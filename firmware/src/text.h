#pragma once

#include <stdint.h>

// Uppercase, digits, a full stop and a percent, five by seven, centred on x.
// Enough to name a mood, an address and a charge level and nothing more - a font
// is a lot of flash to spend on a handful of words.
//
// The size is in quarters, so 4 is actual size, 5 is a quarter bigger and 8 is
// double. Whole multiples alone left nothing between a font too small to read
// across a desk and one taking a third of the panel, and halves left nothing
// between those two either.
void textDraw(uint16_t *fb, const char *s, int16_t centreX, int16_t top, int16_t quarters,
              uint16_t colour);

// How wide one glyph and the gap after it come out at that size.
int16_t textStep(int16_t quarters);
