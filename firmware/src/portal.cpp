#include "portal.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <string.h>

#include "net.h"
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
<!-- The panels are a transparent white over this rather than a colour of their
     own, so they lift by the same amount wherever they sit. -->
<script>tailwind.config = {theme: {extend: {colors: {ink: '#0A0B0D'}}}}</script>
<body class="bg-ink text-neutral-200 antialiased">
<main class="max-w-7xl px-8 py-10">
  <h1 class="text-2xl font-bold text-white">Settings</h1>
  <p id=who class="mt-1 hidden text-sm text-red-400"></p>

  <div class="mt-8 grid items-start gap-6 xl:grid-cols-[26rem_minmax(0,1fr)]">
    <div class="grid gap-6">

      <section class="rounded-xl border border-neutral-800 bg-white/[0.02]">
        <div class="flex items-center justify-between border-b border-neutral-800 px-5 py-3">
          <h2 class="text-xs uppercase tracking-wider text-neutral-400">Claude session token</h2>
          <span id=tokenState class="text-xs text-neutral-500">&nbsp;</span>
        </div>
        <form id=tokenForm class="px-5 py-4">
          <input id=token name=token type=password autocomplete=off spellcheck=false
                 placeholder="sk-ant-sid02-..."
                 class="w-full rounded-md border border-neutral-800 bg-ink px-3 py-2 text-sm
                        text-neutral-100 placeholder-neutral-600 outline-none
                        focus:outline focus:outline-[1.5px] focus:outline-offset-0
                        focus:outline-blue-500 disabled:opacity-50">
          <p class="mt-2 text-xs text-neutral-500">Leave empty to clear the token</p>
          <button id=tokenSave data-label="Save token" data-busy="Saving"
                  class="mt-4 inline-flex items-center gap-2 rounded-md bg-blue-600 px-4 py-2
                         text-sm font-medium text-white hover:bg-blue-500
                         disabled:cursor-not-allowed disabled:opacity-50"><span>Save token</span></button>
          <p id=tokenMsg class="mt-3 hidden text-sm"></p>
        </form>
      </section>

      <section class="rounded-xl border border-neutral-800 bg-white/[0.02]">
        <div class="flex items-center justify-between border-b border-neutral-800 px-5 py-3">
          <h2 class="text-xs uppercase tracking-wider text-neutral-400">Wi-Fi networks</h2>
          <span id=netCount class="text-xs text-neutral-500">&nbsp;</span>
        </div>
        <ul id=netList class="divide-y divide-neutral-800"></ul>
        <form id=netForm class="border-t border-neutral-800 px-5 py-4">
          <div class="grid gap-2 sm:grid-cols-2">
            <input id=ssid name=ssid autocomplete=off spellcheck=false maxlength=32
                   placeholder="Network name"
                   class="rounded-md border border-neutral-800 bg-ink px-3 py-2 text-sm
                          text-neutral-100 placeholder-neutral-600 outline-none
                          focus:outline focus:outline-[1.5px] focus:outline-offset-0
                          focus:outline-blue-500 disabled:opacity-50">
            <input id=pass name=password type=password autocomplete=new-password maxlength=63
                   placeholder="Password"
                   class="rounded-md border border-neutral-800 bg-ink px-3 py-2 text-sm
                          text-neutral-100 placeholder-neutral-600 outline-none
                          focus:outline focus:outline-[1.5px] focus:outline-offset-0
                          focus:outline-blue-500 disabled:opacity-50">
          </div>
          <p class="mt-2 text-xs text-neutral-500">
            Kept on the device. A password goes in and is never shown again, here or anywhere.
          </p>
          <button id=netAdd data-label="Add network" data-busy="Adding"
                  class="mt-4 inline-flex items-center gap-2 rounded-md bg-blue-600 px-4 py-2
                         text-sm font-medium text-white hover:bg-blue-500
                         disabled:cursor-not-allowed disabled:opacity-50"><span>Add network</span></button>
          <p id=netMsg class="mt-3 hidden text-sm"></p>
        </form>
      </section>

    </div>

    <section class="overflow-hidden rounded-xl border border-neutral-800 bg-white/[0.02]">
      <div class="border-b border-neutral-800 px-5 py-3">
        <h2 class="text-xs uppercase tracking-wider text-neutral-400">Requests</h2>
      </div>
      <!-- Fixed, so a path longer than the column ends in an ellipsis rather than
           widening the table until the last column is outside the card. -->
      <table class="w-full table-fixed text-xs">
        <thead>
          <tr class="text-[10px] uppercase tracking-wider text-neutral-500">
            <th class="w-20 py-2 pl-5 pr-3 text-left font-normal">Time</th>
            <th class="px-3 py-2 text-left font-normal">Endpoint</th>
            <th class="w-20 px-3 py-2 text-right font-normal">Status</th>
            <th class="w-20 px-3 py-2 text-right font-normal">Took</th>
            <th class="w-20 py-2 pl-3 pr-5 text-right font-normal">Size</th>
          </tr>
        </thead>
        <tbody id=rows class="divide-y divide-neutral-800"></tbody>
      </table>
      <p id=quiet class="px-5 py-6 text-sm text-neutral-500">Nothing asked for yet.</p>
      <div class="flex items-center gap-4 border-t border-neutral-800 px-5 py-3">
        <label for=every
               class="whitespace-nowrap text-[10px] uppercase tracking-wider text-neutral-400">
          Read every
        </label>
        <input id=every type=range min=1 max=5 step=1 value=1
               class="h-1 w-full max-w-xs cursor-pointer appearance-none rounded-full
                      bg-neutral-800 accent-blue-500">
        <span id=everyLabel
              class="w-20 shrink-0 whitespace-nowrap text-xs tabular-nums text-neutral-300">
          &nbsp;
        </span>
      </div>
    </section>
  </div>
