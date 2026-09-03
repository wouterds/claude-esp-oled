#include "usage.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <string.h>

#include "audio.h"
#include "net.h"
#include "portal.h"
#include "wifi.h"

namespace {

constexpr char HOST[] = "https://claude.ai";
// A minute is what the endpoint's own windows move on; five is as long as the
// numbers can go stale before the glass is telling somebody yesterday's news.
constexpr uint8_t EVERY_MIN = 1;
constexpr uint8_t EVERY_MAX = 5;
constexpr uint32_t RETRY_MS = 15000;
// While there is nothing to ask with or nothing to ask over. Short, because
// this is the gap between the radio joining and the bars filling, and a retry
// timer spent waiting on the handshake is fifteen seconds of empty track.
constexpr uint32_t WAITING_MS = 400;
// The usage reply is under two kilobytes and the list of organisations is
// smaller again. The cap is not a size either of them approaches - it is there
// so a reply that is not what this asked for cannot take the heap with it.
constexpr size_t MOST = 16384;
// Between bytes, not for the whole read.
constexpr uint32_t PATIENCE_MS = 6000;

Preferences store;
volatile uint8_t every = EVERY_MIN;
char org[40] = {0};
volatile bool ready = false;
volatile uint8_t session = 0;
volatile uint8_t weekly = 0;
volatile bool wake = false;
volatile bool switched = false;
volatile uint8_t still = 0;
// When each window rolls over, as a deadline on the device's own clock. Nought
// for one nobody has spent anything in, which is what the endpoint says by
// giving no date at all.
volatile uint32_t resetAt[2] = {0, 0};
// What the endpoint said the time was, and when that was heard. The device has
// no clock of its own and no reason to ask the network for one: the reply that
// carries the deadlines carries the time they are measured against.
uint32_t heard = 0;
uint32_t heardAt = 0;
int lastCode = 0;
uint8_t misses = 0;
// A token can be revoked, or simply expire. Three refusals in a row and the
// device goes back to asking for a new one: the address returns, the numbers go
// away and the bars come in.
constexpr uint8_t GIVE_UP_AT = 3;

// Days from the epoch to a civil date. No calendar library and no local time to
// be wrong about - everything on this endpoint is UTC, and this is the whole of
// what turning a date into a number takes.
int32_t daysFrom(int32_t y, int32_t m, int32_t d) {
  y -= m <= 2;
  int32_t era = (y >= 0 ? y : y - 399) / 400;
  int32_t yoe = y - era * 400;
  int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

uint32_t epochOf(int32_t y, int32_t mo, int32_t d, int32_t h, int32_t mi, int32_t s) {
  // Through 64 bits on the way: a day count times eighty-six thousand runs off
  // the end of a signed 32 in 2038, and the seconds themselves do not.
  return (uint32_t)((int64_t)daysFrom(y, mo, d) * 86400 + h * 3600 + mi * 60 + s);
}

// "2026-08-21T22:30:00.191734+00:00". Read as far as the seconds: the fraction
// is more than a countdown wants and the offset is always zero here.
uint32_t whenOf(const String &iso) {
  int y, mo, d, h, mi, s;
  if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) {
    return 0;
  }
  return epochOf(y, mo, d, h, mi, s);
}

// "Fri, 21 Aug 2026 19:20:27 GMT", off the response's own Date header.
uint32_t nowOf(const String &date) {
  constexpr char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char name[4] = {0};
  int d, y, h, mi, s;
  if (sscanf(date.c_str(), "%*3s, %d %3s %d %d:%d:%d", &d, name, &y, &h, &mi, &s) != 6) {
    return 0;
  }
  const char *at = strstr(MONTHS, name);
  if (!at) {
    return 0;
  }
  return epochOf(y, (int32_t)(at - MONTHS) / 3 + 1, d, h, mi, s);
}

// A date turned into the deadline it is on this device. One already past is one
// the next read will have moved, so it counts as not knowing.
uint32_t deadlineOf(uint32_t when) {
  if (!when || !heard || when <= heard) {
    return 0;
  }
  return heardAt + (when - heard) * 1000;
}

// A window rolling over is worth a read of its own: what is behind it has just
// changed, and sitting out the rest of the interval leaves the old figures on
// the glass with a countdown at nought under them.
bool rolled() {
  bool over = false;
  for (uint8_t i = 0; i < 2; i++) {
    if (resetAt[i] && (int32_t)(millis() - resetAt[i]) >= 0) {
      // Spent as soon as it is acted on. A read that fails leaves the deadline
      // in the past, and left there the wait would break on it every quarter
      // second - which is a poller with no interval at all.
      resetAt[i] = 0;
      over = true;
    }
  }
  return over;
}

uint32_t leftOn(uint32_t deadline) {
  if (!deadline) {
    return 0;
  }
  uint32_t left = deadline - millis();
  // Unsigned all the way round, so the count survives the millis wrap; a span
  // this side of a fortnight is one the sign bit can still answer for.
  return (int32_t)left > 0 ? left : 0;
}

// The web app's own headers, near enough. The endpoint is not documented and
// answers a browser, so it is asked the way a browser asks.
void dress(HTTPClient &http, const char *token) {
  String cookie = "sessionKey=";
  cookie += token;
  http.addHeader("Cookie", cookie);
  http.addHeader("Accept", "*/*");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("anthropic-client-platform", "web_claude_ai");
  http.addHeader("Referer", "https://claude.ai/new");
  http.setUserAgent(
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) "
      "Version/17.0 Safari/605.1.15");
}

