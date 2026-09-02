#pragma once

#include <stdint.h>

// What claude.ai says the account has spent, read from the same endpoint the
// web app reads and with the same session cookie. All of it happens on the
// other core: a TLS handshake takes longer than a frame.
void usageBegin();

// The picked token has changed - a new one stored, or another of them picked.
// Whatever was worked out from the last one is dropped - the organisation
// before anything else, because that belongs to the account the token was for
// rather than to the device - and the next read happens now rather than at the
// next slot.
void usageTokenChanged();

// How long the poller waits between reads, in whole minutes. One to five, kept
// in the store, and applied from the next wait rather than to the one already
// under way.
uint8_t usageEvery();
bool usageSetEvery(uint8_t minutes);

// Whether the last read worked. Until it has, there is nothing real to show.
bool usageReady();

// How many reads in a row have come back saying exactly what the last one did.
// Nothing is happening, and something upstairs would like to know.
uint8_t usageStill();

// Percentages, as the web app shows them.
uint8_t usageSession();
uint8_t usageWeekly();

// How long each window has left before it rolls over, in milliseconds off the
// device's own clock. Nought when the last read did not say when - a window
// nobody has spent anything in has no reset to give.
uint32_t usageSessionResetsIn();
uint32_t usageWeeklyResetsIn();
