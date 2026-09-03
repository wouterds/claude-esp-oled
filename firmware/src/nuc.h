#pragma once

#include <stdint.h>

// What the box in the other room is doing, off the same endpoint its own
// dashboard reads - one flat object of a couple of hundred bytes, no key and
// nothing of ours going up with it.
//
// Read only while its page is up, for the same reason the market screens are: a
// reply that lands on a page nobody is looking at costs a TLS session, and the
// radio is the most expensive thing on this board.

// Readings kept. Two more than the page draws across the glass: it holds the
// trace a couple of readings behind the newest, so the ones it has not revealed
// yet still have to be somewhere.
constexpr uint8_t NUC_POINTS = 50;

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
  // How many readings have landed, this one included. In here rather than beside
  // it so the count and the readings it counts are the same copy: read
  // separately, a poller caught between the two hands out a buffer that has
  // moved on and a count that has not, and the trace steps back a whole reading
  // for a frame.
  uint32_t taken;
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
// moves it too - what is on the glass changes when one does, which is why
// anything scrolling the history along counts `taken` instead.
uint32_t nucRevision();

// The longest gap between readings worth timing. Past it the gap holds the wait
// after a failed read rather than a round trip.
uint32_t nucGapMost();

