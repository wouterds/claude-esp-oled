#pragma once

#include <stdint.h>

// Uppercase and space, five by seven, centred on x. Enough to name a mood and
// nothing more - a font is a lot of flash to spend on seven words.
void textDraw(uint16_t *fb, const char *s, int16_t centreX, int16_t top, int16_t scale,
              uint16_t colour);
