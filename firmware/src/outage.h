#pragma once

#include <stdint.h>

// Whether Claude itself is up, off the same public status page the web one
// shows. Its own task for the same reason the usage numbers have one: a TLS
// handshake takes longer than a frame.
void outageBegin();

// Ordered, worst last - improving is a fall, and that is what earns a sound.
// Unknown sorts under all of them, so the first read of the day is a rise
// however good its news is and cannot ring the bell on the way in.
enum class Outage : uint8_t { Unknown, None, Partial, Major };

// The worst thing any component on the page is saying, and Unknown until a read
// has actually landed - having nothing to show is not the same as being well.
Outage outageLevel();
