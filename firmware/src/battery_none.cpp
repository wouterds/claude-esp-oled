#include "battery.h"

#include <Arduino.h>

// The AMOLED board keeps its pack behind an AXP2101 PMIC and carries no fuel
// gauge at all. Whether the part exists is a fact about the board rather than
// something to find out at runtime: probed instead, a gauge that has browned
// out with its own pack is indistinguishable from one that was never fitted,
// and those two want opposite answers - the first should be asked again when a
// pack turns up, the second never again.
//
// Reading the charge back through the PMIC is not written yet, so this says
// what is true in the meantime: nothing is known about the battery.

void batteryBegin() { Serial.println("battery: no gauge on this board"); }

BatteryState batteryRead() { return BatteryState{0, 0, false, false}; }
