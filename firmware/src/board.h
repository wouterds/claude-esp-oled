#pragma once

#include <stdint.h>

#include "pins.h"

// Brings up the QSPI bus, the panel and the backlight, in that order. False
// means the panel never came up and there is nothing to draw on.
bool boardBegin();

// The offscreen framebuffer. Scenes draw here and boardFlush() sends it.
//
// It holds RGB565 in the panel's byte order, which is the reverse of the S3's.
// Swapping where a pixel is built costs nothing - the value is in a register
// either way - and it is the difference between the flush being a memcpy and
// being a loop over every pixel on the panel. Black and white are the same
// either way round, which is why memset and 0xFFFF need no thought.
uint16_t *boardFramebuffer();

inline uint16_t boardColour(uint16_t rgb565) {
  return (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
}

// The panel is mounted upside down. The turn is applied where pixels are
// written rather than in the flush or in the panel's own scan order: the flush
// touches every pixel on the glass and would have to give up its memcpy, and
// MADCTL would leave every coordinate in here reasoning about a screen that is
// the other way up. Drawing touches a few thousand pixels and can afford it.
inline uint16_t *boardRow(uint16_t *fb, int16_t y) {
  return fb + (int32_t)(SCREEN_H - 1 - y) * SCREEN_W;
}
inline int16_t boardX(int16_t x) { return SCREEN_W - 1 - x; }
void boardFlush();
void boardFlushRows(int16_t from, int16_t to);
