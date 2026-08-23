#pragma once

#include <stdint.h>

// A shelf of them, one at a time, as the bitmaps they were drawn as. The ball
// is the first, and after it whatever has been traced since.
//
// Nothing here moves, so it is drawn when the one being shown changes and left
// alone otherwise.
void pokemonStep(uint16_t *fb);

// Back to the ball, and all of it drawn again. For arriving at the page.
void pokemonOpen();

// One on, or one back, wrapping at both ends.
void pokemonTurn(bool onward);
