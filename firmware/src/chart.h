#pragma once

#include <stdint.h>

// One market screen: a name, a price, how far it has moved and a line of where
// it has been. Seven screens share it, because the only thing that differs
// between a coin and a stock index is the words.
//
// It owns the glass from below the network and charge figures downward, and
// leaves those alone - they mean the same thing on every page and are drawn by
// whoever owns them.

// Forgets what it last put down, so the next step draws all of it. For arriving
// at a screen, where the panel has just been cleared and nothing on it can be
// compared against.
void chartForget();

// Draws whatever has changed. The figures move once an interval, so most frames
// have nothing to do; while a screen has never been read there is a pulse to
// keep moving and that is all it redraws.
void chartStep(uint16_t *fb, uint8_t screen);
