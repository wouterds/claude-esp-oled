#include "net.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {

SemaphoreHandle_t held = nullptr;

NetCall *calls = nullptr;
uint16_t kept = 0;
uint16_t nextCall = 0;
uint32_t made = 0;
uint32_t heard = 0;
uint32_t heardAt = 0;

// Long enough that coming straight back finds what was already there, short
// enough that anything worth a second look is read again.
constexpr uint32_t STALE_MS = 30000;
volatile bool watched = true;
volatile bool stale[NET_POLLERS] = {false, false};
uint32_t awayAt = 0;

// Enough that a two kilobyte reply comes over in a handful of passes, small
// enough that it sits on the stack rather than in the heap this is protecting.
constexpr size_t CHUNK = 512;
// What the socket is given between passes. Long enough to be a block rather
// than a yield - which is the point - and short enough that it costs a reply
// nothing to be read this way.
constexpr uint32_t BREATH_MS = 5;

}  // namespace

void netBegin() {
  held = xSemaphoreCreateMutex();
  calls = (NetCall *)heap_caps_calloc(NET_CALLS, sizeof(NetCall), MALLOC_CAP_SPIRAM);
  // Not fatal. The log is a view onto what the board is doing, not part of
  // doing it, so without the memory it simply stays empty.
  Serial.printf("net: call log %s\n", calls ? "in psram" : "has no psram, staying empty");
}

void netWatching(bool on) {
  if (on == watched) {
    return;
  }
  watched = on;
  if (!on) {
    awayAt = millis();
    return;
  }
  if (millis() - awayAt < STALE_MS) {
    return;
  }
  for (uint8_t i = 0; i < NET_POLLERS; i++) {
    stale[i] = true;
  }
}

bool netWatched() { return watched; }

bool netStale(uint8_t poller) {
  if (poller >= NET_POLLERS || !stale[poller]) {
    return false;
  }
  stale[poller] = false;
  return true;
}

uint32_t netDueIn(uint32_t period, uint32_t phase) {
  if (!period) {
    return 250;
  }
  // The period is added before the phase is taken off it, so a phase ahead of
  // the clock early in a boot subtracts rather than wrapping to seven weeks.
  uint32_t since = (millis() + period - phase % period) % period;
  return period - since;
}

void netHeard(uint32_t unix) {
  heard = unix;
  heardAt = millis();
}

void netRecord(const char *url, uint32_t began, int code, uint32_t size) {
  if (!calls) {
    return;
  }
  NetCall &c = calls[nextCall];
  strncpy(c.url, url, sizeof(c.url) - 1);
  c.url[sizeof(c.url) - 1] = '\0';
  // Device time. The clock arrives on a reply, which is later than the first
  // calls out, so what a call gets stamped with is worked out on the way out
  // rather than here - by then there may be a clock that there was not yet.
  c.at = millis();
  c.ms = (uint16_t)(millis() - began);
  c.code = (int16_t)code;
  c.size = size;
  // Last, so a reader that catches this mid-write sees the slot before it
  // rather than half of the one being filled.
  nextCall = (uint16_t)((nextCall + 1) % NET_CALLS);
  if (kept < NET_CALLS) {
    kept++;
  }
  made++;
}

uint16_t netCallCount() { return calls ? kept : 0; }

uint32_t netCallsMade() { return made; }

bool netCallAt(uint16_t i, NetCall *out) {
  if (!calls || i >= kept) {
    return false;
  }
  *out = calls[(nextCall + NET_CALLS - 1 - i) % NET_CALLS];
  out->url[sizeof(out->url) - 1] = '\0';
  // Signed, and either way round: a call made before the clock landed is a
  // negative distance from it, which is exactly the time it happened at.
  int32_t since = (int32_t)(out->at - heardAt);
  out->at = heard ? (uint32_t)((int32_t)heard + since / 1000) : 0;
  return true;
}

void netTake() {
  if (held) {
    xSemaphoreTake(held, portMAX_DELAY);
  }
}

void netGive() {
  if (held) {
    xSemaphoreGive(held);
  }
}

bool netBody(HTTPClient &http, String &out, size_t cap, uint32_t patience) {
  int said = http.getSize();
  if (said > 0 && (size_t)said > cap) {
    Serial.printf("net: the reply says %d bytes, which is past the %u this holds\n", said,
                  (unsigned)cap);
    return false;
  }
  // NetworkClient on this core and WiFiClient on the last one, and the name is
  // not worth pinning down here.
  auto *stream = http.getStreamPtr();
  if (!stream) {
    return false;
  }

  out = "";
  // Asked for up front when the length is known, so the string is not grown and
  // copied a dozen times through the memory this is trying to leave alone.
  if (said > 0) {
    out.reserve((size_t)said);
  }

  char chunk[CHUNK];
  // Since anything last arrived, rather than since the read began: a reply that
  // is still coming is not one that has stalled, however long it takes.
  uint32_t moved = millis();
  while (said != 0) {
    int ready = stream->available();
    if (ready <= 0) {
      // Nothing waiting, so the two reasons to stop are asked about before
      // anything is spent waiting on more.
      if (!http.connected() || millis() - moved > patience) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(BREATH_MS));
      continue;
    }

    size_t take = (size_t)ready > CHUNK ? CHUNK : (size_t)ready;
    if (said > 0 && take > (size_t)said) {
      take = (size_t)said;
    }
    if (out.length() + take > cap) {
      Serial.printf("net: the reply ran past the %u this holds\n", (unsigned)cap);
      return false;
    }
    int got = stream->readBytes(chunk, take);
    if (got <= 0) {
      break;
    }
    if (!out.concat(chunk, (unsigned int)got)) {
      Serial.println("net: no room for the reply");
      return false;
    }
    moved = millis();
    if (said > 0) {
      said -= got;
    }
  }

  // A known length that did not all arrive is a truncated reply, and a truncated
  // reply parsed for a number is worse than no reply at all.
  if (said > 0) {
    Serial.printf("net: %d bytes of the reply never came\n", said);
    return false;
  }
  return out.length() > 0;
}
