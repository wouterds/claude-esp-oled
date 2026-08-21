#pragma once

#include <stdint.h>

// Two arcs hugging the left and right of the glass, each a track with a filled
// part. The values are picked once and do not move, so the pixels are worked
// out once too and put back every frame - the face wanders far enough sideways
// to wipe them, and its own flush is the only cheap chance to repair them.
void gaugeBegin();

// Brings them onto the glass. They stay off it until the intro is done, then
// fade up empty before anything sends them anywhere.
void gaugeReveal();

// Shows or hides the two numbers, which are off until somebody asks.
void gaugeFigures();

// Whether they are up, for whoever else has something to say only while the
// question they answer is being asked.
bool gaugeFiguresShown();

// Rolls one of them onto a new value when its turn comes round, and puts that
// bar on the panel itself.
void gaugeStep(uint16_t *fb, uint32_t now);

// Puts them back into the framebuffer. Cheap: a blit of the pixels gaugeBegin
// worked out, not the shapes again.
void gaugeDraw(uint16_t *fb);
