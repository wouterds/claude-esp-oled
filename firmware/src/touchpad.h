#pragma once

#include <stdint.h>

// The two things that differ between the boards' touch silicon: letting the
// controller out of reset, and getting one report out of it. What a swipe is
// gets decided in touch.cpp from these and knows about neither controller.

// Lets the controller go and says whether it answered at all.
bool touchpadBegin();

// One report. `along` is how far down the glass the finger is and `across` how
// far to the right, in the same direction and the same units the scene is
// drawn in, so that a finger moving towards the bottom of the picture always
// makes the first larger.
//
// False means nothing was read: no finger, or a controller that has gone to
// sleep and stopped acknowledging its own address.
bool touchpadRead(bool *down, int16_t *along, int16_t *across);
