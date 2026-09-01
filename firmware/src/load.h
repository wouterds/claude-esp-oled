#pragma once

#include <stdint.h>

// What the board is spending on itself. Two numbers the details page puts on
// the glass either side of the cell: how much of the drawing core is not idling,
// and how much of the internal heap has gone.
//
// The drawing core and not both of them, because the counter this comes off
// answers for the core that asks it and because core nought has nothing steady
// to do - it holds the two pollers and they are asleep between the minute they
// wait out. Averaging a busy core with a sleeping one would halve every reading
// and move the gauge further from what it is being looked at to find out.
//
// Internal heap and not PSRAM, which is the whole point of the second one. The
// eight megabytes of PSRAM sit all but untouched - a gauge on them would read
// five percent for ever and say nothing - while the internal RAM is what the
// panel's band buffers, the radio and a TLS handshake all come out of, and it
// is the one that runs out. When it does, an allocation inside mbedTLS fails
// and the read that failed reads as the network being down.
void loadBegin();

// Both are differences measured over an interval rather than readings taken at
// an instant, so this goes in the frame loop and keeps its own clock - it costs
// a comparison on the frames between samples.
//
// In the loop rather than on the page that shows it, or the only load it would
// ever measure is the load of showing it: the details page draws almost nothing
// and the face draws the whole time, so a figure sampled only while the face is
// off the glass is a figure about the wrong thing.
void loadStep();

// Nought to a hundred, both of them.
uint8_t loadCpu();
uint8_t loadRam();
