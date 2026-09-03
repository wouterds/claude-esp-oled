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

// Which coins, then what one of them is worth. The sparkline is nearly all of
// the weight - fifteen rows without one is twelve kilobytes and a single row
// with one is four - so the line is asked for one coin at a time, which is the
// only coin whose screen is up anyway.
//
// Named rather than ranked. The ranking was read first to find the three largest
// worth a screen, which cost a call, a filter for the things sitting at a dollar
// that came back with it, and a screen count that could not be known until the
// call landed. Three coins picked by hand cost none of that and change about as
// often as the ranking's top three do.
constexpr char PRICES[] =
    "https://api.coingecko.com/api/v3/coins/markets?vs_currency=usd&sparkline=true"
    "&price_change_percentage=24h&ids=";

// One session, five minutes apiece - the same day the percentage beside it is
// quoted over. A hundred and thirty-five points for a contract that trades
// nearly around the clock, and eight kilobytes of them. Two minutes apiece
// would be eighteen, and the reply is read into internal RAM while the TLS
// session is holding forty-eight of it.
constexpr char CHART[] = "https://query1.finance.yahoo.com/v8/finance/chart/";
constexpr char CHART_ARGS[] = "?interval=5m&range=1d";

struct Coin {
  const char *id;
  const char *ticker;
  const char *name;
};

constexpr Coin COINS[MARKET_COINS] = {
    {"bitcoin", "BTC", "BITCOIN"},
    {"ethereum", "ETH", "ETHEREUM"},
    {"litecoin", "LTC", "LITECOIN"},
};

struct Listing {
  const char *symbol;
  const char *ticker;
  const char *name;
};

constexpr Listing LISTINGS[MARKET_INDICES] = {
    {"ES=F", "ES", "S&P 500 FUTURES"},
    {"NQ=F", "NQ", "NASDAQ 100 FUTURES"},
};

// Twelve kilobytes of coin list is the largest of these; one coin with its line
// is four and an index is three to eight. The cap is not a size any of them
// approaches - it is there so a reply that is not what this asked for cannot
// take the heap with it.
constexpr size_t MOST = 16384;
constexpr uint32_t PATIENCE_MS = 6000;
constexpr uint32_t RETRY_MS = 20000;

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
  netSecure(tls);

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
// `tail` keeps only the last that many values, or all of them at nought. The
// series upstream gives is a week and the span wanted is a day, and the points
// are an hour apiece - so the day is the last twenty-four of them rather than
// another call.
uint8_t seriesFor(const String &body, const char *key, int from, int limit, float *out,
                  uint8_t most, uint16_t tail) {
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

  uint16_t first = (tail && total > tail) ? (uint16_t)(total - tail) : 0;
  uint16_t within = (uint16_t)(total - first);
  uint8_t want = within < most ? (uint8_t)within : most;
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
    uint16_t due = first + (want > 1 ? (uint16_t)(((uint32_t)kept * (within - 1) + (want - 1) / 2) /
                                                  (want - 1))
                                     : 0);
    if (seen >= first && seen == due) {
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

// One coin's own line. Asked for by id, which is the only way to get a
// sparkline without paying for fourteen others alongside it.
bool readCoin(uint8_t screen) {
  String body;
  if (!fetch(String(PRICES) + COINS[screen].id, body)) {
    return false;
  }

  Market m = {};
  textFor(body, "\"symbol\":\"", 0, -1, m.ticker, sizeof(m.ticker));
  textFor(body, "\"name\":\"", 0, -1, m.name, sizeof(m.name));
  for (char *c = m.ticker; *c; c++) {
    *c = (char)toupper(*c);
  }
  if (!numberFor(body, "\"current_price\":", 0, -1, m.price)) {
    Serial.printf("market: %s had no price\n", COINS[screen].id);
    return false;
  }
  if (!numberFor(body, "\"price_change_percentage_24h\":", 0, -1, m.change)) {
    m.change = 0.0f;
  }
  // The last day of the week it gives: the points are an hour apiece, so the
  // last twenty-four of them are the last twenty-four hours, and that is the
  // span the percentage beside it is quoted over.
  m.count = seriesFor(body, "\"sparkline_in_7d\":{\"price\":[", 0, -1, m.points, MARKET_POINTS, 24);
  extent(m);
  strncpy(m.over, "24H", sizeof(m.over) - 1);
  strncpy(m.span, "24H", sizeof(m.span) - 1);
  m.ready = true;

  take();
  held[screen] = m;
  give();
  readAt[screen] = millis();
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
  strncpy(m.span, "24H", sizeof(m.span) - 1);
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
  m.count = seriesFor(body, "\"close\":[", 0, -1, m.points, MARKET_POINTS, 0);
  extent(m);
  m.ready = true;

  take();
  held[screen] = m;
  give();
  readAt[screen] = millis();
  revision = revision + 1;
  return true;
}

void task(void *) {
  for (;;) {
    int8_t screen = watching;
    uint32_t period = (uint32_t)usageEvery() * 60000UL;
    if (screen >= 0 && screen < MARKET_SCREENS && wifiConnected()) {
      uint8_t at = (uint8_t)screen;
      bool stale = !readAt[at] || millis() - readAt[at] >= period;
      bool waited = !triedAt[at] || millis() - triedAt[at] >= RETRY_MS;
      if (stale && waited) {
        triedAt[at] = millis();
        if (netRoom("market")) {
          mark(at, true, false);
          netTake();
          bool got = at < MARKET_COINS ? readCoin(at) : readListing(at);
          netGive();
          mark(at, false, !got);
        }
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
  // The names up front, so a screen swiped onto says what it is while its own
  // call is still out.
  for (uint8_t i = 0; i < MARKET_SCREENS; i++) {
    const char *ticker = i < MARKET_COINS ? COINS[i].ticker : LISTINGS[i - MARKET_COINS].ticker;
    const char *name = i < MARKET_COINS ? COINS[i].name : LISTINGS[i - MARKET_COINS].name;
    strncpy(held[i].ticker, ticker, sizeof(held[i].ticker) - 1);
    strncpy(held[i].name, name, sizeof(held[i].name) - 1);
  }
  // Priority nought and core nought, for the same reason the other two pollers
  // are there: this task lives inside HTTPClient, which waits on a socket by
  // yielding rather than blocking, and a task above the idle task that only
  // ever yields never lets the watchdog's own task run.
  // Eight kilobytes, not twelve. A handshake's own buffers come off the heap
  // rather than off here, so what this actually has to hold is HTTPClient and
  // the parse above it - measured at a little over four kilobytes with a reply
  // in hand. The four that frees is internal RAM, which is the memory the
  // handshake then runs out of.
  xTaskCreatePinnedToCore(task, "market", 8192, nullptr, 0, nullptr, 0);
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
