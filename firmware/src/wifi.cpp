#include "wifi.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>

// Generated from firmware/.env into the build directory by secrets.py. A
// checkout without a .env still compiles; the list is simply empty.
#include "secrets.h"

namespace {

constexpr uint32_t ATTEMPT_MS = 8000;
constexpr uint32_t RETRY_MS = 5000;
// The signal is on the glass, so it is asked for often enough to move.
constexpr uint32_t POLL_MS = 1000;
constexpr uint8_t COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

WiFiMulti multi;
volatile bool joined = false;
char onNetwork[33] = {0};
char onAddress[16] = {0};
volatile int onRssi = 0;

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

  uint8_t known = 0;
  for (uint8_t i = 0; i < COUNT; i++) {
    if (!WIFI_NETWORKS[i].ssid) {
      continue;
    }
    multi.addAP(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password);
    known++;
  }
  Serial.printf("wifi: %u network%s known\n", known, known == 1 ? "" : "s");
  if (known == 0) {
    return;
  }

  // Core 0. Arduino's loop runs on core 1, and a scan would otherwise stall the
  // panel for whole seconds at a time.
  xTaskCreatePinnedToCore(wifiTask, "wifi", 4096, nullptr, 1, nullptr, 0);
}

bool wifiConnected() { return joined; }

const char *wifiNetwork() { return joined ? onNetwork : nullptr; }

const char *wifiAddress() { return joined ? onAddress : nullptr; }

int wifiRssi() { return joined ? onRssi : 0; }
