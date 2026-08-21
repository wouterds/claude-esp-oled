#pragma once

#include <stdint.h>

// The two windows stood up as bars: how much of each is gone, and how long is
// left of it. The same two numbers the arcs on the face's page carry, read
// straight rather than out of the corner of an eye.
//
// Drawn only when something it says has changed, and drawn whole when it is -
// which here is once a second, because a countdown is what moves. That is also
// why the countdowns stop at seconds: hundredths would mean sending the whole
// panel every frame for a digit nobody can read.
void allowanceStep(uint16_t *fb);

// Forgets what it last put down, so the next step draws all of it. For arriving
// at the page, when nothing has changed and all of it still has to go back on
// the glass.
void allowanceForget();
