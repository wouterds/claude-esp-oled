#include "wifi.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <string.h>

// Generated from firmware/.env into the build directory by secrets.py. A
// checkout without a .env still compiles; the list is simply empty.
#include "secrets.h"

namespace {

constexpr uint32_t ATTEMPT_MS = 8000;
constexpr uint32_t RETRY_MS = 5000;
// The signal is on the glass, so it is asked for often enough to move.
constexpr uint32_t POLL_MS = 1000;
constexpr uint8_t COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

// 32 characters is the whole of what an SSID may be, and 63 the whole of a WPA2
// passphrase; both carry the terminator on top of that.
constexpr uint8_t SSID_MAX = 33;
constexpr uint8_t PASS_MAX = 64;

struct Known {
  char ssid[SSID_MAX];
  char password[PASS_MAX];
  bool stored;
};

WiFiMulti multi;
Preferences store;
Known known[COUNT + WIFI_STORED_MAX];
uint8_t knownCount = 0;
// Held over every read and every change of the list above. The portal changes
// it from its own task while the radio task is walking it.
SemaphoreHandle_t lock = nullptr;
void listTake() { xSemaphoreTake(lock, portMAX_DELAY); }
void listGive() { xSemaphoreGive(lock); }
// The list has changed and the radio's copy of it has not. Rebuilt on the wifi
// task rather than under the caller, because run() may be inside a scan.
volatile bool relist = false;
volatile bool joined = false;
char macAddress[18] = {0};
char onNetwork[33] = {0};
char onAddress[16] = {0};
volatile int onRssi = 0;

int findKnown(const char *ssid) {
  for (uint8_t i = 0; i < knownCount; i++) {
    if (strcmp(known[i].ssid, ssid) == 0) {
      return i;
    }
  }
  return -1;
}

void keyFor(char *out, size_t size, char kind, uint8_t i) { snprintf(out, size, "%c%u", kind, i); }

// The whole stored set, rewritten every time one of them changes. Anything past
// the new count is removed rather than left: a forgotten network's password
// staying in the flash is the one thing forgetting it was meant to do.
void saveStored() {
  uint8_t n = 0;
  char sk[4];
  char pk[4];
  for (uint8_t i = 0; i < knownCount; i++) {
    if (!known[i].stored) {
      continue;
    }
    keyFor(sk, sizeof(sk), 's', n);
    keyFor(pk, sizeof(pk), 'p', n);
    store.putString(sk, known[i].ssid);
    store.putString(pk, known[i].password);
    n++;
  }
  for (uint8_t i = n; i < WIFI_STORED_MAX; i++) {
    keyFor(sk, sizeof(sk), 's', i);
    keyFor(pk, sizeof(pk), 'p', i);
    store.remove(sk);
    store.remove(pk);
  }
  store.putUChar("n", n);
}

// After the compiled-in ones, so a name in both belongs to the build - which is
// the copy that cannot be removed.
void loadStored() {
  uint8_t n = store.getUChar("n", 0);
  char sk[4];
  char pk[4];
  for (uint8_t i = 0; i < n && i < WIFI_STORED_MAX; i++) {
    keyFor(sk, sizeof(sk), 's', i);
    keyFor(pk, sizeof(pk), 'p', i);
    Known &k = known[knownCount];
    k.ssid[0] = '\0';
    k.password[0] = '\0';
    store.getString(sk, k.ssid, sizeof(k.ssid));
    if (!k.ssid[0] || findKnown(k.ssid) >= 0) {
      continue;
    }
    store.getString(pk, k.password, sizeof(k.password));
    k.stored = true;
    knownCount++;
  }
}

void rebuild() {
  multi.APlistClean();
  for (uint8_t i = 0; i < knownCount; i++) {
    multi.addAP(known[i].ssid, known[i].password);
  }
}

// Once a boot, after the radio has settled: what is actually in the air.
// An SSID that does not appear here is not one the device is failing to join,
// it is one that is not there - a different problem that looks exactly the same
// from outside. Two usual reasons: a phone handing out its hotspot on 5GHz,
// which this radio cannot hear at all, and a phone that has gone to sleep and
// stopped handing it out.
void look() {
  int found = WiFi.scanNetworks();
  Serial.printf("wifi: %d networks in range\n", found);
  for (int i = 0; i < found; i++) {
    String name = WiFi.SSID(i);
    Serial.printf("wifi:   [%s] %d dBm, %u chars\n", name.c_str(), WiFi.RSSI(i),
                  (unsigned)name.length());
  }
  WiFi.scanDelete();
}

void wifiTask(void *) {
  static bool looked = false;
  for (;;) {
    if (relist) {
      relist = false;
      listTake();
      rebuild();
      bool keep = !onNetwork[0] || findKnown(onNetwork) >= 0;
      listGive();
      // Coming off a network that has just been forgotten, rather than staying
      // on it until something else knocks the board off.
      if (!keep) {
        Serial.printf("wifi: leaving %s, forgotten\n", onNetwork);
        WiFi.disconnect();
      }
    }
    if (WiFi.status() == WL_CONNECTED) {
      if (!joined) {
        if (!looked) {
          looked = true;
          look();
        }
        strncpy(onNetwork, WiFi.SSID().c_str(), sizeof(onNetwork) - 1);
        strncpy(onAddress, WiFi.localIP().toString().c_str(), sizeof(onAddress) - 1);
        Serial.printf("wifi: on %s, %d dBm, %s\n", onNetwork, WiFi.RSSI(),
                      WiFi.localIP().toString().c_str());
        joined = true;
      }
      onRssi = WiFi.RSSI();
      vTaskDelay(pdMS_TO_TICKS(POLL_MS));
      continue;
    }

    if (joined) {
      Serial.println("wifi: dropped");
      joined = false;
      onRssi = 0;
      onNetwork[0] = '\0';
      onAddress[0] = '\0';
    }
    // Blocks for as long as it takes to scan and associate, which is why none
    // of this is on the core drawing the face.
    multi.run(ATTEMPT_MS);
    if (WiFi.status() != WL_CONNECTED) {
      if (!looked) {
        looked = true;
        look();
      }
      vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
    }
  }
}

}  // namespace

