#pragma once

#include <stdint.h>

// A page on port 80 with one field on it. The session token it takes is the
// whole of somebody's claude.ai login, so it is never in this repository and
// never on a wire out of the building - it is typed into the device over the
// local network and kept in the flash's key store, and the only place it is
// ever sent is back to the origin it came from.
void portalBegin();

// The stored token, or nullptr when nobody has set one.
const char *portalToken();
