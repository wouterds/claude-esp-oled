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
volatile bool wake = false;

// What is kept for each token the board has read with, so that going back to an
// account puts what it last said on the glass at once rather than an empty one
// and a wait. Found by a hash of the token and not by its slot: the slots slide
// down when one is trashed, and a token is not something to hold a second copy
// of. In RAM only - it is a reading, and it goes with the power like one.
//
// The organisation is kept with it for the same reason. It belongs to the
// account rather than to the device, it never changes, and asking for it again
// on every swipe was half of what a swipe cost.
enum class Seen : uint8_t { Never, Read, Lost };
struct Account {
  uint32_t key;   // nought for a place nothing has used
  uint32_t used;  // when it was last picked, for choosing which to give up
  char org[40];
  volatile Seen seen;
  volatile uint8_t session;
  volatile uint8_t weekly;
  // When each window rolls over, as a deadline on the device's own clock.
  // Nought for one nobody has spent anything in, which is what the endpoint
  // says by giving no date at all.
  volatile uint32_t resetAt[2];
};
Account accounts[PORTAL_TOKEN_SLOTS] = {};
// Which of them the glass is on, or none of them with no token to read with.
volatile int8_t picked = -1;
uint32_t picks = 0;
// Bumped when the numbers on the glass become another account's: as one with a
// reading is picked, or as the first read of one without lands - and only once
// the numbers are in place, so a reader who sees it move sees them as well.
volatile uint8_t account = 0;
volatile uint8_t still = 0;
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
// Which place the last read was for. The strikes above are against one token,
// and do not carry over to the next.
int8_t polled = -1;
// The last read was turned away rather than lost. A token that has been
// refused is not let in fifteen seconds later, and asking that often is four
// requests a minute with a dead login from an address other people share - so
// it is asked again at the pace the readings are, and at once when a token
// changes.
bool barred = false;

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
bool rolled(Account &a) {
  bool over = false;
  for (uint8_t i = 0; i < 2; i++) {
    if (a.resetAt[i] && (int32_t)(millis() - a.resetAt[i]) >= 0) {
      // Spent as soon as it is acted on. A read that fails leaves the deadline
      // in the past, and left there the wait would break on it every quarter
      // second - which is a poller with no interval at all.
      a.resetAt[i] = 0;
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
  netSecure(tls);

  HTTPClient http;
  http.setTimeout(12000);
  // One request and done, and said so. Asked to keep the connection claude.ai
  // does, and its replies are chunked - so with no length to count down and no
  // close to wait for, the read below sat out the whole of its patience after
  // the last byte had landed. Six seconds on every read that worked, with the
  // session's forty-eight kilobytes held for all of them.
  http.setReuse(false);
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
bool findOrg(const char *token, Account &a) {
  if (a.org[0]) {
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
  if (open < 0 || close < 0 || close - open > (int)sizeof(a.org)) {
    return false;
  }
  body.substring(open + 1, close).toCharArray(a.org, sizeof(a.org));
  Serial.printf("usage: organisation %s\n", a.org);
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
  // A window nobody has opened is null rather than an object with nothing in
  // it. Searched past, the first utilization after it is the next window's -
  // which put the week's figure and the week's reset on the five hour bar of an
  // account that was sitting idle.
  if (body.charAt(at + (int)strlen(name) + 3) != '{') {
    out = 0;
    reset = 0;
    return true;
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

// Zero is the place nothing has used, so no token is allowed to come to it.
uint32_t keyOf(const char *token) {
  uint32_t hash = 2166136261u;
  for (; *token; token++) {
    hash = (hash ^ (uint8_t)*token) * 16777619u;
  }
  return hash ? hash : 1;
}

// Points the glass at whatever the portal has picked: the place kept for that
// token, or the one picked longest ago made over into it.
void pick() {
  char token[PORTAL_TOKEN_MAX];
  if (!portalToken(token, sizeof(token))) {
    picked = -1;
    return;
  }
  uint32_t key = keyOf(token);
  int8_t at = -1;
  int8_t oldest = 0;
  for (int8_t i = 0; i < (int8_t)PORTAL_TOKEN_SLOTS; i++) {
    if (accounts[i].key == key) {
      at = i;
    }
    if (accounts[i].used < accounts[oldest].used) {
      oldest = i;
    }
  }
  if (at < 0) {
    at = oldest;
    Account &a = accounts[at];
    a.seen = Seen::Never;
    a.org[0] = '\0';
    a.session = 0;
    a.weekly = 0;
    a.resetAt[0] = 0;
    a.resetAt[1] = 0;
    a.key = key;
  }
  accounts[at].used = ++picks;
  if (at == picked) {
    return;
  }
  still = 0;
  if (accounts[at].seen == Seen::Read) {
    account = account + 1;
  }
  picked = at;
}

void poll(const char *token, int8_t at) {
  Account &a = accounts[at];
  String body;
  uint8_t hours = 0;
  uint8_t week = 0;
  uint32_t hoursOn = 0;
  uint32_t weekOn = 0;
  bool got = findOrg(token, a) &&
             get(String(HOST) + "/api/organizations/" + a.org + "/usage", token, body) &&
             window(body, "five_hour", hours, hoursOn) &&
             window(body, "seven_day", week, weekOn);
  // A read takes seconds and a swipe does not wait for it. What comes back is
  // still this account's and is kept as that - but the sound and the count of
  // readings that have not moved are about what is on the glass.
  bool shown = at == picked;

  if (!got) {
    // A 404 on the usage path is the organisation not being this account's - a
    // token that changed without this being told, which the portal normally
    // says. What is stale is the organisation and not the token, so it goes and
    // is asked for again next round rather than counting as a strike.
    if (lastCode == 404) {
      Serial.println("usage: 404, asking which organisation again");
      a.org[0] = '\0';
      misses = 0;
      return;
    }
    // Turned away outright is answer enough; anything else gets the benefit of
    // the doubt for a couple of rounds, because a radio drops.
    bool refused = lastCode == 401 || lastCode == 403;
    barred = refused;
    if (refused || ++misses >= GIVE_UP_AT) {
      if (a.seen == Seen::Read) {
        Serial.printf("usage: %s, asking for a token again\n",
                      refused ? "refused" : "no answer");
      }
      a.seen = Seen::Lost;
      misses = 0;
      a.resetAt[0] = 0;
      a.resetAt[1] = 0;
      // A new token may belong to somebody else.
      a.org[0] = '\0';
      if (shown) {
        still = 0;
      }
    }
    return;
  }

  // A window only ever fills while it is open, so the one time either number
  // falls is the one time it rolled over. Against this account's own last
  // reading, however long ago the glass was last on it.
  bool known = a.seen == Seen::Read;
  if (known && shown && (hours < a.session || week < a.weekly)) {
    Serial.println("usage: a window rolled over");
    audioCheered();
  }

  misses = 0;
  barred = false;
  a.resetAt[0] = deadlineOf(hoursOn);
  a.resetAt[1] = deadlineOf(weekOn);
  if (!known || hours != a.session || week != a.weekly) {
    Serial.printf("usage: five hour %u%% for another %lus, seven day %u%% for another %lus\n",
                  hours, (unsigned long)(leftOn(a.resetAt[0]) / 1000), week,
                  (unsigned long)(leftOn(a.resetAt[1]) / 1000));
    if (shown) {
      still = 0;
    }
  } else if (shown && still < 255) {
    still = still + 1;
  }
  a.session = hours;
  a.weekly = week;
  a.seen = Seen::Read;
  if (!known && shown) {
    account = account + 1;
  }
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
    // The place first and the token after, and the one checked against the
    // other: a token picked between the two is another account's, and read into
    // this place its numbers would be kept under the wrong one.
    int8_t at = picked;
    char token[PORTAL_TOKEN_MAX];
    bool have = at >= 0 && portalToken(token, sizeof(token)) && keyOf(token) == accounts[at].key;
    if (wifiConnected() && have && (netWatched() || due) && netRoom("usage")) {
      due = false;
      if (at != polled) {
        polled = at;
        misses = 0;
        barred = false;
      }
      poll(token, at);
      timed = accounts[at].seen == Seen::Read;
      until = millis() + (barred ? (uint32_t)every * 60000UL : RETRY_MS);
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
      if (timed && rolled(accounts[at])) {
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
  pick();
  // Eight kilobytes, not twelve. A handshake's own buffers come off the heap
  // rather than off here, so what this actually has to hold is HTTPClient and
  // the parse above it - measured at a little over four kilobytes with a reply
  // in hand. The four that frees is internal RAM, which is the memory the
  // handshake then runs out of.
  xTaskCreatePinnedToCore(task, "usage", 8192, nullptr, 0, nullptr, 0);
}

void usageTokenChanged() {
  pick();
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

bool usageReady() {
  int8_t at = picked;
  return at >= 0 && accounts[at].seen == Seen::Read;
}

bool usageWaiting() {
  int8_t at = picked;
  return at >= 0 && accounts[at].seen == Seen::Never;
}

uint8_t usageAccount() { return account; }

uint8_t usageStill() { return still; }

uint8_t usageSession() {
  int8_t at = picked;
  return at >= 0 ? accounts[at].session : 0;
}

uint8_t usageWeekly() {
  int8_t at = picked;
  return at >= 0 ? accounts[at].weekly : 0;
}

uint32_t usageSessionResetsIn() {
  int8_t at = picked;
  return at >= 0 ? leftOn(accounts[at].resetAt[0]) : 0;
}

uint32_t usageWeeklyResetsIn() {
  int8_t at = picked;
  return at >= 0 ? leftOn(accounts[at].resetAt[1]) : 0;
}
