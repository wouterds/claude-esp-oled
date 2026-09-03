#include "portal.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <string.h>
#include <uri/UriBraces.h>

#include "net.h"
#include "shot.h"
#include "usage.h"
#include "wifi.h"

namespace {

constexpr uint16_t PORT = 80;
constexpr size_t TOKEN_MAX = 512;
// One per account somebody wants to watch. Held in RAM rather than read out of
// the store per request, which is half a kilobyte each and the reason there are
// four of them rather than a list that grows.
constexpr uint8_t TOKEN_SLOTS = 4;

Preferences store;
WebServer server(PORT);
char tokens[TOKEN_SLOTS][TOKEN_MAX] = {{0}};
uint8_t tokenCount = 0;
// Which one the requests go out with. Always a real slot while there is one.
uint8_t picked = 0;

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
<body class="bg-gray-950 text-white/90 antialiased">
<main class="max-w-7xl px-8 py-10">
  <h1 class="text-2xl font-bold text-white">Settings</h1>
  <p id=who class="mt-1 hidden text-sm text-rose-400"></p>

  <div class="mt-8 grid items-start gap-6 xl:grid-cols-[26rem_minmax(0,1fr)]">
    <div class="grid gap-6">

      <section class="rounded-xl border border-white/10 bg-white/[0.05]">
        <div class="flex items-center justify-between border-b border-white/[0.06] px-5 py-3">
          <h2 class="text-xs uppercase tracking-wider text-white/60">Claude session tokens</h2>
          <span id=tokenState class="text-xs text-white/45">&nbsp;</span>
        </div>
        <ul id=tokenList class="divide-y divide-white/[0.06]"></ul>
        <form id=tokenForm class="border-t border-white/[0.06] px-5 py-4">
          <input id=token name=token type=password autocomplete=off spellcheck=false
                 placeholder="sk-ant-sid02-..."
                 class="w-full rounded-md border border-white/10 bg-gray-950 px-3 py-2 text-sm
                        text-white/95 placeholder-white/30 outline-none
                        focus:outline focus:outline-[1.5px] focus:outline-offset-0
                        focus:outline-blue-500 disabled:opacity-50">
          <p class="mt-2 text-xs text-white/45">
            Kept on the device. A new one is used straight away; it goes in and is never shown
            again, here or anywhere.
          </p>
          <button id=tokenAdd data-label="Add token" data-busy="Adding"
                  class="mt-4 inline-flex items-center gap-2 rounded-md bg-blue-600 px-4 py-2
                         text-sm font-medium text-white hover:bg-blue-600/85
                         disabled:cursor-not-allowed disabled:opacity-50"><span>Add token</span></button>
          <p id=tokenMsg class="mt-3 hidden text-sm"></p>
        </form>
      </section>

      <section class="rounded-xl border border-white/10 bg-white/[0.05]">
        <div class="flex items-center justify-between border-b border-white/[0.06] px-5 py-3">
          <h2 class="text-xs uppercase tracking-wider text-white/60">Wi-Fi networks</h2>
          <span id=netCount class="text-xs text-white/45">&nbsp;</span>
        </div>
        <ul id=netList class="divide-y divide-white/[0.06]"></ul>
        <form id=netForm class="border-t border-white/[0.06] px-5 py-4">
          <div class="grid gap-2 sm:grid-cols-2">
            <input id=ssid name=ssid autocomplete=off spellcheck=false maxlength=32
                   placeholder="Network name"
                   class="rounded-md border border-white/10 bg-gray-950 px-3 py-2 text-sm
                          text-white/95 placeholder-white/30 outline-none
                          focus:outline focus:outline-[1.5px] focus:outline-offset-0
                          focus:outline-blue-500 disabled:opacity-50">
            <input id=pass name=password type=password autocomplete=new-password maxlength=63
                   placeholder="Password"
                   class="rounded-md border border-white/10 bg-gray-950 px-3 py-2 text-sm
                          text-white/95 placeholder-white/30 outline-none
                          focus:outline focus:outline-[1.5px] focus:outline-offset-0
                          focus:outline-blue-500 disabled:opacity-50">
          </div>
          <p class="mt-2 text-xs text-white/45">
            Kept on the device. A password goes in and is never shown again, here or anywhere.
          </p>
          <button id=netAdd data-label="Add network" data-busy="Adding"
                  class="mt-4 inline-flex items-center gap-2 rounded-md bg-blue-600 px-4 py-2
                         text-sm font-medium text-white hover:bg-blue-600/85
                         disabled:cursor-not-allowed disabled:opacity-50"><span>Add network</span></button>
          <p id=netMsg class="mt-3 hidden text-sm"></p>
        </form>
      </section>

