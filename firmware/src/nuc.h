#pragma once

#include <stdint.h>

// What the box in the other room is doing, off the same endpoint its own
// dashboard reads - one flat object of a couple of hundred bytes, no key and
// nothing of ours going up with it.
//
// Read only while its page is up, for the same reason the market screens are: a
// reply that lands on a page nobody is looking at costs a TLS session, and the
// radio is the most expensive thing on this board.

// Columns the load history is drawn as, and the whole of the history there is.
// Nothing is asked for off the page, so this is however long you have been
// watching rather than a window of wall clock. Forty-eight of them at ten
// seconds apiece is eight minutes, and forty-eight is as many as will fit across
// the glass and still be a column rather than a hair.
constexpr uint8_t NUC_POINTS = 48;

struct Nuc {
  float cpu;
  float memory;
  float disk;
  // Sensors the box may not have, and a meter that may not answer. NAN rather
  // than nought, which on every one of these lines is a plausible reading.
  float coreTemp;
  float nvmeTemp;
  float power;
  // Nothing but the highest the meter has actually reported, which is what the
  // bar beside the draw is a fraction of. It only ever grows.
  float powerPeak;
  float load[NUC_POINTS];
  uint8_t count;
  // Read at least once. Until then there is nothing to show but the shape of
  // what is coming.
  bool ready;
  bool failed;
};

void nucBegin();

// Whether the page it is drawn on is the one in front of somebody.
void nucWatching(bool on);

// A copy of what was last read. Copied rather than pointed at because the poller
// writes this from its own task, and a reading caught half-written is a wrong
// number rather than a wrong pixel.
void nucLatest(Nuc *out);

// Moves whenever any of it does, so the page can tell it has something new to
// draw without comparing three hundred bytes to find out. A read that failed
// moves it too - what is on the glass changes when one does.
uint32_t nucRevision();

// How many readings have actually been taken. Not the same as the revision: a
// read that failed moves that and adds nothing to the history, and anything
// scrolling the history along has to know the difference or it slides the trace
// on for a reading that never arrived.
uint32_t nucReadings();
