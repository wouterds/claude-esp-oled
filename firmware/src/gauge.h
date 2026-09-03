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

// How far the numbers are onto the glass: nought with them off, one with them
// up and the bars pulled in behind them. Anything else that wants the bottom of
// the glass rides this rather than the tap, so the two are one handover rather
// than a swap with a delay in it.
//
// Not the same as how short the bars are, though they move together: the bars
// are also short before there is anything to show them, and something riding
// that comes up the instant the first reading lands and fades out again as they
// grow.
float gaugeFiguresLevel();

// Rolls one of them onto a new value when its turn comes round, and puts that
// bar on the panel itself.
void gaugeStep(uint16_t *fb, uint32_t now);

// The colour a reading of this size wears, off the one ramp the whole device
// shares: the teal it starts at, through yellow and amber, to the brand's pink
// at the top. Anything that draws a percentage as a length - the dials, the
// bars, the charge - takes its colour from here, or the same number means one
// thing on the face and another beside it.
//
// Not the battery glyph in the status bar. That one is white until the charge is
// low enough to be worth saying something about and then goes yellow, amber and
// red: a warning rather than a reading, which is a different question from how
// full the cell is.
uint16_t gaugeColour(uint8_t percent);

// Puts them back into the framebuffer, over the box about to be sent and no
// wider - the box arrives the way boardFlushRect takes it. A blit of the pixels
// gaugeBegin worked out rather than the shapes again, but there are twenty
// thousand of them and the face's box is a fifth of the panel across: outside
// it nothing was cleared and nothing is going out, so a pixel written there is
// a write into PSRAM that no eye ever sees.
void gaugeDraw(uint16_t *fb, int16_t x0, int16_t x1, int16_t from, int16_t to);
