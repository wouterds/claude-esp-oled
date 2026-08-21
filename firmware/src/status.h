#pragma once

#include <stdint.h>

// The chrome: what it is running on rather than what it is feeling. Draws and
// sends its own rows, because it owns two bands at opposite ends of the panel
// and a single range covering both would be the whole screen - which is the one
// thing the partial flush exists to avoid.
void statusDraw(uint16_t *fb);