    </div>

    <!-- The column, rather than the one card that used to be here. The cap on
         how tall it gets moved up to this: the screenshots take what they need
         and the requests take the rest, so the two together stop where the one
         did instead of running the page off the bottom. -->
    <div class="grid min-w-0 gap-6 xl:max-h-[calc(100vh-10rem)]
                xl:grid-rows-[auto_minmax(0,1fr)]">

      <section class="rounded-xl border border-white/10 bg-white/[0.05]">
        <div class="flex items-center justify-between border-b border-white/[0.06] px-5 py-3">
          <h2 class="text-xs uppercase tracking-wider text-white/60">Screenshots</h2>
          <span id=shotCount class="text-xs text-white/45">&nbsp;</span>
        </div>
        <!-- Capped and scrolling rather than as tall as it needs. Ten rows is
             most of the column on a laptop, and what it would be taking that
             from is the request log underneath - which is the thing on this
             page somebody is actually reading. The cap cuts a row in half on
             purpose: a list that ends flush with its own border looks like the
             whole of it. -->
        <ul id=shotList class="max-h-72 divide-y divide-white/[0.06] overflow-y-auto"></ul>
      </section>

      <section class="flex max-h-[200px] min-h-0 flex-col overflow-hidden rounded-xl
                      border border-white/10 bg-white/[0.05] xl:max-h-none">
        <div class="flex flex-none flex-wrap items-center justify-between gap-x-4 gap-y-2
                    border-b border-white/[0.06] px-5 py-3">
          <h2 class="text-xs uppercase tracking-wider text-white/60">Requests</h2>
          <div class="flex items-center gap-3">
            <label for=every class="text-xs text-white/45">Refresh interval</label>
            <input id=every type=range min=1 max=5 step=1 value=1
                   class="h-1 w-28 cursor-pointer appearance-none rounded-full bg-white/10
                          accent-blue-500">
            <span id=everyLabel
                  class="w-12 shrink-0 text-right text-xs tabular-nums text-white/80">&nbsp;</span>
          </div>
        </div>
        <!-- The rows scroll and the two ends hold still. They stay inside the one
             table so the columns cannot drift apart, which is what a second table
             for the head would risk the moment a scrollbar took a width off this
             one. A stuck row has to be opaque or the rows pass through it, and
             #10131E is this panel's own colour resolved - the one fixed value on
             the page, and only because there is no way to name a composite. -->
        <div class="min-h-0 flex-1 overflow-y-auto">
          <!-- Fixed, so a path longer than the column ends in an ellipsis rather
               than widening the table until the last column is outside the card. -->
          <table class="w-full table-fixed text-xs">
            <thead>
              <tr class="text-[10px] uppercase tracking-wider text-white/45">
                <th class="sticky top-0 z-10 w-20 border-b border-white/[0.06] bg-[#10131E] py-2
                           pl-5 pr-3 text-left font-normal">Time</th>
                <th class="sticky top-0 z-10 border-b border-white/[0.06] bg-[#10131E] px-3 py-2
                           text-left font-normal">Endpoint</th>
                <th class="sticky top-0 z-10 w-20 border-b border-white/[0.06] bg-[#10131E] px-3
                           py-2 text-right font-normal">Status</th>
                <th class="sticky top-0 z-10 w-20 border-b border-white/[0.06] bg-[#10131E] px-3
                           py-2 text-right font-normal">Took</th>
                <th class="sticky top-0 z-10 w-20 border-b border-white/[0.06] bg-[#10131E] py-2
                           pl-3 pr-5 text-right font-normal">Size</th>
              </tr>
            </thead>
            <tbody id=rows class="divide-y divide-white/[0.06]"></tbody>
            <tfoot id=totals class="hidden">
              <tr class="text-white/45">
                <td colspan=3 id=avgLabel
                    class="sticky bottom-0 z-10 border-t border-white/[0.06] bg-[#10131E] py-2 pl-5
                           pr-3 text-[10px] uppercase tracking-wider"></td>
                <td id=avgMs class="sticky bottom-0 z-10 whitespace-nowrap border-t
                           border-white/[0.06] bg-[#10131E] px-3 py-2 text-right tabular-nums"></td>
                <td id=avgSize class="sticky bottom-0 z-10 whitespace-nowrap border-t
                           border-white/[0.06] bg-[#10131E] py-2 pl-3 pr-5 text-right
                           tabular-nums"></td>
              </tr>
            </tfoot>
          </table>
          <p id=quiet class="px-5 py-6 text-sm text-white/45">Nothing asked for yet.</p>
        </div>
      </section>

    </div>
  </div>
</main>

<script>
const $ = (id) => document.getElementById(id);
const who = $('who');

const spin = (size) => '<svg class="' + size + ' animate-spin" viewBox="0 0 24 24" fill=none>' +
  '<circle class="opacity-25" cx=12 cy=12 r=10 stroke=currentColor stroke-width=4></circle>' +
  '<path class="opacity-75" fill=currentColor d="M4 12a8 8 0 0 1 8-8V0C5.4 0 0 5.4 0 12h4z"></path></svg>';

const esc = (s) => String(s).replace(/[&<>"]/g,
  (c) => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
const clock = (at) => (at ? new Date(at * 1000).toISOString().slice(11, 19) : '--:--:--');
const bytes = (n) => (n < 1024 ? n + ' B' : (n / 1024).toFixed(1) + ' KB');

function busy(btn, on) {
  btn.disabled = on;
  btn.innerHTML =
    (on ? spin('h-4 w-4') : '') + '<span>' + (on ? btn.dataset.busy : btn.dataset.label) + '</span>';
}

// Said and then taken back: the answer is about the press that caused it, and
// left up it goes on describing a page that has moved on since.
const SAID_MS = 5000;
const fading = new WeakMap();

function say(el, text, ok) {
  el.textContent = text;
  el.className = 'mt-3 text-sm ' + (ok ? 'text-teal-400' : 'text-rose-400');
  // Restarted rather than stacked, so a second message gets its own five
  // seconds instead of inheriting what is left of the first one's.
  clearTimeout(fading.get(el));
  fading.set(el, setTimeout(() => el.classList.add('hidden'), SAID_MS));
}

// Blurred the moment a form is sent: the answer appears under the field, and a
// field still holding focus with a ring around it reads as still wanting
// something typed. It also puts a phone's keyboard away.
const unfocus = (form) => form.querySelectorAll('input').forEach((i) => i.blur());

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
  if (s.calls !== drawn) {
    drawn = s.calls;
    requests();
  }
  // Not while it is being dragged, or the poll would drag it back every five
  // seconds under the finger holding it.
  if (document.activeElement !== $('every')) {
    $('every').value = s.every;
    showEvery();
  }
}

// A press in flight owns its list: the five second tick would otherwise redraw
// it underneath the button and replace the one that is spinning with a fresh
// one, which reads as the click having been dropped.
let forgetting = false;
let trashing = false;
let binning = false;

async function tokens() {
  if (trashing) {
    return;
  }
  let list;
  try {
    list = await (await fetch('/tokens')).json();
  } catch (e) {
    return;
  }
  $('tokenState').textContent = list.length ? list.length + ' stored' : 'None set';
  // The radio is drawn rather than accented: accent-color fills the whole disc
  // and puts a white dot in it, which against a dark page reads as a blob and
  // not as a ring with a chosen one inside it.
  $('tokenList').innerHTML = list.map((t, i) => `<li class="flex items-center justify-between
    gap-3 px-5 py-3">
    <label class="flex min-w-0 cursor-pointer items-center gap-3">
      <input type=radio name=pick value=${i} ${t.picked ? 'checked' : ''}
             class="h-4 w-4 shrink-0 cursor-pointer appearance-none rounded-full border-2
                    border-white/25 text-blue-500 checked:border-blue-500 checked:bg-[radial-gradient(circle_at_center,currentColor_0_3px,transparent_3.5px)]">
      <span class="truncate font-mono text-xs ${t.picked ? 'text-white' : 'text-white/60'}"
        >${esc(t.hint)}</span>
    </label>
    <button data-token=${i} class="inline-flex shrink-0 items-center gap-1.5 rounded-md
      border border-white/15 px-2.5 py-1 text-xs text-white/60 hover:border-rose-500/50
      hover:text-rose-400 disabled:cursor-not-allowed disabled:opacity-50">Remove</button>
  </li>`).join('') || '<li class="px-5 py-4 text-sm text-white/45">None set.</li>';
}

async function networks() {
  if (forgetting) {
    return;
  }
  let list;
  try {
    list = await (await fetch('/networks')).json();
  } catch (e) {
    return;
  }
  $('netCount').textContent = list.length + (list.length === 1 ? ' known' : ' known');
  $('netList').innerHTML = list.map((n) => `<li class="flex items-center justify-between gap-3 px-5 py-3">
    <div class="min-w-0">
      <p class="truncate text-sm ${n.connected ? 'text-white' : 'text-white/80'}">${esc(n.ssid)}</p>
      ${n.connected ? `<p class="mt-0.5 text-xs text-white/45">${esc(n.address)} · ${n.rssi} dBm</p>` : ''}
    </div>
    <div class="flex shrink-0 items-center gap-2">
      ${n.connected ? `<span class="inline-flex items-center gap-1.5 rounded-full bg-teal-500/10
        px-2 py-0.5 text-[10px] font-medium text-teal-400"><span
        class="h-1.5 w-1.5 rounded-full bg-teal-400"></span>Connected</span>` : ''}
      <button data-ssid="${esc(n.ssid)}" class="inline-flex items-center gap-1.5 rounded-md
        border border-white/15 px-2.5 py-1 text-xs text-white/60
        hover:border-rose-500/50 hover:text-rose-400 disabled:cursor-not-allowed
        disabled:opacity-50">Remove</button>
    </div>
  </li>`).join('') || '<li class="px-5 py-4 text-sm text-white/45">None known.</li>';
}

async function shots() {
  if (binning) {
    return;
  }
  let list;
  try {
    list = await (await fetch('/shots')).json();
  } catch (e) {
    return;
  }
  $('shotCount').textContent = list.length ? list.length + ' kept' : 'None yet';
  // The name is the link. It goes to its own path rather than to a query, so
  // the tab it opens is called what the file is called and saving it out of the
  // browser keeps that name.
  $('shotList').innerHTML = list.map((s) => `<li class="flex items-center justify-between gap-3
    px-5 py-3">
    <a href="/shots/${esc(s.name)}" target=_blank rel="noopener noreferrer"
       class="min-w-0 truncate font-mono text-xs text-white/80 hover:text-white hover:underline"
      >${esc(s.name)}</a>
    <div class="flex shrink-0 items-center gap-3">
      <span class="text-xs tabular-nums text-white/45">${bytes(s.size)}</span>
      <button data-shot="${esc(s.name)}" class="inline-flex items-center gap-1.5 rounded-md
        border border-white/15 px-2.5 py-1 text-xs text-white/60 hover:border-rose-500/50
        hover:text-rose-400 disabled:cursor-not-allowed disabled:opacity-50">Remove</button>
    </div>
  </li>`).join('') || '<li class="px-5 py-4 text-sm text-white/45">None yet. Hold the button on ' +
    'the board for three seconds.</li>';
}

// What the board had made last time the rows were drawn. A thousand of them is
// a hundred kilobytes of JSON and as many table rows to build, and none of it
// changes between calls - so the rows are fetched when that number moves rather
// than on the clock.
let drawn = -1;

async function requests() {
  let list;
  try {
    list = await (await fetch('/requests')).json();
  } catch (e) {
    return;
  }
  // Held across the redraw, or reading back through the log would throw
  // somebody to the top every time the board made a call.
  const box = $('rows').closest('div');
  const wasAt = box.scrollTop;
  $('quiet').classList.toggle('hidden', list.length > 0);
  // Only the ones that came back with something. A call that failed has no size
  // to speak of and averaging its nought in says the replies are smaller than
  // any of them was.
  const done = list.filter((c) => c.code === 200);
  $('totals').classList.toggle('hidden', done.length === 0);
  if (done.length) {
    const avg = (of) => done.reduce((sum, c) => sum + of(c), 0) / done.length;
    $('avgLabel').textContent =
      'Average of ' + done.length + (done.length === 1 ? ' response' : ' responses');
    $('avgMs').textContent = Math.round(avg((c) => c.ms)) + ' ms';
    $('avgSize').textContent = bytes(Math.round(avg((c) => c.size)));
  }
  $('rows').innerHTML = list.map((c) => `<tr>
    <td class="whitespace-nowrap py-2 pl-5 pr-3 tabular-nums text-white/45">${clock(c.at)}</td>
    <td class="truncate px-3 py-2"><a href="${esc(c.url)}" target=_blank
        rel="noopener noreferrer" title="${esc(c.url)}"
        class="text-white/60 hover:underline">${esc(c.url)}</a></td>
    <td class="whitespace-nowrap px-3 py-2 text-right ${
      c.code === 200 ? 'tabular-nums text-teal-400' : 'text-rose-400'}"
        title="${c.code > 0 ? 'HTTP ' + c.code : 'client error ' + c.code}">${
      c.code > 0 ? c.code : 'failed'}</td>
    <td class="whitespace-nowrap px-3 py-2 text-right tabular-nums text-white/45">${c.ms} ms</td>
    <td class="whitespace-nowrap py-2 pl-3 pr-5 text-right tabular-nums
        text-white/45">${bytes(c.size)}</td>
  </tr>`).join('');
  box.scrollTop = wasAt;
}

function showEvery() {
  $('everyLabel').textContent = $('every').value + ' min';
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
  unfocus(e.target);
  const btn = $('tokenAdd');
  busy(btn, true);
  $('tokenMsg').classList.add('hidden');
  try {
    await post('/tokens', {token: $('token').value});
    say($('tokenMsg'), 'Added. The board is using it now.', true);
    $('token').value = '';
  } catch (err) {
    say($('tokenMsg'), 'Not added: ' + err.message, false);
  } finally {
    busy(btn, false);
    // Everything, not just the token's own list: adding one sets the poller off
    // and what that turns up belongs on the page now rather than in five
    // seconds' time.
    tick();
  }
});

$('tokenList').addEventListener('change', async (e) => {
  const radio = e.target.closest('input[name=pick]');
  if (!radio) return;
  try {
    await post('/tokens/pick', {index: radio.value});
    $('tokenMsg').classList.add('hidden');
  } catch (err) {
    say($('tokenMsg'), 'Not picked: ' + err.message, false);
  }
  tick();
});

$('tokenList').addEventListener('click', async (e) => {
  const btn = e.target.closest('button[data-token]');
  if (!btn) return;
  // A token cannot be read back off the board, so there is no getting this one
  // again without going to claude.ai for a fresh one.
  if (!confirm('Are you sure you want to remove this token? It cannot be shown again.')) return;
  btn.disabled = true;
  btn.innerHTML = spin('h-3 w-3') + '<span>Removing</span>';
  trashing = true;
  try {
    await post('/tokens/trash', {index: btn.dataset.token});
    $('tokenMsg').classList.add('hidden');
  } catch (err) {
    say($('tokenMsg'), 'Not removed: ' + err.message, false);
  } finally {
    trashing = false;
  }
  // Awaited, so the button spins until the row it sits in has actually gone
  // rather than until the board said it would.
  await tokens();
  refresh();
});

$('netForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  unfocus(e.target);
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
    tick();
  }
});

$('netList').addEventListener('click', async (e) => {
  const btn = e.target.closest('button[data-ssid]');
  if (!btn) return;
  const ssid = btn.dataset.ssid;
  // Removing the one the board is on takes this page down with it, and the
  // password goes rather than being hidden - both are worth a second's thought.
  const sure = 'Are you sure you want to remove the Wi-Fi network "' + ssid +
      '"? You will need its password to add it back.';
  if (!confirm(sure)) return;
  btn.disabled = true;
  btn.innerHTML = spin('h-3 w-3') + '<span>Removing</span>';
  forgetting = true;
  try {
    await post('/forget', {ssid: ssid});
    $('netMsg').classList.add('hidden');
  } catch (err) {
    say($('netMsg'), 'Not removed: ' + err.message, false);
  } finally {
    forgetting = false;
  }
  // Awaited, so the button spins until the row it sits in has actually gone
  // rather than until the board said it would. A refusal redraws it as it was.
  await networks();
  refresh();
});

$('shotList').addEventListener('click', async (e) => {
  const btn = e.target.closest('button[data-shot]');
  if (!btn) return;
  // The frame it was taken of is long gone, so this is the only copy of it.
  if (!confirm('Are you sure you want to remove ' + btn.dataset.shot + '?')) return;
  btn.disabled = true;
  btn.innerHTML = spin('h-3 w-3') + '<span>Removing</span>';
  binning = true;
  try {
    await post('/shots/trash', {name: btn.dataset.shot});
  } catch (err) {
    // Nothing to say here that the list does not say better. A removal only
    // fails because the board no longer has that one, and what it does have is
    // what gets drawn below.
  } finally {
    binning = false;
  }
  // Awaited, so the button spins until the row it sits in has actually gone
  // rather than until the board said it would.
  await shots();
});

// requests() is not in here: refresh() asks for it when the board says there is
// something new to ask for.
function tick() {
  refresh();
  tokens();
  networks();
  shots();
}
tick();
// None of this changes because the page did it: another tab, the board joining
// a different network, or the poller going round again a minute from now.
setInterval(tick, 5000);
</script>
)HTML";

