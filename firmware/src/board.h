#pragma once

#include <stdint.h>

// The panel is round and 360 across. Everything drawn on it is clipped by the
// glass rather than by the framebuffer, so a scene has to keep itself inside
// the inscribed circle - the corners of the buffer are simply not there.
static constexpr int16_t SCREEN_W = 360;
static constexpr int16_t SCREEN_H = 360;
static constexpr float SCREEN_R = 180.0f;

// Brings up the QSPI bus, the panel and the backlight, in that order. False
// means the panel never came up and there is nothing to draw on.
bool boardBegin();

// The offscreen framebuffer. Scenes draw here and boardFlush() sends it.
uint16_t *boardFramebuffer();
void boardFlush();

// 0 to 1023. Black on this panel is the crystal blocking a backlight that is
// always on, so this is the only real control over how black the black looks.
void boardBacklight(uint16_t duty);
