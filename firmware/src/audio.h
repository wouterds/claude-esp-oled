#pragma once

#include <stdint.h>

// The ES8311 on the far end of the same I2C bus everything else is on, driven a
// note at a time. None of this is music: they are a handful of short sounds,
// and they are here so the board can say something happened to somebody who is
// not looking at it. Nothing announces the board itself - it is on a desk, and a
// thing on a desk that sings when it is switched on gets switched off.
//
// Asking for one returns immediately. Rendering a note takes longer than a
// frame does, so it happens on the other core and the last one asked for wins -
// nothing here is worth queueing.
void audioBegin();

// 0-100, taken on the next note. Nought is silence with the amplifier still
// switched for it, which is nothing anybody can hear.
void audioVolume(uint8_t percent);

// The charger has just landed.
void audioPlugged();

// The volume has just been set, and this is what it sounds like.
void audioSampled();

// A window has rolled over and there is room again.
void audioCheered();

// An outage has just appeared where the last look found none.
void audioErrored();

// The face has just spent the lot.
void audioDied();
