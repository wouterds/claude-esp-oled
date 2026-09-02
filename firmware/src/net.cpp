#include "net.h"

#include <Arduino.h>
#include <string.h>

namespace {

SemaphoreHandle_t held = nullptr;

NetCall calls[NET_CALLS] = {};
uint8_t kept = 0;
uint8_t nextCall = 0;
uint32_t heard = 0;
uint32_t heardAt = 0;

// Enough that a two kilobyte reply comes over in a handful of passes, small
// enough that it sits on the stack rather than in the heap this is protecting.
constexpr size_t CHUNK = 512;
// What the socket is given between passes. Long enough to be a block rather
// than a yield - which is the point - and short enough that it costs a reply
// nothing to be read this way.
constexpr uint32_t BREATH_MS = 5;

}  // namespace

void netBegin() { held = xSemaphoreCreateMutex(); }

void netHeard(uint32_t unix) {
  heard = unix;
  heardAt = millis();
}

void netRecord(const char *url, uint32_t began, int code, uint32_t size) {
  NetCall &c = calls[nextCall];
  strncpy(c.url, url, sizeof(c.url) - 1);
  c.url[sizeof(c.url) - 1] = '\0';
  c.at = heard ? heard + (millis() - heardAt) / 1000 : 0;
  c.ms = (uint16_t)(millis() - began);
  c.code = (int16_t)code;
  c.size = size;
  // Last, so a reader that catches this mid-write sees the slot before it
  // rather than half of the one being filled.
  nextCall = (uint8_t)((nextCall + 1) % NET_CALLS);
  if (kept < NET_CALLS) {
    kept++;
  }
}

uint8_t netCalls(NetCall *out, uint8_t max) {
  uint8_t n = kept < max ? kept : max;
  for (uint8_t i = 0; i < n; i++) {
    out[i] = calls[(nextCall + NET_CALLS - 1 - i) % NET_CALLS];
    out[i].url[sizeof(out[i].url) - 1] = '\0';
  }
  return n;
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
