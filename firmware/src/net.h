#pragma once

#include <HTTPClient.h>
#include <stddef.h>
#include <stdint.h>

// The lock on going out over the network, and the one safe way to read what
// comes back. Both pollers wake on the same edge - the radio joining - so
// without either of these they do the same expensive thing in the same instant.
void netBegin();

// Around a whole request, handshake included. A TLS session is around 48KB of
// internal RAM while it is up, and internal RAM is the scarce memory here: two
// sessions at once took the free heap from 118KB to under four, and the board
// has been measured finishing that window with 200 bytes to its name. An
// allocation that comes back empty inside mbedTLS is an abort rather than a
// return, so what that costs is not a failed read but a reboot.
//
// The handshake is where the memory goes and the socket is what holds it, so
// letting go before the session closes is letting go before it is given back.
void netTake();
void netGive();

// The body of a reply, read without spinning on it.
//
// HTTPClient::getString() waits for a slow reply by polling the socket and
// handing the core straight back to itself. A task at priority one that yields
// rather than blocks never lets its core's idle task run at all - and the idle
// task is what the task watchdog is watching. So a reply that arrives slowly is
// not a slow read: it is the watchdog aborting the board five seconds later,
// with `outage` or `usage` named as the task that was running and nothing else
// to say why. It reads as a random reset.
//
// It is a race the read cannot win, either. The HTTP timeout here is twelve
// seconds and the watchdog's is five, so anything HTTPClient is prepared to wait
// out reboots the board first.
//
// Waiting a tick at a time blocks instead, which is the whole of the fix. The
// cap is the other half: the body lands in internal RAM, and neither reply this
// asks for is more than a couple of kilobytes, so one that says it is larger is
// refused rather than allowed to take the heap down with it.
bool netBody(HTTPClient &http, String &out, size_t cap, uint32_t patience);

// The last few calls out, whoever made them - the usage read and the status
// read both come through here, so this is the one place that sees all of them.
// A debug view rather than a record: it is in RAM and goes with the power.
//
// The ring itself lives in PSRAM. Internal RAM is what a TLS handshake needs
// forty-eight kilobytes of and what the board has least of, and nothing in here
// is touched by DMA - so the slower memory costs this nothing and leaves the
// scarce memory to the thing that cannot do without it.
struct NetCall {
  uint32_t at;    // UTC seconds, or nought while nothing has said what the time is
  uint32_t size;  // bytes of body read
  uint16_t ms;
  int16_t code;   // an HTTP status, negative for a client failure, nought for no connection
  char url[80];
};
// Sixteen bits deliberately: a thousand does not fit in the eight this was, and
// silently wrapping to 232 would have looked like a log that simply forgot.
constexpr uint16_t NET_CALLS = 1000;

// Whether the page the readings are drawn on is the one in front of somebody.
// Nothing is asked for while it is not: a reply that lands on a page nobody is
// looking at costs a TLS session and a slice of the account's rate to be thrown
// away, and the radio is the most expensive thing on this board.
void netWatching(bool on);
bool netWatched();

// True once for each poller when that page comes back after being away for
// longer than a reading stays worth showing - so what is drawn on arrival is
// read fresh rather than being whatever was left when it went. Cleared by the
// asking, and asked for separately by each poller so neither takes the other's.
constexpr uint8_t NET_USAGE = 0;
constexpr uint8_t NET_OUTAGE = 1;
constexpr uint8_t NET_POLLERS = 2;
bool netStale(uint8_t poller);

// How long until the next slot on a shared cadence, in milliseconds. Both
// pollers hang off this so they keep a fixed offset from each other rather than
// drifting together: the usage read takes the slot on the period and the status
// read takes the one half a period after it, and neither ends up asking at the
// same moment as the other.
//
// Off the device clock rather than off when the last read finished, so a read
// that takes ten seconds does not push the next one ten seconds late. The clock
// wraps every seven weeks and one cycle either side of that is mistimed.
uint32_t netDueIn(uint32_t period, uint32_t phase);

// The device has no clock of its own. Whichever poller reads a Date header off
// a reply says so here, and the log below timestamps itself from that.
void netHeard(uint32_t unix);

// One call, timed from `began`. Called from inside the lock above, which is
// what keeps the two pollers from writing over each other.
void netRecord(const char *url, uint32_t began, int code, uint32_t size);

// How many are worth reading, newest first.
uint16_t netCallCount();

// How many have ever been recorded. Only changes when the board actually calls
// something, so it is what says whether the log is worth fetching again - a
// thousand rows is a hundred kilobytes and pulling that every few seconds to
// find it unchanged is most of what the board would be doing.
uint32_t netCallsMade();

// The nth newest, copied out; false past the end. One at a time rather than the
// lot: a thousand of them is ninety kilobytes and no caller should be putting
// that on a task stack. Copied rather than pointed at, so a row read while it is
// being written over is a wrong row rather than a crash.
bool netCallAt(uint16_t i, NetCall *out);