bool fetch(const String &url, const char *token, String &out) {
  uint32_t began = millis();
  WiFiClientSecure tls;
  // No certificate store on the device, and the only thing being carried is a
  // token going back to the host that issued it.
  tls.setInsecure();

  HTTPClient http;
  http.setTimeout(12000);
  if (!http.begin(tls, url)) {
    Serial.println("usage: begin failed");
    netRecord(url.c_str(), began, 0, 0);
    return false;
  }
  dress(http, token);
  static const char *WANTED[] = {"Date"};
  http.collectHeaders(WANTED, 1);
  int code = http.GET();
  lastCode = code;
  if (code != 200) {
    Serial.printf("usage: %d from %s\n", code, url.c_str());
    netRecord(url.c_str(), began, code, 0);
    http.end();
    return false;
  }
  uint32_t said = nowOf(http.header("Date"));
  if (said) {
    heard = said;
    heardAt = millis();
    // The call log has no clock of its own and this is the only reply on the
    // board that carries one.
    netHeard(said);
  }
  bool read = netBody(http, out, MOST, PATIENCE_MS);
  http.end();
  netRecord(url.c_str(), began, code, out.length());
  // A body that does not come over is the one failure here that said nothing at
  // all: the status line was 200, so the log showed a poll that simply never
  // reported, which reads as the poller having stopped rather than as a read
  // that broke.
  if (!read) {
    Serial.printf("usage: 200 but the body stopped at %u bytes\n", (unsigned)out.length());
  }
  return read;
}

// Held across the one request rather than across the pair of them below, so the
// other poller gets its turn in between instead of waiting on both.
bool get(const String &url, const char *token, String &out) {
  netTake();
  bool got = fetch(url, token, out);
  netGive();
  return got;
}

// Which organisation the account is in, which the address of everything else
// hangs off. Asked for rather than written down: it is not the same for
// everybody and it does not belong in a public repository.
bool findOrg(const char *token) {
  if (org[0]) {
    return true;
  }
  String body;
  if (!get(String(HOST) + "/api/organizations", token, body)) {
    return false;
  }
  int at = body.indexOf("\"uuid\"");
  if (at < 0) {
    Serial.println("usage: no organisation in the list");
    return false;
  }
  int open = body.indexOf('"', body.indexOf(':', at) + 1);
  int close = body.indexOf('"', open + 1);
  if (open < 0 || close < 0 || close - open > (int)sizeof(org)) {
    return false;
  }
  body.substring(open + 1, close).toCharArray(org, sizeof(org));
  Serial.printf("usage: organisation %s\n", org);
  return true;
}

