#pragma once

#include <stdint.h>

// The third page: the two things about this board that are somebody's taste
// rather than a reading. A slider each, set by a finger and kept in the flash's
// key store, so they are what they were the last time somebody set them.

// Reads them back out of the store. Before the panel, because the panel comes
// up at whatever this says rather than at full and then jumping.
void settingsBegin();

uint8_t settingsBrightness();
uint8_t settingsVolume();

// Follows the finger, redraws whatever it moved, and applies it as it goes.
void settingsStep(uint16_t *fb);

// Writes a change to the store once the finger has been off it for a moment.
// Every frame, on every page: what was set just before the page was left still
// has to land.
void settingsKeep();

// A finger is on one of the sliders. A drag along a track wanders up and down
// as well, and a wander that far is otherwise a swipe.
bool settingsHolding();

// Forgets what it last put down, so the next step draws all of it.
void settingsForget();
