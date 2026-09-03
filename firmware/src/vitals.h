#pragma once

#include <stdint.h>

// The box in the other room, on one screen: what it is carrying, how warm it is
// getting, what it is pulling out of the wall and what has gone over the wire.
//
// It owns the glass from below the network and charge figures downward, and
// leaves those alone - they mean the same thing on every page and are drawn by
// whoever owns them.

// Forgets what it last put down, so the next step draws all of it. For arriving
// at the page, where the panel has just been cleared and nothing on it can be
// compared against.
void vitalsForget();

// Draws whatever has changed. The figures move once a poll, so most frames have
// nothing to do at all.
void vitalsStep(uint16_t *fb);