</main>

<script>
const $ = (id) => document.getElementById(id);
const who = $('who');

const SPIN = '<svg class="h-4 w-4 animate-spin" viewBox="0 0 24 24" fill=none>' +
  '<circle class="opacity-25" cx=12 cy=12 r=10 stroke=currentColor stroke-width=4></circle>' +
  '<path class="opacity-75" fill=currentColor d="M4 12a8 8 0 0 1 8-8V0C5.4 0 0 5.4 0 12h4z"></path></svg>';

const esc = (s) => String(s).replace(/[&<>"]/g,
  (c) => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
const clock = (at) => (at ? new Date(at * 1000).toISOString().slice(11, 19) : '--:--:--');
const bytes = (n) => (n < 1024 ? n + ' B' : (n / 1024).toFixed(1) + ' KB');

function busy(btn, on) {
  btn.disabled = on;
  btn.innerHTML = (on ? SPIN : '') + '<span>' + (on ? btn.dataset.busy : btn.dataset.label) + '</span>';
}

function say(el, text, ok) {
  el.textContent = text;
  el.className = 'mt-3 text-sm ' + (ok ? 'text-emerald-400' : 'text-red-400');
}

async function post(url, fields) {
  const r = await fetch(url, {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: new URLSearchParams(fields),
  });
  const d = await r.json().catch(() => ({}));
  if (!r.ok || !d.ok) throw new Error(d.error || ('the board answered ' + r.status));
  return d;
}

async function refresh() {
  let s;
  try {
    s = await (await fetch('/state')).json();
  } catch (e) {
    who.textContent = 'The board stopped answering.';
    who.classList.remove('hidden');
    return;
  }
  who.classList.add('hidden');
  $('tokenState').textContent = s.stored ? 'Stored' : 'None set';
  // Not while it is being dragged, or the poll would drag it back every five
  // seconds under the finger holding it.
  if (document.activeElement !== $('every')) {
    $('every').value = s.every;
    showEvery();
  }
}

async function networks() {
  let list;
  try {
    list = await (await fetch('/networks')).json();
  } catch (e) {
    return;
  }
  $('netCount').textContent = list.length + (list.length === 1 ? ' known' : ' known');
  $('netList').innerHTML = list.map((n) => `<li class="flex items-center justify-between gap-3 px-5 py-3">
    <div class="min-w-0">
      <p class="truncate text-sm ${n.connected ? 'text-white' : 'text-neutral-300'}">${esc(n.ssid)}</p>
      ${n.connected ? `<p class="mt-0.5 text-xs text-neutral-500">${esc(n.address)} · ${n.rssi} dBm</p>` : ''}
    </div>
    <div class="flex shrink-0 items-center gap-2">
      ${n.connected ? `<span class="inline-flex items-center gap-1.5 rounded-full bg-emerald-500/10
        px-2 py-0.5 text-[10px] font-medium text-emerald-400"><span
        class="h-1.5 w-1.5 rounded-full bg-emerald-400"></span>Connected</span>` : ''}
      ${n.stored
        ? `<button data-ssid="${esc(n.ssid)}" class="rounded-md border border-neutral-700 px-2.5 py-1
             text-xs text-neutral-400 hover:border-red-500/50 hover:text-red-400">Forget</button>`
        : `<span class="text-[10px] text-neutral-600">Built in</span>`}
    </div>
  </li>`).join('') || '<li class="px-5 py-4 text-sm text-neutral-500">None known.</li>';
}

async function requests() {
  let list;
  try {
    list = await (await fetch('/requests')).json();
  } catch (e) {
    return;
  }
  $('quiet').classList.toggle('hidden', list.length > 0);
  $('rows').innerHTML = list.map((c) => `<tr>
    <td class="whitespace-nowrap py-2 pl-5 pr-3 tabular-nums text-neutral-500">${clock(c.at)}</td>
    <td class="truncate px-3 py-2 text-neutral-400"
        title="${esc(c.url)}">${esc(c.url)}</td>
    <td class="whitespace-nowrap px-3 py-2 text-right tabular-nums ${
      c.code === 200 ? 'text-emerald-400' : 'text-red-400'}">${c.code || 'no reply'}</td>
    <td class="whitespace-nowrap px-3 py-2 text-right tabular-nums text-neutral-500">${c.ms} ms</td>
    <td class="whitespace-nowrap py-2 pl-3 pr-5 text-right tabular-nums
        text-neutral-500">${bytes(c.size)}</td>
  </tr>`).join('');
}

function showEvery() {
  const n = Number($('every').value);
  $('everyLabel').textContent = n + (n === 1 ? ' minute' : ' minutes');
}

$('every').addEventListener('input', showEvery);

// On change rather than input: the step snaps it, and one write per drag beats
// one per position of it.
$('every').addEventListener('change', async () => {
  try {
    await post('/every', {minutes: $('every').value});
  } catch (err) {
    say($('netMsg'), 'Interval not saved: ' + err.message, false);
  }
});

$('tokenForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const btn = $('tokenSave');
  busy(btn, true);
  $('tokenMsg').classList.add('hidden');
  try {
    const d = await post('/token', {token: $('token').value});
    say($('tokenMsg'), d.stored ? 'Saved. The board is using it now.' : 'Forgotten.', true);
    $('token').value = '';
  } catch (err) {
    say($('tokenMsg'), 'Not saved: ' + err.message, false);
  } finally {
    busy(btn, false);
    refresh();
  }
});

$('netForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const btn = $('netAdd');
  busy(btn, true);
  $('netMsg').classList.add('hidden');
  try {
    await post('/networks', {ssid: $('ssid').value, password: $('pass').value});
    say($('netMsg'), 'Added. The board will try it when it next looks.', true);
    $('ssid').value = $('pass').value = '';
  } catch (err) {
    say($('netMsg'), 'Not added: ' + err.message, false);
  } finally {
    busy(btn, false);
    networks();
  }
});

$('netList').addEventListener('click', async (e) => {
  const btn = e.target.closest('button[data-ssid]');
  if (!btn) return;
  const ssid = btn.dataset.ssid;
  // Forgetting the one the board is on takes this page down with it, and the
  // password is gone rather than hidden - both are worth a second's thought.
  if (!confirm('Forget "' + ssid + '"? Its password is deleted with it.')) return;
  btn.disabled = true;
  btn.textContent = '...';
  try {
    await post('/forget', {ssid: ssid});
    $('netMsg').classList.add('hidden');
  } catch (err) {
    say($('netMsg'), 'Not forgotten: ' + err.message, false);
  }
  networks();
});

function tick() {
  refresh();
  networks();
  requests();
}
tick();
// None of this changes because the page did it: another tab, the board joining
// a different network, or the poller going round again a minute from now.
setInterval(tick, 5000);
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
  json += ",\"address\":";
  appendQuoted(json, wifiAddress() ? wifiAddress() : "");
  json += ",\"every\":";
  json += usageEvery();
  json += "}";
  server.send(200, "application/json", json);
}

// Names only. There is no accessor for a stored password and this is the reason
// there is not: the page that would show one is this one.
void handleNetworks() {
  const char *on = wifiNetwork();
  String json = "[";
  for (uint8_t i = 0; i < wifiKnown(); i++) {
    const char *ssid = wifiKnownSsid(i);
    if (!ssid) {
      continue;
    }
    if (json.length() > 1) {
      json += ',';
    }
    json += "{\"ssid\":";
    appendQuoted(json, ssid);
    json += ",\"stored\":";
    json += wifiKnownStored(i) ? "true" : "false";
    bool live = on && strcmp(on, ssid) == 0;
    json += ",\"connected\":";
    json += live ? "true" : "false";
    if (live) {
      json += ",\"address\":";
      appendQuoted(json, wifiAddress() ? wifiAddress() : "");
      json += ",\"rssi\":";
      json += wifiRssi();
    }
    json += '}';
  }
  json += ']';
  server.send(200, "application/json", json);
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
                "{\"ok\":false,\"error\":\"that is not one of the stored ones\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRequests() {
  NetCall calls[NET_CALLS];
  uint8_t n = netCalls(calls, NET_CALLS);
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
    json += ",\"url\":";
    appendQuoted(json, calls[i].url);
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
  server.on("/networks", HTTP_GET, handleNetworks);
  server.on("/networks", HTTP_POST, handleAddNetwork);
  server.on("/forget", HTTP_POST, handleForgetNetwork);
  server.on("/every", HTTP_POST, handleSetEvery);
  server.on("/token", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  // Core 0, with the radio. Nothing here may sit in the way of a frame. The
  // stack carries a copy of the whole call log on its way out, which is most of
  // two kilobytes that was not here before.
  xTaskCreatePinnedToCore(task, "portal", 12288, nullptr, 1, nullptr, 0);
}

const char *portalToken() { return token[0] ? token : nullptr; }
