#pragma once

// The ES8311 on the far end of the same I2C bus everything else is on, driven a
// note at a time. None of this is music: they are three short sounds, and they
// are here so the board can say something happened to somebody who is not
// looking at it.
//
// Asking for one returns immediately. Rendering a note takes longer than a
// frame does, so it happens on the other core and the last one asked for wins -
// nothing here is worth queueing.
void audioBegin();

// On the way up.
void audioHello();

// The charger has just landed.
void audioPlugged();

// A window has rolled over and there is room again.
void audioCheered();
