#include "market.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

#include "net.h"
#include "usage.h"
#include "wifi.h"

namespace {

// Which coins, then what they are worth - two calls rather than one, because
// the sparkline is nearly all of the weight. Ten rows of it is forty kilobytes
// and ten rows without it is eight, and the three largest coins do not change
// on the hour, so the asking-which is worth doing rarely and cheaply.
constexpr char RANK[] =
    "https://api.coingecko.com/api/v3/coins/markets?vs_currency=usd&order=market_cap_desc"
    "&per_page=10&page=1&sparkline=false";
constexpr char PRICES[] =
    "https://api.coingecko.com/api/v3/coins/markets?vs_currency=usd&sparkline=true"
    "&price_change_percentage=24h&ids=";
constexpr uint32_t RANK_MS = 1800000;

// Five sessions an hour apiece. A day of it is a line drawn from eight points
// an hour into the morning, which is a chart of nothing; five days is thirty
// for an index and a hundred for a contract that trades around the clock, and
// still only a few kilobytes - which matters, because the reply is read into
// internal RAM while the TLS session is holding forty-eight of it.
constexpr char CHART[] = "https://query1.finance.yahoo.com/v8/finance/chart/";
constexpr char CHART_ARGS[] = "?interval=1h&range=5d";

struct Listing {
  const char *symbol;
  const char *ticker;
  const char *name;
};

// The caret is a URL's own character and has to go up encoded, or the request
// is for a symbol that does not exist.
constexpr Listing LISTINGS[MARKET_INDICES] = {
    {"%5EGSPC", "SPX", "S&P 500"},
    {"%5ENDX", "NDX", "NASDAQ 100"},
    {"ES=F", "ES", "S&P 500 FUTURES"},
    {"NQ=F", "NQ", "NASDAQ 100 FUTURES"},
};

// Ten kilobytes of coin table is the largest of these by some way, and the cap
// is not a size any of them approaches - it is there so a reply that is not
// what this asked for cannot take the heap with it.
constexpr size_t MOST = 20480;
constexpr uint32_t PATIENCE_MS = 6000;
constexpr uint32_t RETRY_MS = 20000;

// The ids the ranking picked, in market cap order, and when it picked them.
char chosen[MARKET_COINS][20] = {{0}};
uint32_t rankedAt = 0;

Market *held = nullptr;
uint32_t readAt[MARKET_SCREENS] = {0};
uint32_t triedAt[MARKET_SCREENS] = {0};
volatile int8_t watching = -1;
volatile uint32_t revision = 0;
SemaphoreHandle_t lock = nullptr;

void take() { xSemaphoreTake(lock, portMAX_DELAY); }
void give() { xSemaphoreGive(lock); }

// Both endpoints are a web page's own backend rather than anything published,
// and Yahoo answers a request with no user agent with a 429 - which reads as
// the symbol being wrong rather than as the actual problem.
void dress(HTTPClient &http) {
  http.addHeader("Accept", "application/json");
  http.setUserAgent(
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) "
      "Version/17.0 Safari/605.1.15");
}

bool fetch(const String &url, String &out) {
  uint32_t began = millis();
  WiFiClientSecure tls;
  // No certificate store on the device, and nothing of ours goes up with it -
  // these are public prices asked for without so much as a name.
  tls.setInsecure();

  HTTPClient http;
  http.setTimeout(12000);
  if (!http.begin(tls, url)) {
    Serial.println("market: begin failed");
    netRecord(url.c_str(), began, 0, 0);
    return false;
  }
  dress(http);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("market: %d from %s\n", code, url.c_str());
    netRecord(url.c_str(), began, code, 0);
    http.end();
    return false;
  }
  out.reserve(MOST / 2);
  bool read = netBody(http, out, MOST, PATIENCE_MS);
  http.end();
  netRecord(url.c_str(), began, code, out.length());
  if (!read) {
    Serial.printf("market: 200 but the body stopped at %u bytes\n", (unsigned)out.length());
  }
  return read;
}