// Every window in the document is an object with a utilization in it, so the
// one wanted is found by name and read from there. No parser: the whole reply
// is under two kilobytes and only two numbers of it matter.
bool window(const String &body, const char *name, uint8_t &out, uint32_t &reset) {
  int at = body.indexOf(String("\"") + name + "\":");
  if (at < 0) {
    return false;
  }
  int found = body.indexOf("\"utilization\":", at);
  if (found < 0) {
    return false;
  }
  // Past the key and its colon, which is fourteen characters and not fifteen -
  // one over and the leading digit of every number is thrown away.
  int start = found + 14;
  int end = body.indexOf(',', start);
  if (end < 0) {
    return false;
  }
  float value = body.substring(start, end).toFloat();
  out = (uint8_t)(value < 0.0f ? 0.0f : (value > 100.0f ? 100.0f : value + 0.5f));

  // Sat right behind the utilization inside the same object, so the first one
  // past it is this window's. A window with nothing in it says null rather than
  // a date, which is why the quote is looked for where it has to be rather than
  // searched for - searched for, a null borrows the next window's.
  reset = 0;
  int stamp = body.indexOf("\"resets_at\":", found);
  if (stamp >= 0) {
    int opens = stamp + 12;
    if (body.charAt(opens) == '"') {
      int close = body.indexOf('"', opens + 1);
      if (close > opens) {
        reset = whenOf(body.substring(opens + 1, close));
      }
    }
  }
  return true;
}

void poll(const char *token) {
  if (switched) {
    switched = false;
    // The organisation was worked out from the token before this one and is not
    // the same account's. Dropped here rather than where the new token landed,
    // so this task stays the only one that ever touches it.
    org[0] = '\0';
    misses = 0;
  }
  String body;
  uint8_t hours = 0;
  uint8_t week = 0;
  uint32_t hoursOn = 0;
  uint32_t weekOn = 0;
  bool got = findOrg(token) &&
             get(String(HOST) + "/api/organizations/" + org + "/usage", token, body) &&
             window(body, "five_hour", hours, hoursOn) &&
             window(body, "seven_day", week, weekOn);

  if (!got) {
    // A 404 on the usage path is the organisation not being this account's - a
    // token that changed without this being told, which the portal normally
    // says. What is stale is the organisation and not the token, so it goes and
    // is asked for again next round rather than counting as a strike.
    if (lastCode == 404) {
      Serial.println("usage: 404, asking which organisation again");
      org[0] = '\0';
      misses = 0;
      return;
    }
    // Turned away outright is answer enough; anything else gets the benefit of
    // the doubt for a couple of rounds, because a radio drops.
    bool refused = lastCode == 401 || lastCode == 403;
    if (refused || ++misses >= GIVE_UP_AT) {
      if (ready) {
        Serial.printf("usage: %s, asking for a token again\n",
                      refused ? "refused" : "no answer");
      }
      ready = false;
      still = 0;
      misses = 0;
      resetAt[0] = 0;
      resetAt[1] = 0;
      // A new token may belong to somebody else.
      org[0] = '\0';
    }
    return;
  }

  // A window only ever fills while it is open, so the one time either number
  // falls is the one time it rolled over.
  if (ready && (hours < session || week < weekly)) {
    Serial.println("usage: a window rolled over");
    audioCheered();
  }

  misses = 0;
  resetAt[0] = deadlineOf(hoursOn);
  resetAt[1] = deadlineOf(weekOn);
  if (!ready || hours != session || week != weekly) {
    Serial.printf("usage: five hour %u%% for another %lus, seven day %u%% for another %lus\n",
                  hours, (unsigned long)(leftOn(resetAt[0]) / 1000), week,
                  (unsigned long)(leftOn(resetAt[1]) / 1000));
    still = 0;
  } else if (still < 255) {
    still = still + 1;
  }
  session = hours;
  weekly = week;
  ready = true;
}

