#pragma once

#include <FS.h>
#include <stddef.h>
#include <stdint.h>

// What is on the glass, kept as a PNG on the flash.
//
// Nothing is read back off the panel to do it. The framebuffer already holds
// the whole picture - every scene draws into it and the flush only ever reads
// it - so a screenshot is that buffer encoded, and the controller is never
// asked a question it answers slowly and only half of.
//
// PNG rather than JPEG, and not out of habit: this glass carries flat colour,
// hairline arcs and small text on black, which is what a DCT rings haloes
// around and what deflate is best at. The lossless one is both the honest file
// and the smaller one here.

// How many are kept. Taking the eleventh is what removes the first.
constexpr uint8_t SHOTS_MAX = 10;
// "shot-000000.png" and the nul after it.
constexpr size_t SHOT_NAME_MAX = 16;

// Mounts the flash and reads what is already on it. Everything below answers as
// though there are none when it could not, so a board with no filesystem loses
// the screenshots and nothing else.
void shotBegin();

// Encodes the framebuffer and writes it, the oldest making way at the cap.
// Seconds rather than milliseconds - deflate over a third of a megabyte - and
// the caller is stopped for all of them, so this belongs where a dropped frame
// is the whole of the cost. False is nothing written.
bool shotTake();

// How many are on the flash.
uint8_t shotCount();

// The nth newest, into a buffer of SHOT_NAME_MAX. False past the last one.
bool shotAt(uint8_t at, char *name, uint32_t *bytes);

// Removes one by name. False where it has no such thing - which is also what a
// name that never was one gets. The kept list is the only way in here, so there
// is no path in this for a request to walk out of the directory on.
bool shotTrash(const char *name);

// Opens one by name, for reading. A file that did not open is a name it does
// not have, for the same reason.
File shotOpen(const char *name);

// The list, and the files under it, move on the core the board is drawn on
// while the page that asks for them is served on the other. Every look at
// either goes between these, reads included: a name read while it is being
// written is not a name, and a file unlinked out from under an open handle is
// worse than that. Held again from inside is fine - it is the one lock, and it
// counts.
void shotHold();
void shotDrop();
