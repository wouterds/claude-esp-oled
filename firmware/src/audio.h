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

// The glass has just been written to a file.
void audioShuttered();

// Waits for whatever was last asked for to have been played, and to have left
// the part rather than merely the queue. Gives up after a moment, so an audio
// task that has gone quiet cannot take the panel down with it.
//
// Nothing else wants this - a sound is asked for and forgotten. It is here for
// a caller about to take the board away from itself for a second: the sounds
// are rendered on the core the radio is on, and a screenshot walks over PSRAM
// hard enough that the flash those notes are fetched through cannot keep up.
// The acknowledgement has to be out of the speaker before the thing it is
// acknowledging starts, not queued behind it.
void audioWait();

// The forever song, round and round from the top until it is asked to stop -
// which it does at the end of the note it is on.
void audioSong(bool on);