void wifiBegin() {
  WiFi.mode(WIFI_STA);
  // The radio sleeps between beacons on its own; left awake it costs more than
  // everything else on the board put together.
  WiFi.setSleep(true);
  WiFi.setAutoReconnect(true);

  strncpy(macAddress, WiFi.macAddress().c_str(), sizeof(macAddress) - 1);
  macAddress[sizeof(macAddress) - 1] = '\0';

  lock = xSemaphoreCreateMutex();
  for (uint8_t i = 0; i < COUNT; i++) {
    if (!WIFI_NETWORKS[i].ssid) {
      continue;
    }
    Known &k = known[knownCount++];
    strncpy(k.ssid, WIFI_NETWORKS[i].ssid, sizeof(k.ssid) - 1);
    k.ssid[sizeof(k.ssid) - 1] = '\0';
    strncpy(k.password, WIFI_NETWORKS[i].password, sizeof(k.password) - 1);
    k.password[sizeof(k.password) - 1] = '\0';
    k.stored = false;
  }
  store.begin("wifi", false);
  loadStored();
  rebuild();
  Serial.printf("wifi: %u network%s known\n", knownCount, knownCount == 1 ? "" : "s");

  // Core 0. Arduino's loop runs on core 1, and a scan would otherwise stall the
  // panel for whole seconds at a time.
  xTaskCreatePinnedToCore(wifiTask, "wifi", 4096, nullptr, 1, nullptr, 0);
}

bool wifiConnected() { return joined; }

const char *wifiNetwork() { return joined ? onNetwork : nullptr; }

const char *wifiAddress() { return joined ? onAddress : nullptr; }

const char *wifiMac() { return macAddress; }

int wifiRssi() { return joined ? onRssi : 0; }

uint8_t wifiKnown() { return knownCount; }

const char *wifiKnownSsid(uint8_t i) { return i < knownCount ? known[i].ssid : nullptr; }

bool wifiKnownStored(uint8_t i) { return i < knownCount && known[i].stored; }

bool wifiAdd(const char *ssid, const char *password) {
  if (!ssid || !ssid[0] || strlen(ssid) >= SSID_MAX) {
    return false;
  }
  if (password && strlen(password) >= PASS_MAX) {
    return false;
  }
  listTake();
  bool room = knownCount < COUNT + WIFI_STORED_MAX && findKnown(ssid) < 0;
  if (room) {
    Known &k = known[knownCount++];
    strncpy(k.ssid, ssid, sizeof(k.ssid) - 1);
    k.ssid[sizeof(k.ssid) - 1] = '\0';
    strncpy(k.password, password ? password : "", sizeof(k.password) - 1);
    k.password[sizeof(k.password) - 1] = '\0';
    k.stored = true;
    saveStored();
    relist = true;
  }
  listGive();
  if (room) {
    Serial.printf("wifi: keeping %s\n", ssid);
  }
  return room;
}

bool wifiForget(const char *ssid) {
  if (!ssid || !ssid[0]) {
    return false;
  }
  listTake();
  int at = findKnown(ssid);
  bool gone = at >= 0 && known[at].stored;
  if (gone) {
    for (uint8_t i = (uint8_t)at; i + 1 < knownCount; i++) {
      known[i] = known[i + 1];
    }
    knownCount--;
    saveStored();
    relist = true;
  }
  listGive();
  if (gone) {
    Serial.printf("wifi: forgetting %s\n", ssid);
  }
  return gone;
}
