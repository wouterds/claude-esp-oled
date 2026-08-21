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
