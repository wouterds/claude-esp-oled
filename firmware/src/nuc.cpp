#include "nuc.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

#include "net.h"
#include "wifi.h"

namespace {

constexpr char URL[] = "https://nuc.wouterds.com/api";

// No interval at all: the next read starts as soon as the last one lands, so the
// page moves at whatever the round trip costs rather than at a cadence somebody
// picked. About a second, nearly all of it the handshake.
//
// What makes that affordable is that it only happens while the page is up, and
// while it is up this is the only thing on the radio - the readings on the face
// and the market screens have all stopped by then.
//
// A read that failed waits, though. A box that is off answers nothing in the
// same second, over and over, and the point of the pause is to stop asking it
// at the speed of a failing connection.
constexpr uint32_t RETRY_MS = 3000;
// What a gap between readings can be worth timing. Anything past this contains
// the wait above rather than a round trip, and averaging it in drives whatever
// is paced off it to a crawl.
constexpr uint32_t GAP_MOST_MS = RETRY_MS - 100;
// The reply is a couple of hundred bytes and has been for as long as it has been
// asked. The cap is not a size it approaches - it is there so a reply that is
// not what this asked for cannot take the heap with it.
constexpr size_t MOST = 4096;
// Between bytes, not for the whole read: a reply still arriving has not stalled
// however long it takes, and one that has stopped is not coming.
constexpr uint32_t PATIENCE_MS = 6000;

Nuc held = {};
// When the last read failed, or nought while they are landing. The only clock
// this poller keeps - there is no interval between good reads to time.
uint32_t failedAt = 0;
volatile bool watching = false;
volatile uint32_t revision = 0;
SemaphoreHandle_t lock = nullptr;

void take() { xSemaphoreTake(lock, portMAX_DELAY); }
void give() { xSemaphoreGive(lock); }

bool fetch(String &out) {
  uint32_t began = millis();
  WiFiClientSecure tls;
  netSecure(tls);

  HTTPClient http;
  http.setTimeout(12000);
  if (!http.begin(tls, URL)) {
    Serial.println("nuc: begin failed");
    netRecord(URL, began, 0, 0);
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("nuc: %d from the box\n", code);
    netRecord(URL, began, code, 0);
    http.end();
    return false;
  }
  bool read = netBody(http, out, MOST, PATIENCE_MS);
  http.end();
  netRecord(URL, began, code, out.length());
  if (!read) {
    Serial.printf("nuc: 200 but the body stopped at %u bytes\n", (unsigned)out.length());
  }
  return read;
}

// One flat object with no key appearing twice, so a reading is found by looking
// for its name and nothing has to be sliced off first.
float numberFor(const String &body, const char *key) {
  int at = body.indexOf(key);
  if (at < 0) {
    return NAN;
  }
  at += (int)strlen(key);
  // A sensor the box has not got is null rather than absent, and toFloat turns
  // that into nought - which for a temperature is a reading.
  if (at >= (int)body.length() || body.charAt(at) == 'n') {
    return NAN;
  }
  int end = at + 24;
  return body.substring(at, end > (int)body.length() ? (int)body.length() : end).toFloat();
}

bool read() {
  String body;
  if (!fetch(body)) {
    return false;
  }

  Nuc m = {};
  m.cpu = numberFor(body, "\"cpu\":");
  // The one reading this page is built around. Without it the reply was
  // something other than what was asked for, and the rest of it is not worth
  // reading over what is already on the glass.
  if (isnan(m.cpu)) {
    Serial.println("nuc: the reply had no load in it");
    return false;
  }
  m.memory = numberFor(body, "\"memory\":");
  m.disk = numberFor(body, "\"disk\":");
  m.coreTemp = numberFor(body, "\"cpu_temp\":");
  m.nvmeTemp = numberFor(body, "\"nvme_temp\":");
  m.power = numberFor(body, "\"power\":");
  m.powerPeak = numberFor(body, "\"power_peak\":");
  m.ready = true;

  take();
  memcpy(m.load, held.load, sizeof(m.load));
  m.count = held.count;
  m.taken = held.taken + 1;
  give();
  if (m.count < NUC_POINTS) {
    m.load[m.count++] = m.cpu;
  } else {
    memmove(m.load, m.load + 1, sizeof(float) * (NUC_POINTS - 1));
    m.load[NUC_POINTS - 1] = m.cpu;
  }

  take();
  held = m;
  give();
  return true;
}

void task(void *) {
  for (;;) {
    bool waited = !failedAt || millis() - failedAt >= RETRY_MS;
    if (watching && wifiConnected() && waited) {
      if (!netRoom("nuc")) {
        failedAt = millis();
      } else {
        netTake();
        bool got = read();
        netGive();
        failedAt = got ? 0 : millis();
        take();
        held.failed = !got;
        give();
        revision = revision + 1;
      }
    }
    // A tick, not a wait. Straight back round is a task at priority nought that
    // never blocks, and the idle task it starves is the one the watchdog watches.
    vTaskDelay(1);
  }
}

}  // namespace

void nucBegin() {
  lock = xSemaphoreCreateMutex();
  // Nothing read yet, and nought is a reading on every one of these lines - so
  // the page comes up saying it has nothing rather than saying the box is cold,
  // idle and switched off.
  held.cpu = held.memory = held.disk = NAN;
  held.coreTemp = held.nvmeTemp = held.power = held.powerPeak = NAN;
  // Priority nought and core nought, for the same reason the other pollers are
  // there: this task lives inside HTTPClient, which waits on a socket by
  // yielding rather than blocking, and a task above the idle task that only ever
  // yields never lets the watchdog's own task run.
  // Eight kilobytes, not twelve. A handshake's own buffers come off the heap
  // rather than off here, so what this actually has to hold is HTTPClient and
  // the parse above it - measured at a little over four kilobytes with a reply
  // in hand. The four that frees is internal RAM, which is the memory the
  // handshake then runs out of.
  xTaskCreatePinnedToCore(task, "nuc", 8192, nullptr, 0, nullptr, 0);
}

void nucWatching(bool on) { watching = on; }

void nucLatest(Nuc *out) {
  take();
  *out = held;
  give();
}

uint32_t nucRevision() { return revision; }

uint32_t nucGapMost() { return GAP_MOST_MS; }
