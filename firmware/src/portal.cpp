#include "portal.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <WebServer.h>
#include <string.h>
#include <uri/UriBraces.h>

#include "net.h"
#include "page.h"
#include "shot.h"
#include "usage.h"
#include "version.h"
#include "wifi.h"

namespace {

constexpr uint16_t PORT = 80;

Preferences store;
WebServer server(PORT);
char tokens[PORTAL_TOKEN_SLOTS][PORTAL_TOKEN_MAX] = {{0}};
uint8_t tokenCount = 0;
// Which one the requests go out with. Always a real slot while there is one.
uint8_t picked = 0;

// The whole page, and it never changes: what is on it comes from /state and
// goes back through /token, so the board serves one string out of flash rather
// than building a new one per request on the core the radio is on. It lives in
// assets/portal.html and reaches here already gzipped - see page.py for why.
//
// Tailwind off a CDN, which the board never fetches - the browser does, and the
// browser is the thing on this network with a way out of it. The alternative is
// a stylesheet in the firmware image, paid for on every flash.
void handleRoot() {
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, PSTR("text/html"), (PGM_P)PAGE, sizeof(PAGE));
}

void tokenKey(char *out, size_t size, uint8_t i) { snprintf(out, size, "t%u", i); }

// The whole set, rewritten whenever one of them moves. Anything past the new
// count is removed rather than left behind: a token is the whole of somebody's
// login, and it being out of the flash is the only thing trashing it does.
void saveTokens() {
  char key[4];
  for (uint8_t i = 0; i < tokenCount; i++) {
    tokenKey(key, sizeof(key), i);
    store.putString(key, tokens[i]);
  }
  for (uint8_t i = tokenCount; i < PORTAL_TOKEN_SLOTS; i++) {
    tokenKey(key, sizeof(key), i);
    store.remove(key);
  }
  store.putUChar("tn", tokenCount);
  store.putUChar("tp", picked);
}

void loadTokens() {
  if (store.isKey("tn")) {
    tokenCount = store.getUChar("tn", 0);
    if (tokenCount > PORTAL_TOKEN_SLOTS) {
      tokenCount = PORTAL_TOKEN_SLOTS;
    }
    char key[4];
    for (uint8_t i = 0; i < tokenCount; i++) {
      tokenKey(key, sizeof(key), i);
      store.getString(key, tokens[i], PORTAL_TOKEN_MAX);
    }
    picked = store.getUChar("tp", 0);
    if (picked >= tokenCount) {
      picked = 0;
    }
    return;
  }
  // The single token this replaced, carried into the first slot so a board
  // already reading an account keeps reading it across the flash.
  store.getString("token", tokens[0], PORTAL_TOKEN_MAX);
  tokenCount = tokens[0][0] ? 1 : 0;
  store.remove("token");
  saveTokens();
}

// Enough of one to tell two apart and not enough to be one. Both ends rather
// than the tail alone: the head is sk-ant-sid02- on every one of them and says
// nothing, but a hint that starts where the token does reads as the token
// rather than as a scrap of one. Anything shorter than three of these is not a
// session token and gets no more than the ellipsis - the ends of a short string
// are the string.
constexpr size_t HINT_END = 12;

void tokenHint(const char *value, char *out, size_t size) {
  size_t len = strlen(value);
  if (len < HINT_END * 3) {
    snprintf(out, size, "\u2026");
    return;
  }
  snprintf(out, size, "%.*s\u2026%s", (int)HINT_END, value, value + len - HINT_END);
}

// An SSID is somebody else's string and may hold a quote or a backslash. Left
// raw, one of those ends this object early and the page reads the parse failure
// as the board having stopped answering.
void appendQuoted(String &json, const char *s) {
  json += '"';
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') {
      json += '\\';
    }
    json += *s;
  }
  json += '"';
}

