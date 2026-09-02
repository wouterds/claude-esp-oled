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

// The whole page, and it never changes: what is on it comes from /state and
// goes back through /token, so the board serves one string out of flash rather
// than building a new one per request on the core the radio is on.
//
// Tailwind off a CDN, which the board never fetches - the browser does, and the
// browser is the thing on this network with a way out of it. The alternative is
// a stylesheet in the firmware image, paid for on every flash.
const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang=en>
<meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>buddy</title>
<script src="https://cdn.tailwindcss.com"></script>
<body class="bg-neutral-950 text-neutral-200 antialiased">
<main class="max-w-7xl px-8 py-10">
  <h1 class="text-2xl font-bold text-white">Settings</h1>
  <p id=who class="mt-1 text-sm text-neutral-500">&nbsp;</p>

  <div class="mt-8 grid items-start gap-6 lg:grid-cols-[24rem_minmax(0,1fr)]">
  <section class="rounded-xl border border-neutral-800 bg-neutral-900/40">
    <form id=form class="px-6 py-5">
      <label for=token class="block text-sm text-neutral-300">Claude session token</label>
      <div class="mt-2 flex flex-col gap-3 sm:flex-row sm:items-center">
        <input id=token name=token type=password autocomplete=off spellcheck=false
               placeholder="sk-ant-sid02-..."
               class="w-full rounded-md border border-neutral-800 bg-neutral-950 px-3 py-2 text-sm
                      text-neutral-100 placeholder-neutral-600 outline-none
                      focus:outline focus:outline-[1.5px] focus:outline-offset-0
                      focus:outline-blue-500 disabled:opacity-50 sm:max-w-sm">
        <p id=state class="text-sm text-neutral-500">&nbsp;</p>
      </div>
      <p class="mt-2 text-sm text-neutral-500">Leave it empty to clear the token</p>

      <div class="mt-5 flex items-center gap-3">
        <button id=save
                class="inline-flex items-center gap-2 rounded-md bg-blue-500 px-4 py-2 text-sm
                       font-medium text-white hover:bg-blue-400 disabled:cursor-not-allowed
                       disabled:opacity-50">
          <svg id=spin class="hidden h-4 w-4 animate-spin" viewBox="0 0 24 24" fill=none>
            <circle class="opacity-25" cx=12 cy=12 r=10 stroke=currentColor stroke-width=4></circle>
            <path class="opacity-75" fill=currentColor
                  d="M4 12a8 8 0 0 1 8-8V0C5.4 0 0 5.4 0 12h4z"></path>
          </svg>
          <span id=label>Save token</span>
        </button>
      </div>

      <p id=msg class="mt-4 hidden text-sm"></p>
    </form>
  </section>

  <section class="overflow-hidden rounded-xl border border-neutral-800 bg-neutral-900/40">
    <div class="flex items-baseline justify-between border-b border-neutral-800 px-5 py-3">
      <h2 class="text-xs uppercase tracking-wider text-neutral-400">Requests</h2>
      <p id=meta class="text-xs text-neutral-500">&nbsp;</p>
    </div>
    <table class="w-full text-sm">
      <thead>
        <tr class="text-xs uppercase tracking-wider text-neutral-500">
          <th class="px-5 py-2 text-left font-normal">Time</th>
          <th class="px-5 py-2 text-left font-normal">Endpoint</th>
          <th class="px-5 py-2 text-right font-normal">Status</th>
          <th class="px-5 py-2 text-right font-normal">Took</th>
          <th class="px-5 py-2 text-right font-normal">Size</th>
        </tr>
      </thead>
      <tbody id=rows class="divide-y divide-neutral-800"></tbody>
    </table>
    <p id=quiet class="px-5 py-6 text-sm text-neutral-500">Nothing asked for yet.</p>
  </section>
  </div>
</main>

<script>
const form = document.getElementById('form');
const field = document.getElementById('token');
const save = document.getElementById('save');
const spin = document.getElementById('spin');
const label = document.getElementById('label');
const msg = document.getElementById('msg');
const state = document.getElementById('state');
const who = document.getElementById('who');
const meta = document.getElementById('meta');
const rows = document.getElementById('rows');
const quiet = document.getElementById('quiet');

// The board keeps only the path, because it only ever asks the one host - so
// the host is written here as well rather than sent back thirty times over.
const HOST = 'https://claude.ai';

const esc = (s) => s.replace(/[&<>]/g, (c) => ({'&': '&amp;', '<': '&lt;', '>': '&gt;'}[c]));
const clock = (at) => (at ? new Date(at * 1000).toISOString().slice(11, 19) : '--:--:--');
const bytes = (n) => (n < 1024 ? n + ' B' : (n / 1024).toFixed(1) + ' KB');