void task(void *) {
  // A join, or a different token being picked. Either is worth a read whether
  // or not the page is in front of somebody: the first leaves it empty until
  // somebody swipes onto it and the second leaves it showing the last account's
  // numbers. Held rather than acted on where it is noticed, because both turn
  // up inside the wait below and the read belongs at the top of the loop.
  bool due = false;
  for (;;) {
    due = netJoined(NET_USAGE) || due;
    if (wake) {
      wake = false;
      due = true;
    }
    bool timed = false;
    uint32_t until = millis() + WAITING_MS;
    char token[PORTAL_TOKEN_MAX];
    bool have = portalToken(token, sizeof(token));
    if (wifiConnected() && have && (netWatched() || due)) {
      due = false;
      poll(token);
      timed = ready;
      until = millis() + RETRY_MS;
    }
    // A quarter second at a time rather than one long delay: the wait can be
    // cut short by a wake, the idle task the watchdog watches is scheduled all
    // the way through it however long the interval is, and a timed wait asks
    // the cadence again each pass - so a change to the interval takes hold of
    // the wait already under way rather than only the one after it.
    for (;;) {
      // Off the face there is nothing due: the wait idles here rather than
      // counting down to a read that would land on a page nobody is on.
      if (!netWatched() && !due) {
        // Cleared at the top of the loop rather than here, so both ways out of
        // this wait leave it in the same state.
        if (netJoined(NET_USAGE) || wake) {
          due = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        continue;
      }
      // Back, and away long enough that what is on the glass has gone off - so
      // it is read now rather than at whatever is left of the interval.
      if (netStale(NET_USAGE)) {
        break;
      }
      int32_t left = timed ? (int32_t)netDueIn((uint32_t)every * 60000UL, 0)
                           : (int32_t)(until - millis());
      if (left <= 250) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(250));
      if (wake) {
        wake = false;
        break;
      }
      if (timed && rolled()) {
        break;
      }
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
void usageBegin() {
  store.begin("usage", false);
  uint8_t kept = store.getUChar("every", EVERY_MIN);
  every = (kept >= EVERY_MIN && kept <= EVERY_MAX) ? kept : EVERY_MIN;
  Serial.printf("usage: reading every %u minute%s\n", every, every == 1 ? "" : "s");
  // Eight kilobytes, not twelve. A handshake's own buffers come off the heap
  // rather than off here, so what this actually has to hold is HTTPClient and
  // the parse above it - measured at a little over four kilobytes with a reply
  // in hand. The four that frees is internal RAM, which is the memory the
  // handshake then runs out of.
  xTaskCreatePinnedToCore(task, "usage", 8192, nullptr, 0, nullptr, 0);
}

void usageTokenChanged() {
  switched = true;
  wake = true;
}

uint8_t usageEvery() { return every; }

bool usageSetEvery(uint8_t minutes) {
  if (minutes < EVERY_MIN || minutes > EVERY_MAX || minutes == every) {
    return minutes >= EVERY_MIN && minutes <= EVERY_MAX;
  }
  every = minutes;
  store.putUChar("every", minutes);
  Serial.printf("usage: reading every %u minute%s\n", minutes, minutes == 1 ? "" : "s");
  return true;
}

bool usageReady() { return ready; }

uint8_t usageStill() { return still; }

uint8_t usageSession() { return session; }

uint8_t usageWeekly() { return weekly; }

uint32_t usageSessionResetsIn() { return leftOn(resetAt[0]); }

uint32_t usageWeeklyResetsIn() { return leftOn(resetAt[1]); }
