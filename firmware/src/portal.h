#pragma once

#include <stddef.h>
#include <stdint.h>

// Room for one token and its terminator, which is what portalToken copies into.
constexpr size_t PORTAL_TOKEN_MAX = 512;

// A page on port 80. A session token it takes is the whole of somebody's
// claude.ai login, so one is never in this repository and never on a wire out
// of the building - it is typed into the device over the local network and kept
// in the flash's key store, and the only place it is ever sent is back to the
// origin it came from. Several can be kept, one of them picked at a time.
void portalBegin();

// The picked token, copied out. False when nobody has set one. Copied rather
// than pointed at: the slots are rewritten in place when one is trashed, and a
// reader holding a pointer across a request is reading a buffer that moves under
// it - which spends the token on a request that then reads as a refused login.
bool portalToken(char *out, size_t size);
