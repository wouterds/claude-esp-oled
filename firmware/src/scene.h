#pragma once

#include <stdint.h>

// Eighteen balls loose on the glass, six of each primary, drawn at half opacity
// over a grid. Stepping and drawing are separate so the clock never reaches the
// framebuffer: the same state always draws the same pixels.
void sceneBegin();
void sceneStep(float dt);
void sceneDraw(uint16_t *fb);
