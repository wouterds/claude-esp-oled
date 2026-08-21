#pragma once

#include <stdint.h>

// Uppercase, digits, a full stop and a percent, five by seven, centred on x.
// Enough to name an address and a charge level and nothing more - a font is a
// lot of flash to spend on a handful of words.
//
// Whole multiples only. A bitmap glyph scaled by anything else has to double
// some of its columns and not others, and five pixels of letterform will not
// survive that - it comes out as something you can tell is text and cannot
// read.
void textDraw(uint16_t *fb, const char *s, int16_t centreX, int16_t top, int16_t scale,
              uint16_t colour);

// How wide one glyph and the gap after it come out at that size.
int16_t textStep(int16_t scale);
