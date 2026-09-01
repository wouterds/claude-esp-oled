#include "load.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

namespace {

// Long enough that the figure means something and short enough that the gauge
// still looks alive. Under about a quarter of a second the idle counter has too
// little in it to divide and the needle jitters on rounding alone.
constexpr uint32_t EVERY_MS = 500;

uint32_t lastAt = 0;
uint32_t lastIdle = 0;
uint8_t cpu = 0;
uint8_t ram = 0;

// The run time counter is the esp_timer, so it is microseconds and it is the
// same clock the interval below is measured on - which is what makes the two
// divisible by each other. Kept at the width it is returned in: a microsecond
// counter goes round every seventy minutes, and a difference taken in the same
// width comes out right across the turn where a widened one does not.
uint32_t idleSoFar() { return (uint32_t)ulTaskGetIdleRunTimeCounter(); }

uint8_t percentOf(uint64_t part, uint64_t whole) {
  if (!whole) {
    return 0;
  }
  if (part > whole) {
    part = whole;
  }
  return (uint8_t)((part * 100 + whole / 2) / whole);
}

}  // namespace

void loadBegin() {
  lastAt = millis();
  lastIdle = idleSoFar();
}

void loadStep() {
  uint32_t now = millis();
  uint32_t since = now - lastAt;
  if (since < EVERY_MS) {
    return;
  }
  uint32_t idle = idleSoFar();

  // One core's worth, because one core is what was asked. The counter answers
  // for the idle task of whichever core calls it, and this is called from the
  // frame loop, so what comes back is core one - the core the panel is drawn on
  // and the only one with steady work to do. Core nought holds the pollers, and
  // they are asleep between the minute they wait out.
  //
  // It does not sit pinned at a hundred, either, which is what makes it worth a
  // gauge: the flush blocks on its own DMA for most of its six milliseconds and
  // the idle task has that time.
  uint64_t couldIdle = (uint64_t)since * 1000ULL;
  uint32_t idled = idle - lastIdle;
  cpu = (uint8_t)(100 - percentOf((uint64_t)idled, couldIdle));

  size_t whole = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  size_t left = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  ram = percentOf((uint64_t)(whole - left), (uint64_t)whole);

  lastAt = now;
  lastIdle = idle;
}

uint8_t loadCpu() { return cpu; }

uint8_t loadRam() { return ram; }