// Past a key and its colon, within the slice it is allowed to be in. The slice
// is what keeps one coin's price from being read as the next one's.
int past(const String &body, const char *key, int from, int limit) {
  int at = body.indexOf(key, from);
  if (at < 0 || (limit >= 0 && at >= limit)) {
    return -1;
  }
  return at + (int)strlen(key);
}

bool numberFor(const String &body, const char *key, int from, int limit, float &out) {
  int at = past(body, key, from, limit);
  if (at < 0 || at >= (int)body.length()) {
    return false;
  }
  // A reading the endpoint has not got is null rather than absent, and toFloat
  // turns that into nought - which on this screen is a price.
  if (body.charAt(at) == 'n') {
    return false;
  }
  int end = at + 24;
  out = body.substring(at, end > (int)body.length() ? (int)body.length() : end).toFloat();
  return true;
}

void textFor(const String &body, const char *key, int from, int limit, char *out, size_t size) {
  out[0] = '\0';
  int at = past(body, key, from, limit);
  if (at < 0) {
    return;
  }
  int close = body.indexOf('"', at);
  if (close < 0) {
    return;
  }
  size_t n = (size_t)(close - at);
  if (n >= size) {
    n = size - 1;
  }
  memcpy(out, body.c_str() + at, n);
  out[n] = '\0';
}

// The series, thinned to what a chart can show. Walked once: the count is
// worked out on the first pass over the values and the ones that are wanted are
// taken on the same pass, because the alternative is scanning the whole array
// again for every point kept.
//
// Nulls are dropped rather than read as nought. An index quotes them for the
// minutes of a session that have not happened yet, and a nought among real
// prices is not a gap in a line, it is a line to the floor.
uint8_t seriesFor(const String &body, const char *key, int from, int limit, float *out,
                  uint8_t most) {
  int at = past(body, key, from, limit);
  if (at < 0) {
    return 0;
  }
  int end = body.indexOf(']', at);
  if (end < 0) {
    return 0;
  }

  uint16_t total = 0;
  for (int i = at; i < end; i++) {
    char c = body.charAt(i);
    if ((c >= '0' && c <= '9') && (i == at || body.charAt(i - 1) == ',' ||
                                  body.charAt(i - 1) == '[' || body.charAt(i - 1) == '-')) {
      total++;
      while (i < end && body.charAt(i) != ',') {
        i++;
      }
    }
  }
  if (!total) {
    return 0;
  }

  uint8_t want = total < most ? (uint8_t)total : most;
  uint8_t kept = 0;
  uint16_t seen = 0;
  int i = at;
  while (i < end && kept < want) {
    char c = body.charAt(i);
    bool starts = (c == '-' || (c >= '0' && c <= '9')) &&
                  (i == at || body.charAt(i - 1) == ',' || body.charAt(i - 1) == '[');
    if (!starts) {
      i++;
      continue;
    }
    // Spread across the whole series with both ends kept exactly, so the line
    // starts and finishes where the price did rather than on a neighbour.
    uint16_t due = want > 1 ? (uint16_t)(((uint32_t)kept * (total - 1) + (want - 1) / 2) /
                                         (want - 1))
                            : 0;
    if (seen == due) {
      int stop = i + 24;
      out[kept++] = body.substring(i, stop > end ? end : stop).toFloat();
    }
    seen++;
    while (i < end && body.charAt(i) != ',') {
      i++;
    }
  }
  return kept;
}

void extent(Market &m) {
  m.low = m.count ? m.points[0] : 0.0f;
  m.high = m.low;
  for (uint8_t i = 1; i < m.count; i++) {
    if (m.points[i] < m.low) {
      m.low = m.points[i];
    }
    if (m.points[i] > m.high) {
      m.high = m.points[i];
    }
  }
}

void mark(uint8_t screen, bool loading, bool failed) {
  take();
  held[screen].loading = loading;
  held[screen].failed = failed;
  give();
  revision = revision + 1;
}