async function requests() {
  let list;
  try {
    list = await (await fetch('/requests')).json();
  } catch (e) {
    return;
  }
  quiet.classList.toggle('hidden', list.length > 0);
  meta.textContent = HOST + ' · ' + list.length + (list.length === 1 ? ' call' : ' calls') + ', UTC';
  rows.innerHTML = list.map((c) => `<tr>
    <td class="whitespace-nowrap px-5 py-2.5 tabular-nums text-neutral-400">${clock(c.at)}</td>
    <td class="whitespace-nowrap px-5 py-2.5 text-neutral-200"><span
        class="text-neutral-600">${HOST}</span>${esc(c.path)}</td>
    <td class="px-5 py-2.5 text-right tabular-nums ${
      c.code === 200 ? 'text-emerald-400' : 'text-red-400'}">${c.code || 'no reply'}</td>
    <td class="px-5 py-2.5 text-right tabular-nums text-neutral-400">${c.ms} ms</td>
    <td class="px-5 py-2.5 text-right tabular-nums text-neutral-400">${bytes(c.size)}</td>
  </tr>`).join('');
}

function say(text, ok) {
  msg.textContent = text;
  msg.className = 'mt-4 text-sm ' + (ok ? 'text-emerald-400' : 'text-red-400');
}

async function refresh() {
  try {
    const s = await (await fetch('/state')).json();
    who.textContent = s.network ? s.network + ' · ' + s.address : 'not on a network';
    state.textContent = s.stored ? 'Stored.' : 'None set.';
  } catch (e) {
    state.textContent = 'The board stopped answering.';
  }
}

form.addEventListener('submit', async (e) => {
  e.preventDefault();
  save.disabled = field.disabled = true;
  spin.classList.remove('hidden');
  label.textContent = 'Saving';
  msg.classList.add('hidden');
  try {
    const r = await fetch('/token', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: new URLSearchParams({token: field.value}),
    });
    const d = await r.json().catch(() => ({}));
    if (!r.ok || !d.ok) throw new Error(d.error || ('the board answered ' + r.status));
    say(d.stored ? 'Saved. The board is using it now.' : 'Forgotten.', true);
    field.value = '';
  } catch (err) {
    say('Not saved: ' + err.message, false);
  } finally {
    save.disabled = field.disabled = false;
    spin.classList.add('hidden');
    label.textContent = 'Save token';
    refresh();
  }
});

refresh();
requests();
// The network line and whether anything is stored can both change without this
// page having done it - another tab, or the board joining a different network.
setInterval(() => {
  refresh();
  requests();
}, 5000);
</script>
)HTML";

void handleRoot() { server.send_P(200, PSTR("text/html"), PAGE); }

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

void handleState() {
  String json = "{\"stored\":";
  json += token[0] ? "true" : "false";
  json += ",\"network\":";
  if (wifiNetwork()) {
    appendQuoted(json, wifiNetwork());
  } else {
    json += "null";
  }
  json += ",\"address\":";
  appendQuoted(json, wifiAddress() ? wifiAddress() : "");
  json += "}";
  server.send(200, "application/json", json);
}

void handleRequests() {
  UsageCall calls[USAGE_CALLS];
  uint8_t n = usageCalls(calls, USAGE_CALLS);
  String json = "[";
  for (uint8_t i = 0; i < n; i++) {
    if (i) {
      json += ',';
    }
    json += "{\"at\":";
    json += calls[i].at;
    json += ",\"ms\":";
    json += calls[i].ms;
    json += ",\"code\":";
    json += calls[i].code;
    json += ",\"size\":";
    json += calls[i].size;
    json += ",\"path\":";
    appendQuoted(json, calls[i].path);
    json += '}';
  }
  json += ']';
  server.send(200, "application/json", json);
}

void handleSave() {
  String given = server.arg("token");
  given.trim();
  if (given.length() >= TOKEN_MAX) {
    server.send(413, "application/json",
                "{\"ok\":false,\"error\":\"that is longer than the store will take\"}");
    return;
  }
  // The store first and the copy in RAM only if it took: a write that failed
  // would otherwise leave the board using a token the page has just been told
  // it does not have. Nought written is the refusal - and is also what clearing
  // it reads as, which is why the empty case is not asked.
  size_t wrote = store.putString("token", given.c_str());
  if (wrote == 0 && given.length() > 0) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"the flash would not take it\"}");
    return;
  }
  strncpy(token, given.c_str(), TOKEN_MAX - 1);
  token[TOKEN_MAX - 1] = '\0';
  Serial.printf("portal: token %s, %u chars\n", token[0] ? "set" : "cleared", strlen(token));
  usageWake();

  char json[48];
  snprintf(json, sizeof(json), "{\"ok\":true,\"stored\":%s}", token[0] ? "true" : "false");
  server.send(200, "application/json", json);
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
  server.on("/state", HTTP_GET, handleState);
  server.on("/requests", HTTP_GET, handleRequests);
  server.on("/token", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  // Core 0, with the radio. Nothing here may sit in the way of a frame. The
  // stack carries a copy of the whole call log on its way out, which is most of
  // two kilobytes that was not here before.
  xTaskCreatePinnedToCore(task, "portal", 12288, nullptr, 1, nullptr, 0);
}

const char *portalToken() { return token[0] ? token : nullptr; }