// Names only. There is no accessor for a stored password and this is the reason
// there is not: the page that would show one is this one.
void networksInto(String &json) {
  const char *on = wifiNetwork();
  json += '[';
  bool any = false;
  for (uint8_t i = 0; i < wifiKnown(); i++) {
    const char *ssid = wifiKnownSsid(i);
    if (!ssid) {
      continue;
    }
    if (any) {
      json += ',';
    }
    any = true;
    json += "{\"ssid\":";
    appendQuoted(json, ssid);
    bool live = on && strcmp(on, ssid) == 0;
    json += ",\"connected\":";
    json += live ? "true" : "false";
    if (live) {
      json += ",\"address\":";
      const char *address = wifiAddress();
      appendQuoted(json, address ? address : "");
      json += ",\"rssi\":";
      json += wifiRssi();
    }
    json += '}';
  }
  json += ']';
}

void handleAddNetwork() {
  String ssid = server.arg("ssid");
  // The name only. A passphrase may legitimately begin or end in a space and
  // trimming one would leave a network that cannot be joined and no way to see
  // why; a name pasted with a stray space is the far likelier of the two.
  ssid.trim();
  if (!ssid.length()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"it needs a name\"}");
    return;
  }
  if (!wifiAdd(ssid.c_str(), server.arg("password").c_str())) {
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"already known, too long, or no room left\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSetEvery() {
  if (!usageSetEvery((uint8_t)server.arg("minutes").toInt())) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"that is not between one and five minutes\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleForgetNetwork() {
  if (!wifiForget(server.arg("ssid").c_str())) {
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"that is not one it knows\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRequests() {
  // Only the calls made since the one the page says it has seen, newest first.
  // A page that claims more than were ever made is from before a reset, and one
  // that says nothing has none of them - both get the lot.
  uint32_t made = netCallsMade();
  uint32_t since = strtoul(server.arg("since").c_str(), nullptr, 10);
  uint16_t n = netCallCount();
  if (since <= made && made - since < n) {
    n = (uint16_t)(made - since);
  }

  // Sent as it is built rather than assembled and then sent. A hundred rows is
  // thirteen kilobytes of JSON, and a String that size is thirteen kilobytes of
  // internal heap taken while a TLS handshake on the other task may be wanting
  // forty-eight of it.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  String out = "{\"calls\":";
  out += made;
  out += ",\"rows\":[";
  NetCall c;
  for (uint16_t i = 0; i < n; i++) {
    // Counted back from the call the reply says it runs up to, not from
    // whatever is newest by now: one made while this is being sent would
    // otherwise be in here and then sent again as new.
    if (!netCallAt((uint16_t)(i + (netCallsMade() - made)), &c)) {
      break;
    }
    if (i) {
      out += ',';
    }
    out += "{\"at\":";
    out += c.at;
    out += ",\"ms\":";
    out += c.ms;
    out += ",\"code\":";
    out += c.code;
    out += ",\"size\":";
    out += c.size;
    out += ",\"url\":";
    appendQuoted(out, c.url);
    out += '}';
    if (out.length() >= 1024) {
      server.sendContent(out);
      out = "";
    }
  }
  out += "]}";
  server.sendContent(out);
  // An empty chunk is what ends a chunked reply.
  server.sendContent("");
}

// Hints only, for the same reason the networks are names only: the page that
// would show a token is the page anybody on this network can open.
void tokensInto(String &json) {
  json += '[';
  char hint[32];
  for (uint8_t i = 0; i < tokenCount; i++) {
    if (i) {
      json += ',';
    }
    tokenHint(tokens[i], hint, sizeof(hint));
    json += "{\"hint\":";
    appendQuoted(json, hint);
    json += ",\"picked\":";
    json += i == picked ? "true" : "false";
    json += '}';
  }
  json += ']';
}

void handleAddToken() {
  String given = server.arg("token");
  given.trim();
  if (!given.length()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"it needs a token\"}");
    return;
  }
  if (given.length() >= PORTAL_TOKEN_MAX) {
    server.send(413, "application/json",
                "{\"ok\":false,\"error\":\"that is longer than the store will take\"}");
    return;
  }
  if (tokenCount >= PORTAL_TOKEN_SLOTS) {
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"there is no room for another one\"}");
    return;
  }
  for (uint8_t i = 0; i < tokenCount; i++) {
    if (strcmp(tokens[i], given.c_str()) == 0) {
      server.send(409, "application/json",
                  "{\"ok\":false,\"error\":\"it already has that one\"}");
      return;
    }
  }
  // The store first and the copy in RAM only if it took: a write that failed
  // would otherwise leave the board using a token the page has just been told
  // it does not have. Nought written is the refusal.
  char key[4];
  tokenKey(key, sizeof(key), tokenCount);
  if (store.putString(key, given.c_str()) == 0) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"the flash would not take it\"}");
    return;
  }
  strncpy(tokens[tokenCount], given.c_str(), PORTAL_TOKEN_MAX - 1);
  tokens[tokenCount][PORTAL_TOKEN_MAX - 1] = '\0';
  // Onto the new one, which is what typing one in is asking for.
  picked = tokenCount++;
  store.putUChar("tn", tokenCount);
  store.putUChar("tp", picked);
  Serial.printf("portal: token %u of %u added and picked\n", picked + 1, tokenCount);
  usageTokenChanged();
  server.send(200, "application/json", "{\"ok\":true}");
}

