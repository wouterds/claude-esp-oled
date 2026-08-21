#pragma once

#include <stdint.h>

// What claude.ai says the account has spent, read from the same endpoint the
// web app reads and with the same session cookie. All of it happens on the
// other core: a TLS handshake takes longer than a frame.
void usageBegin();

// Try again now rather than at the next poll - for when a token has just been
// typed in and nobody wants to wait a minute to find out if it was the right
// one.
void usageWake();

// Whether the last read worked. Until it has, there is nothing real to show.
bool usageReady();

// How many reads in a row have come back saying exactly what the last one did.
// Nothing is happening, and something upstairs would like to know.
uint8_t usageStill();

// Percentages, as the web app shows them.
uint8_t usageSession();
uint8_t usageWeekly();
