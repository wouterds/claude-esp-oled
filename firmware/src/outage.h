#pragma once

#include <stdint.h>

// Whether Claude itself is up, off the same public status page the web one
// shows. Its own task for the same reason the usage numbers have one: a TLS
// handshake takes longer than a frame.
void outageBegin();

// Ordered, worst last - improving is a fall, and that is what earns a sound.
enum class Outage : uint8_t { None, Partial, Major };

// The worst thing any component on the page is saying. None until the first
// read lands, because nothing to show and nothing wrong look the same here.
Outage outageLevel();
