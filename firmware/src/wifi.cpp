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
constexpr uint8_t COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

WiFiMulti multi;
volatile bool joined = false;
char onNetwork[33] = {0};

void wifiTask(void *) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!joined) {
        strncpy(onNetwork, WiFi.SSID().c_str(), sizeof(onNetwork) - 1);
        Serial.printf("wifi: on %s, %d dBm, %s\n", onNetwork, WiFi.RSSI(),
                      WiFi.localIP().toString().c_str());
        joined = true;
      }
      vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
      continue;
    }

    if (joined) {
      Serial.println("wifi: dropped");
      joined = false;
      onNetwork[0] = '\0';
    }
    // Blocks for as long as it takes to scan and associate, which is why none
    // of this is on the core drawing the face.
    multi.run(ATTEMPT_MS);
    if (WiFi.status() != WL_CONNECTED) {
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