// Which three, off the cheap call. The coins are one group for this reason: the
// endpoint is a table ordered by market cap, so reading down it is asking which
// coins are the largest, and the answer is the same for all three screens.
bool readRanking() {
  String body;
  if (!fetch(String(RANK), body)) {
    return false;
  }

  uint8_t taken = 0;
  int at = 0;
  while (taken < MARKET_COINS) {
    int start = body.indexOf("\"id\":\"", at);
    if (start < 0) {
      break;
    }
    int next = body.indexOf("\"id\":\"", start + 6);
    int limit = next < 0 ? -1 : next;
    at = start + 6;

    char id[20];
    textFor(body, "\"id\":\"", start, limit, id, sizeof(id));
    if (!id[0]) {
      continue;
    }
    float price = 0.0f;
    float moved = 0.0f;
    numberFor(body, "\"current_price\":", start, limit, price);
    numberFor(body, "\"price_change_percentage_24h\":", start, limit, moved);
    // Pinned to a dollar, whatever it calls itself: a week of one is a flat
    // line and a move of a hundredth of a per cent, and a screen of that is a
    // screen spent. Both halves are needed and neither alone would do - a real
    // coin can trade near a dollar, and a real coin can have a quiet day, but
    // not both at once. Measured against the top ten: the two stablecoins in it
    // moved 0.013% and the quietest real coin moved 0.22%.
    if (fabsf(price - 1.0f) < 0.05f && fabsf(moved) < 0.5f) {
      continue;
    }
    strncpy(chosen[taken], id, sizeof(chosen[taken]) - 1);
    chosen[taken][sizeof(chosen[taken]) - 1] = '\0';
    taken++;
  }
  if (taken < MARKET_COINS) {
    Serial.printf("market: only %u coins worth a screen\n", taken);
    return false;
  }
  rankedAt = millis();
  Serial.printf("market: coins %s, %s, %s\n", chosen[0], chosen[1], chosen[2]);
  return true;
}

bool readCoins() {
  bool ageing = !rankedAt || millis() - rankedAt >= RANK_MS;
  // A ranking that will not come is fatal only the first time. After that the
  // last one it gave is better than no screen at all.
  if (ageing && !readRanking() && !rankedAt) {
    return false;
  }

  String url = String(PRICES);
  for (uint8_t i = 0; i < MARKET_COINS; i++) {
    if (i) {
      url += "%2C";
    }
    url += chosen[i];
  }
  String body;
  if (!fetch(url, body)) {
    return false;
  }

  uint8_t filled = 0;
  int at = 0;
  for (;;) {
    int start = body.indexOf("\"id\":\"", at);
    if (start < 0) {
      break;
    }
    int next = body.indexOf("\"id\":\"", start + 6);
    int limit = next < 0 ? -1 : next;
    at = start + 6;

    char id[20];
    textFor(body, "\"id\":\"", start, limit, id, sizeof(id));
    // Put where the coin belongs rather than where it turned up. The endpoint
    // answers in its own order, which matches the order asked for only because
    // the ids went up in market cap order - and that is a coincidence to lean
    // on rather than a promise.
    int8_t slot = -1;
    for (uint8_t i = 0; i < MARKET_COINS; i++) {
      if (strcmp(chosen[i], id) == 0) {
        slot = (int8_t)i;
      }
    }
    if (slot < 0) {
      continue;
    }

    Market m = {};
    textFor(body, "\"symbol\":\"", start, limit, m.ticker, sizeof(m.ticker));
    textFor(body, "\"name\":\"", start, limit, m.name, sizeof(m.name));
    for (char *c = m.ticker; *c; c++) {
      *c = (char)toupper(*c);
    }
    if (!numberFor(body, "\"current_price\":", start, limit, m.price)) {
      continue;
    }
    if (!numberFor(body, "\"price_change_percentage_24h\":", start, limit, m.change)) {
      m.change = 0.0f;
    }
    m.count =
        seriesFor(body, "\"sparkline_in_7d\":{\"price\":[", start, limit, m.points, MARKET_POINTS);
    extent(m);
    strncpy(m.over, "24H", sizeof(m.over) - 1);
    strncpy(m.span, "7D", sizeof(m.span) - 1);
    m.ready = true;

    take();
    held[slot] = m;
    give();
    filled++;
  }
  if (!filled) {
    Serial.println("market: no coins in the reply");
    return false;
  }
  readAt[0] = millis();
  revision = revision + 1;
  return true;
}