void handleRoot() { server.send_P(200, PSTR("text/html"), PAGE); }

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
  for (uint8_t i = tokenCount; i < TOKEN_SLOTS; i++) {
    tokenKey(key, sizeof(key), i);
    store.remove(key);
  }
  store.putUChar("tn", tokenCount);
  store.putUChar("tp", picked);
}

void loadTokens() {
  if (store.isKey("tn")) {
    tokenCount = store.getUChar("tn", 0);
    if (tokenCount > TOKEN_SLOTS) {
      tokenCount = TOKEN_SLOTS;
    }
    char key[4];
    for (uint8_t i = 0; i < tokenCount; i++) {
      tokenKey(key, sizeof(key), i);
      store.getString(key, tokens[i], TOKEN_MAX);
    }
    picked = store.getUChar("tp", 0);
    if (picked >= tokenCount) {
      picked = 0;
    }
    return;
  }
  // The single token this replaced, carried into the first slot so a board
  // already reading an account keeps reading it across the flash.
  store.getString("token", tokens[0], TOKEN_MAX);
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

void handleState() {
  String json = "{\"address\":";
  // Once, because the radio task can drop the association between two reads of
  // it and the second would come back null for the first one's non-null answer.
  const char *address = wifiAddress();
  appendQuoted(json, address ? address : "");
  json += ",\"every\":";
  json += usageEvery();
  json += ",\"calls\":";
  json += netCallsMade();
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
                "{\"ok\":false,\"error\":\"that is not one it knows\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRequests() {
  // Sent as it is built rather than assembled and then sent. A hundred rows is
  // thirteen kilobytes of JSON, and a String that size is thirteen kilobytes of
  // internal heap taken while a TLS handshake on the other task may be wanting
  // forty-eight of it.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  String out = "[";
  NetCall c;
  uint16_t n = netCallCount();
  for (uint16_t i = 0; i < n; i++) {
    if (!netCallAt(i, &c)) {
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
  out += ']';
  server.sendContent(out);
  // An empty chunk is what ends a chunked reply.
  server.sendContent("");
}

// Hints only, for the same reason /networks is names only: the page that would
// show a token is the page anybody on this network can open.
void handleTokens() {
  String json = "[";
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
  server.send(200, "application/json", json);
}

void handleAddToken() {
  String given = server.arg("token");
  given.trim();
  if (!given.length()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"it needs a token\"}");
    return;
  }
  if (given.length() >= TOKEN_MAX) {
    server.send(413, "application/json",
                "{\"ok\":false,\"error\":\"that is longer than the store will take\"}");
    return;
  }
  if (tokenCount >= TOKEN_SLOTS) {
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
  strncpy(tokens[tokenCount], given.c_str(), TOKEN_MAX - 1);
  tokens[tokenCount][TOKEN_MAX - 1] = '\0';
  // Onto the new one, which is what typing one in is asking for.
  picked = tokenCount++;
  store.putUChar("tn", tokenCount);
  store.putUChar("tp", picked);
  Serial.printf("portal: token %u of %u added and picked\n", picked + 1, tokenCount);
  usageTokenChanged();
  server.send(200, "application/json", "{\"ok\":true}");
}

// Which slot the requests go out with, by position in the list the page drew.
int tokenAt() {
  long at = server.arg("index").toInt();
  return at >= 0 && at < (long)tokenCount ? (int)at : -1;
}

void handlePickToken() {
  int at = tokenAt();
  if (at < 0) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"that is not one it has\"}");
    return;
  }
  if ((uint8_t)at != picked) {
    picked = (uint8_t)at;
    store.putUChar("tp", picked);
    Serial.printf("portal: token %u picked\n", picked + 1);
    usageTokenChanged();
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleTrashToken() {
  int at = tokenAt();
  if (at < 0) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"that is not one it has\"}");
    return;
  }
  bool was = (uint8_t)at == picked;
  for (uint8_t i = (uint8_t)at; i + 1 < tokenCount; i++) {
    memcpy(tokens[i], tokens[i + 1], TOKEN_MAX);
  }
  tokenCount--;
  memset(tokens[tokenCount], 0, TOKEN_MAX);
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

// Names and sizes. Ten of them, so this is built and sent rather than streamed
// the way the call log has to be.
void handleShots() {
  String json = "[";
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
  loadTokens();
  Serial.printf("portal: %u token%s in store\n", tokenCount, tokenCount == 1 ? "" : "s");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/state", HTTP_GET, handleState);
  server.on("/requests", HTTP_GET, handleRequests);
  server.on("/networks", HTTP_GET, handleNetworks);
  server.on("/networks", HTTP_POST, handleAddNetwork);
  server.on("/forget", HTTP_POST, handleForgetNetwork);
  server.on("/every", HTTP_POST, handleSetEvery);
  server.on("/tokens", HTTP_GET, handleTokens);
  server.on("/tokens", HTTP_POST, handleAddToken);
  server.on("/tokens/pick", HTTP_POST, handlePickToken);
  server.on("/tokens/trash", HTTP_POST, handleTrashToken);
  server.on("/shots", HTTP_GET, handleShots);
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

const char *portalToken() {
  return picked < tokenCount && tokens[picked][0] ? tokens[picked] : nullptr;
}
