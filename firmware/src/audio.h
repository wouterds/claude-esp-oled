#pragma once

// The ES8311 on the far end of the same I2C bus everything else is on, driven a
// note at a time. None of this is music: they are two short sounds, and they
// are here so the board can say something happened to somebody who is not
// looking at it. Nothing announces the board itself - it is on a desk, and a
// thing on a desk that sings when it is switched on gets switched off.
//
// Asking for one returns immediately. Rendering a note takes longer than a
// frame does, so it happens on the other core and the last one asked for wins -
// nothing here is worth queueing.
void audioBegin();

// The charger has just landed.
void audioPlugged();

// A window has rolled over and there is room again.
void audioCheered();

// An outage has just appeared where the last look found none.
void audioErrored();

// The face has given up. Nothing raises it on its own yet - a double press on
// the button is what asks for it, so that there is a way to hear it at all.
void audioDied();
