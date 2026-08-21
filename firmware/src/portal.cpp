#include "portal.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>

#include "usage.h"
#include "wifi.h"

namespace {

constexpr uint16_t PORT = 80;
constexpr size_t TOKEN_MAX = 512;

Preferences store;
WebServer server(PORT);
char token[TOKEN_MAX] = {0};

const char *PAGE_HEAD =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>buddy</title><style>"
    "body{background:#111;color:#eee;font:15px/1.5 system-ui,sans-serif;margin:0;"
    "display:grid;place-items:center;min-height:100vh}"
    "form{width:min(92vw,34rem);padding:2rem}"
    "h1{font-size:1.1rem;font-weight:600;margin:0 0 .25rem}"
    "p{color:#888;margin:0 0 1.5rem}"
    "input{width:100%;box-sizing:border-box;background:#1c1c1c;border:1px solid #333;"
    "color:#eee;border-radius:.5rem;padding:.75rem;font:inherit}"
    "button{margin-top:1rem;background:#eee;color:#111;border:0;border-radius:.5rem;"
    "padding:.75rem 1.25rem;font:inherit;font-weight:600;cursor:pointer}"
    "b{color:#4ade80}i{color:#888;font-style:normal}"
    "</style><form method=post action=/token>"
    "<h1>claude session token</h1>";

void handleRoot() {
  String page = PAGE_HEAD;
  page += "<p>";
  if (token[0]) {
    page += "stored, ";
    page += usageReady() ? "<b>reading your usage</b>" : "<i>not answering yet</i>";
  } else {
    page += "<i>none set</i>";
  }
  page += "</p><input name=token autocomplete=off spellcheck=false placeholder='sk-ant-sid02-...'>";
  page += "<button>save</button></form>";
  server.send(200, "text/html", page);
}

void handleSave() {
  String given = server.arg("token");
  given.trim();
  strncpy(token, given.c_str(), TOKEN_MAX - 1);
  token[TOKEN_MAX - 1] = '\0';
  store.putString("token", token);
  Serial.printf("portal: token %s, %u chars\n", token[0] ? "set" : "cleared", strlen(token));
  usageWake();
  server.sendHeader("Location", "/");
  server.send(303);
}

void task(void *) {
  server.begin();
  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

void portalBegin() {
  store.begin("buddy", false);
  store.getString("token", token, TOKEN_MAX);
  Serial.printf("portal: %s\n", token[0] ? "token in store" : "no token yet");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/token", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  // Core 0, with the radio. Nothing here may sit in the way of a frame.
  xTaskCreatePinnedToCore(task, "portal", 8192, nullptr, 1, nullptr, 0);
}

const char *portalToken() { return token[0] ? token : nullptr; }
