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

// What a fraction of a pixel is worth on this glass. The LCD board's black is a
// backlight leaking through liquid crystal, so the faint end of an antialiased
// edge falls under that floor and is never seen. The AMOLED's black is a pixel
// that is off, so the same faint end is a lit halo around everything with an
// edge - which reads as softness rather than as smoothing, and is why an icon
// that looked crisp on one board looks blurred on the other.
//
// Squaring pulls the dim end down to where it reads as an edge again and leaves
// full coverage alone. The face is deliberately not put through this: its glow
// is the one thing here that is meant to be a halo.
inline float boardInk(float coverage) {
#if defined(BOARD_AMOLED_175C)
  return coverage * coverage;
#else
  return coverage;
#endif
}

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

// The LCD board's panel is mounted upside down. The turn is applied where
// pixels are written rather than in the flush or in the panel's own scan order:
// the flush touches every pixel on the glass and would have to give up its
// memcpy, and MADCTL would leave every coordinate in here reasoning about a
// screen that is the other way up. Drawing touches a few thousand pixels and
// can afford it. The AMOLED board is not mounted that way and its panel undoes
// this turn in MADCTL, so that scenes stay written the one way.
inline uint16_t *boardRow(uint16_t *fb, int16_t y) {
  return fb + (int32_t)(SCREEN_H - 1 - y) * SCREEN_W;
}
inline int16_t boardX(int16_t x) { return SCREEN_W - 1 - x; }
void boardFlush();
void boardFlushRows(int16_t from, int16_t to);
