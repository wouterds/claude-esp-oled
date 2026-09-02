#include "outage.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "audio.h"
#include "net.h"
#include "usage.h"
#include "wifi.h"

namespace {

constexpr char URL[] = "https://status.claude.com/api/v2/summary.json";
constexpr uint32_t RETRY_MS = 15000;
// While there is nothing to ask over. Short, because this is the gap between
// the radio joining and the first answer.
constexpr uint32_t WAITING_MS = 400;
// The status page is a couple of kilobytes and has been for as long as it has
// been asked. Anything claiming to be much larger is not a reply this needs.
constexpr size_t MOST = 16384;
// Between bytes, not for the whole read: a reply still arriving has not stalled
// however long it takes, and one that has stopped is not coming.
constexpr uint32_t PATIENCE_MS = 6000;

volatile Outage level = Outage::Unknown;

bool fetch(String &out) {
  uint32_t began = millis();
  WiFiClientSecure tls;
  // No certificate store on the device, and nothing of ours goes up with it.
  tls.setInsecure();

  HTTPClient http;
  http.setTimeout(12000);
  if (!http.begin(tls, URL)) {
    Serial.println("outage: begin failed");
    netRecord(URL, began, 0, 0);
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("outage: %d from the status page\n", code);
    netRecord(URL, began, code, 0);
    http.end();
    return false;
  }
  bool read = netBody(http, out, MOST, PATIENCE_MS);
  http.end();
  netRecord(URL, began, code, out.length());
  // A body that does not come over is the one failure here that said nothing at
  // all: the status line was 200, so the log showed a poll that simply never
  // reported, which reads as the poller having stopped rather than as a read
  // that broke.
  if (!read) {
    Serial.printf("outage: 200 but the body stopped at %u bytes\n", (unsigned)out.length());
  }
  return read;
}

// The session is the expensive part and it starts inside fetch, so the lock goes
// round the whole of it rather than round the socket.
bool get(String &out) {
  netTake();
  bool got = fetch(out);
  netGive();
  return got;
}

// Only the components say what is actually broken. The page's own indicator is
// derived and lands a notch under them - it calls a partial outage "minor" -
// and every incident update carries component statuses of its own, which is why
// this is bounded to the array rather than run over the whole reply.
bool saysWithin(const String &body, const char *what, int open, int close) {
  int at = body.indexOf(what, open);
  return at >= 0 && at < close;
}

Outage worstOf(const String &body) {
  int open = body.indexOf("\"components\":[");
  int close = body.indexOf("],\"incidents\"", open);
  if (open < 0 || close < 0) {
    Serial.println("outage: no components in the reply");
    return Outage::None;
  }
  if (saysWithin(body, "\"major_outage\"", open, close)) {
    return Outage::Major;
  }
  // Degraded sits under partial and gets the same colour: there are three
  // states here, and anything short of a whole component being down is the one
  // that says go and read the page.
  if (saysWithin(body, "\"partial_outage\"", open, close) ||
      saysWithin(body, "\"degraded_performance\"", open, close)) {
    return Outage::Partial;
  }
  return Outage::None;
}

const char *nameOf(Outage what) {
  switch (what) {
    case Outage::Major:
      return "major outage";
    case Outage::Partial:
      return "minor outage";
    default:
      return "all operational";
  }
}

void poll() {
  String body;
  if (!get(body)) {
    return;
  }
  Outage worst = worstOf(body);
  if (worst == level) {
    return;
  }
  // The same sound a usage window rolling over gets: either way the thing that
  // was in the way has gone. Coming out of Unknown is a rise, so a first read
  // that finds everything well stays quiet.
  if (worst < level) {
    audioCheered();
  } else if (level == Outage::None) {
    // Only off a read that had found everything well. Out of Unknown nothing
    // has changed - the board is learning where it stands rather than being
    // told it moved - and an outage already running would say so again on every
    // boot for as long as it lasted.
    audioErrored();
  }
  Serial.printf("outage: %s\n", nameOf(worst));
  level = worst;
}

void task(void *) {
  bool onJoin = false;
  for (;;) {
    onJoin = netJoined(NET_OUTAGE) || onJoin;
    bool timed = false;
    uint32_t until = millis() + WAITING_MS;
    if (wifiConnected() && (netWatched() || onJoin)) {
      onJoin = false;
      poll();
      timed = level != Outage::Unknown;
      until = millis() + RETRY_MS;
    }
    for (;;) {
      if (!netWatched() && !onJoin) {
        if (netJoined(NET_OUTAGE)) {
          onJoin = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        continue;
      }
      if (netStale(NET_OUTAGE)) {
        break;
      }
      // Half an interval off the usage read's slot, so the two sit either side
      // of each other instead of coming due together and queueing on the one
      // socket. The interval is the usage read's because there is one setting
      // for both of them.
      uint32_t period = (uint32_t)usageEvery() * 60000UL;
      int32_t left = timed ? (int32_t)netDueIn(period, period / 2)
                           : (int32_t)(until - millis());
      if (left <= 250) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }
}

}  // namespace

// Priority nought, which is the idle task's own, and deliberately.
//
// This task spends its time inside HTTPClient, and HTTPClient waits on a socket
// by polling it in a loop that yields rather than blocks. A task above priority
// nought that only yields never lets the idle task on its core run at all - and
// the idle task is what the task watchdog is watching, so a reply that takes its
// time is not a slow read but a reboot five seconds later, blamed on whichever
// poller happened to be holding the socket.
//
// Neither loop is ours to fix. What is ours is what the spinning can cost:
// level with the idle task, the two are time sliced against each other, so the
// idle task is scheduled whatever the framework does in here and the watchdog
// stays fed. Nothing is given up for it either - this is a poller on a minute's
// timer sharing a core that is otherwise asleep, and the face is on the other
// one.
void outageBegin() { xTaskCreatePinnedToCore(task, "outage", 12288, nullptr, 0, nullptr, 0); }

Outage outageLevel() { return level; }
