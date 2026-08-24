#pragma once

// The BOOT button, read as a button rather than left to the ROM.
//
// It is the strapping pin: held low as the board comes up, the ROM stops in the
// bootloader and the sketch never runs. That is a reason to be careful about
// when it is pressed, not a reason the running sketch cannot read it - nothing
// about the strap applies once the board is up.
void buttonBegin();

// The button went down and came up. Cleared by the asking, so one press cannot
// be acted on twice.
bool buttonPressed();
