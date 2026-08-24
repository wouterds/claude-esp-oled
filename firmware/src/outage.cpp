#include "outage.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "audio.h"
#include "wifi.h"

namespace {

constexpr char URL[] = "https://status.claude.com/api/v2/summary.json";
constexpr uint32_t EVERY_MS = 60000;
constexpr uint32_t RETRY_MS = 15000;
// While there is nothing to ask over. Short, because this is the gap between
// the radio joining and the first answer.
constexpr uint32_t WAITING_MS = 400;

volatile Outage level = Outage::Unknown;

bool get(String &out) {
  WiFiClientSecure tls;
  // No certificate store on the device, and nothing of ours goes up with it.
  tls.setInsecure();

  HTTPClient http;
  http.setTimeout(12000);
  if (!http.begin(tls, URL)) {
    Serial.println("outage: begin failed");
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("outage: %d from the status page\n", code);
    http.end();
    return false;
  }
  out = http.getString();
  http.end();
  return true;
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
  }
  Serial.printf("outage: %s\n", nameOf(worst));
  level = worst;
}

void task(void *) {
  for (;;) {
    uint32_t wait = WAITING_MS;
    if (wifiConnected()) {
      poll();
      wait = level == Outage::Unknown ? RETRY_MS : EVERY_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(wait));
  }
}

}  // namespace

void outageBegin() { xTaskCreatePinnedToCore(task, "outage", 12288, nullptr, 1, nullptr, 0); }

Outage outageLevel() { return level; }
