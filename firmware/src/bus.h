#pragma once

// The one I2C bus, and the lock on it. The touch controller, the battery gauge
// and the audio codec all hang off the same two pins, and the touch controller
// is read from its own core - so a transaction can no longer assume it has the
// bus to itself.
//
// The lock is not about the wire, which arbitrates for itself. It is about the
// Wire object: it holds one buffer for the whole bus, so two callers part way
// through a transaction each is one transaction with both their bytes in it.
void busBegin();

// Every transaction goes between these two, reads included - a register read is
// a write of the address and a read that follows it, and letting go in the
// middle of that is letting go in the middle of a transaction.
void busTake();
void busGive();