// Which slot is meant, by position in the list the page drew.
int tokenAt() {
  long at = server.arg("index").toInt();
  return at >= 0 && at < (long)tokenCount ? (int)at : -1;
}

void handleTrashToken() {
  int at = tokenAt();
  if (at < 0) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"that is not one it has\"}");
    return;
  }
  bool was = (uint8_t)at == picked;
  for (uint8_t i = (uint8_t)at; i + 1 < tokenCount; i++) {
    memcpy(tokens[i], tokens[i + 1], PORTAL_TOKEN_MAX);
  }
  tokenCount--;
  memset(tokens[tokenCount], 0, PORTAL_TOKEN_MAX);
  // The one that was picked is gone, so the front one takes over rather than
  // nothing doing: there is still an account to read and the board is for
  // reading it. The others only slid down a slot and keep their turn.
  if (was) {
    picked = 0;
  } else if (picked > (uint8_t)at) {
    picked--;
  }
  saveTokens();
  Serial.printf("portal: token %d trashed, %u left\n", at + 1, tokenCount);
  if (was) {
    usageTokenChanged();
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// Names and sizes, and ten of them at the most.
void shotsInto(String &json) {
  json += '[';
  char name[SHOT_NAME_MAX];
  uint32_t bytes = 0;
  for (uint8_t i = 0; shotAt(i, name, &bytes); i++) {
    if (i) {
      json += ',';
    }
    json += "{\"name\":";
    appendQuoted(json, name);
    json += ",\"size\":";
    json += bytes;
    json += '}';
  }
  json += ']';
}

// Everything the page shows but the call log, in the one reply. It used to be
// four, asked for together every five seconds - and this server answers one
// connection at a time, so on a slow radio the next four arrived before the
// last four were done and the queue only ever grew. Small enough to go out in a
// single write, which is the other half: a reply that fits the socket's buffer
// is the stack's to deliver, and one that does not is this task's to wait on.
void handleState() {
  String json;
  json.reserve(1536);
  json = "{\"address\":";
  // Once, because the radio task can drop the association between two reads of
  // it and the second would come back null for the first one's non-null answer.
  const char *address = wifiAddress();
  appendQuoted(json, address ? address : "");
  json += ",\"every\":";
  json += usageEvery();
  json += ",\"calls\":";
  json += netCallsMade();
  // What the board can say about itself over the one channel that does not need
  // a serial cable. The smallest free block rather than the total: a handshake
  // wants tens of kilobytes of it contiguous, so that is the number that says
  // whether the next read will land or abort.
  json += ",\"uptime\":";
  json += millis() / 1000;
  json += ",\"internal\":";
  json += (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  json += ",\"internalLow\":";
  json += (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  json += ",\"largest\":";
  json += (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  json += ",\"build\":";
  appendQuoted(json, BUILD_COMMIT);
  json += ",\"tokens\":";
  tokensInto(json);
  json += ",\"networks\":";
  networksInto(json);
  json += ",\"shots\":";
  shotsInto(json);
  json += "}";
  server.send(200, "application/json", json);
}

// Under its own name rather than behind a query, so the tab it opens in is
// called what the file is called and saving it out of the browser keeps that
// name. The segment cannot hold a slash and shotOpen only answers to a name it
// already has, so there is nothing here to walk out of the directory on.
void handleShotFile() {
  // Held across the transfer and not merely across the open: screenshots are
  // taken on the other core, and the one being sent is exactly the one an
  // eleventh would evict out from under this handle.
  shotHold();
  bool sent = false;
  File f = shotOpen(server.pathArg(0).c_str());
  if (f) {
    server.streamFile(f, "image/png");
    f.close();
    sent = true;
  }
  shotDrop();
  if (!sent) {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"it has no such screenshot\"}");
  }
}

void handleTrashShot() {
  if (!shotTrash(server.arg("name").c_str())) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"that is not one it has\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// How long after the last request the radio is held awake for. The page asks
// every five seconds while it is open, so this is several of those missed
// before the board decides nobody is there.
constexpr uint32_t AWAKE_MS = 30000;
bool awake = false;
uint32_t askedAt = 0;

void task(void *) {
  server.begin();
  for (;;) {
    server.handleClient();
    if (awake && millis() - askedAt > AWAKE_MS) {
      awake = false;
      wifiAwake(false);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

void portalBegin() {
  store.begin("buddy", false);
  loadTokens();
  Serial.printf("portal: %u token%s in store\n", tokenCount, tokenCount == 1 ? "" : "s");

  // Ahead of every route: somebody is on the page, so the radio stays up for
  // them. The first request still pays for the board having been asleep - the
  // ones behind it do not.
  server.addMiddleware([](WebServer &, Middleware::Callback next) {
    askedAt = millis();
    if (!awake) {
      awake = true;
      wifiAwake(true);
    }
    return next();
  });
  server.on("/", HTTP_GET, handleRoot);
  server.on("/state", HTTP_GET, handleState);
  server.on("/requests", HTTP_GET, handleRequests);
  server.on("/networks", HTTP_POST, handleAddNetwork);
  server.on("/forget", HTTP_POST, handleForgetNetwork);
  server.on("/every", HTTP_POST, handleSetEvery);
  server.on("/tokens", HTTP_POST, handleAddToken);
  server.on("/tokens/trash", HTTP_POST, handleTrashToken);
  server.on("/shots/trash", HTTP_POST, handleTrashShot);
  // Last of the three, and a pattern rather than a path. The two above are
  // exact and are matched first, so neither is read as the name of a file.
  server.on(UriBraces("/shots/{}"), HTTP_GET, handleShotFile);
  server.onNotFound(handleRoot);
  // Core 0, with the radio. Nothing here may sit in the way of a frame. The
  // stack carries a copy of the whole call log on its way out, which is most of
  // two kilobytes that was not here before.
  xTaskCreatePinnedToCore(task, "portal", 12288, nullptr, 1, nullptr, 0);
}

bool portalToken(char *out, size_t size) {
  if (picked >= tokenCount || !tokens[picked][0]) {
    return false;
  }
  snprintf(out, size, "%s", tokens[picked]);
  return true;
}

void portalTurn(int8_t by) {
  int16_t to = (int16_t)picked + by;
  if (to < 0 || to >= tokenCount) {
    return;
  }
  picked = (uint8_t)to;
  store.putUChar("tp", picked);
  Serial.printf("portal: token %u of %u picked\n", picked + 1, tokenCount);
  usageTokenChanged();
}

uint8_t portalPicked() { return tokenCount ? (uint8_t)(picked + 1) : 0; }
