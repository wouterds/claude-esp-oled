#pragma once

// Reads the BQ27220 gauge once and logs what it says. There is nothing to draw
// with it - it is here because a board that will not power on from PWR looks
// exactly the same whether the battery is flat, unplugged, or fine, and PWR
// itself is a power-path switch this chip cannot see.
void batteryReport();