bool readListing(uint8_t screen) {
  const Listing &l = LISTINGS[screen - MARKET_COINS];
  String body;
  if (!fetch(String(CHART) + l.symbol + CHART_ARGS, body)) {
    return false;
  }

  Market m = {};
  strncpy(m.ticker, l.ticker, sizeof(m.ticker) - 1);
  strncpy(m.name, l.name, sizeof(m.name) - 1);
  strncpy(m.over, "1D", sizeof(m.over) - 1);
  strncpy(m.span, "5D", sizeof(m.span) - 1);
  if (!numberFor(body, "\"regularMarketPrice\":", 0, -1, m.price)) {
    Serial.printf("market: %s had no price\n", l.ticker);
    return false;
  }

  // previousClose is the session before this one whatever range was asked for.
  // chartPreviousClose is the close before the range itself, so over five days
  // it is five days old - which is the trap this endpoint sets for anybody
  // quoting a day's move from it, and the reason the two are not
  // interchangeable and this one is asked for first. Measured: on the same
  // instrument the pair read 7631.47 and 7675.70.
  float before = 0.0f;
  if (!numberFor(body, "\"previousClose\":", 0, -1, before)) {
    numberFor(body, "\"chartPreviousClose\":", 0, -1, before);
  }
  m.change = before > 0.0f ? (m.price - before) / before * 100.0f : 0.0f;
  m.count = seriesFor(body, "\"close\":[", 0, -1, m.points, MARKET_POINTS);
  extent(m);
  m.ready = true;

  take();
  held[screen] = m;
  give();
  readAt[screen] = millis();
  revision = revision + 1;
  return true;
}

// The coins share one, the listings have one apiece, and it is the group rather
// than the screen that says whether anything needs asking for.
uint8_t groupOf(uint8_t screen) { return screen < MARKET_COINS ? 0 : screen; }

void task(void *) {
  for (;;) {
    int8_t screen = watching;
    uint32_t period = (uint32_t)usageEvery() * 60000UL;
    if (screen >= 0 && screen < MARKET_SCREENS && wifiConnected()) {
      uint8_t group = groupOf((uint8_t)screen);
      bool stale = !readAt[group] || millis() - readAt[group] >= period;
      bool waited = !triedAt[group] || millis() - triedAt[group] >= RETRY_MS;
      if (stale && waited) {
        triedAt[group] = millis();
        mark((uint8_t)screen, true, false);
        netTake();
        bool got = screen < MARKET_COINS ? readCoins() : readListing((uint8_t)screen);
        netGive();
        mark((uint8_t)screen, false, !got);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

}  // namespace

void marketBegin() {
  lock = xSemaphoreCreateMutex();
  // PSRAM: two kilobytes is not much, but internal RAM is what a handshake
  // needs forty-eight of and nothing in here is touched by DMA.
  held = (Market *)heap_caps_calloc(MARKET_SCREENS, sizeof(Market), MALLOC_CAP_SPIRAM);
  if (!held) {
    Serial.println("market: no psram, the screens will stay empty");
    return;
  }
  for (uint8_t i = 0; i < MARKET_SCREENS; i++) {
    if (i >= MARKET_COINS) {
      const Listing &l = LISTINGS[i - MARKET_COINS];
      strncpy(held[i].ticker, l.ticker, sizeof(held[i].ticker) - 1);
      strncpy(held[i].name, l.name, sizeof(held[i].name) - 1);
    }
  }
  // Priority nought and core nought, for the same reason the other two pollers
  // are there: this task lives inside HTTPClient, which waits on a socket by
  // yielding rather than blocking, and a task above the idle task that only
  // ever yields never lets the watchdog's own task run.
  xTaskCreatePinnedToCore(task, "market", 12288, nullptr, 0, nullptr, 0);
}

void marketWatching(int8_t screen) { watching = screen; }

bool marketAt(uint8_t screen, Market *out) {
  if (!held || screen >= MARKET_SCREENS) {
    return false;
  }
  take();
  *out = held[screen];
  give();
  return true;
}

uint32_t marketRevision() { return revision; }
