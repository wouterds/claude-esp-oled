#pragma once

#include <stdint.h>

// The BOOT button, read as a button rather than left to the ROM.
//
// It is the strapping pin: held low as the board comes up, the ROM stops in the
// bootloader and the sketch never runs. That is a reason to be careful about
// when it is pressed, not a reason the running sketch cannot read it - nothing
// about the strap applies once the board is up.
void buttonBegin();

// The button went down and came up. Cleared by the asking, so one press cannot
// be acted on twice. A press that has already gone out as a hold is not one of
// these: it said what it was while it was still down, and reporting the release
// as well would have one press mean two things.
bool buttonPressed();

// The button is still down and has been for this long. Said once per press -
// the hold has happened by the time it is answered, and going on holding does
// not make it happen again.
//
// Asked before the release rather than measured across it, so what acts on it
// can answer while the finger is still there. That matters for anything slow
// enough to be worth acknowledging.
bool buttonHeld(uint32_t ms);
