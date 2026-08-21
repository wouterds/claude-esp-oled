#pragma once

#include <stdint.h>

// Two arcs hugging the left and right of the glass, each a track with a filled
// part. The values are picked once and do not move, so the pixels are worked
// out once too and put back every frame - the face wanders far enough sideways
// to wipe them, and its own flush is the only cheap chance to repair them.
void gaugeBegin();

// Puts them back into the framebuffer. Cheap: a blit of the pixels gaugeBegin
// worked out, not the shapes again.
void gaugeDraw(uint16_t *fb);
