#pragma once

#include <stdint.h>

#include "pins.h"

// The scene was composed against the 360x360 glass and every distance in it is
// written in that panel's pixels. On a larger piece of glass they are scaled by
// this rather than re-tuned by hand, so the composition is worked out once.
//
// Exactly 1 on the board it was drawn for, so that board's constants are the
// numbers that were already tested rather than the same numbers round-tripped
// through a multiply. Only lengths take it: the seconds, the rates and the
// percentages beside them are not distances and do not move with the glass.
static constexpr float SCENE = SCREEN_R / 180.0f;

// Brings up the QSPI bus, the panel and the backlight, in that order, and
// lights it at the level given - the stored one, so the glass never comes up
// at full and then drops. False means the panel never came up and there is
// nothing to draw on.
bool boardBegin(uint8_t brightness);

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

// The LCD board's panel is mounted upside down. The turn is applied where
// pixels are written rather than in the flush or in the panel's own scan order:
// the flush touches every pixel on the glass and would have to give up its
// memcpy, and MADCTL would leave every coordinate in here reasoning about a
// screen that is the other way up. Drawing touches a few thousand pixels and
// can afford it. The AMOLED board's glass is mounted the same way up, so the
// turn written here is already the one it wants and its panel applies no
// transform at all - scenes stay written the one way on both.
inline uint16_t *boardRow(uint16_t *fb, int16_t y) {
  return fb + (int32_t)(SCREEN_H - 1 - y) * SCREEN_W;
}
inline int16_t boardX(int16_t x) { return SCREEN_W - 1 - x; }
void boardFlush();
void boardFlushRows(int16_t from, int16_t to);

// Only the box that changed. The face is about half the width of the glass, so
// flushing whole rows for it sends the bars and the figures beside it again on
// every frame - bandwidth spent on pixels that did not move, and the panel
// rewritten under its own scan for nothing, which is what static text beside a
// moving face is seen to shimmer from.
void boardFlushRect(int16_t x0, int16_t x1, int16_t from, int16_t to);
