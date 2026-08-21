#pragma once

struct WifiNetwork {
  const char *ssid;
  const char *password;
};

// Joins whichever known network is in range, and keeps trying if none is. All
// of it happens on the other core: scanning and associating take seconds, and
// the face is drawn thirty times in each of them.
void wifiBegin();

bool wifiConnected();

// The network it is on, or nullptr.
const char *wifiNetwork();

// Its address on that network, or nullptr.
const char *wifiAddress();

// How strong that network is in dBm - always negative, and closer to zero is
// better. Zero exactly when it is on nothing.
int wifiRssi();
