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

// The last few calls out to claude.ai, newest first. A debug view rather than a
// record: it is in RAM and goes with the power. Only the path is kept, because
// every one of them goes to the same host and whatever shows this can say that
// host itself rather than being told it thirty times over.
struct UsageCall {
  uint32_t at;    // UTC seconds, or nought before the endpoint said what time it was
  uint32_t size;  // bytes of body read
  uint16_t ms;
  int16_t code;   // an HTTP status, negative for a client-side failure, nought for no connection
  char path[48];
};
constexpr uint8_t USAGE_CALLS = 30;

// Copies out up to max of them, newest first, and returns how many there were.
// Copied rather than pointed at: the poller writes these from its own task, and
// a row read while it is being written over is a wrong row rather than a crash.
uint8_t usageCalls(UsageCall *out, uint8_t max);

// How long each window has left before it rolls over, in milliseconds off the
// device's own clock. Nought when the last read did not say when - a window
// nobody has spent anything in has no reset to give.
uint32_t usageSessionResetsIn();
uint32_t usageWeeklyResetsIn();
