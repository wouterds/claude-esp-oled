#pragma once

#include <stdint.h>

// The chrome: what it is running on rather than what it is feeling. Draws and
// sends its own rows, because it owns two bands at opposite ends of the panel
// and a single range spanning both would be the whole screen - which is the one
// thing the partial flush exists to avoid.
//
// The rows the face just painted are passed in. The face clears its own box
// before it draws, so anything of ours inside that box has been wiped and has
// to go back, whatever it says.
void statusDraw(uint16_t *fb, int16_t faceFrom, int16_t faceTo);

// Just the two figures at the top - the network and the charge. Every page
// carries those and they mean the same thing on all of them; the rest of what
// this file draws belongs to the face alone.
//
// `wipe` for a page that has only just been cleared. There is nothing on the
// glass to compare against then, so what was last drawn is not what is there.
void statusBars(uint16_t *fb, bool wipe);
