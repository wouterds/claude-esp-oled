#pragma once

#include <stdint.h>

struct WifiNetwork {
  const char *ssid;
  const char *password;
};

// Joins whichever known network is in range, and keeps trying if none is. All
// of it happens on the other core: scanning and associating take seconds, and
// the face is drawn thirty times in each of them.
void wifiBegin();

bool wifiConnected();

// Everything the device will try: the networks built into the image from .env
// and the ones typed into the portal and kept in the flash's key store. There
// is deliberately no way to ask for a password - one goes in and comes back out
// only into the radio.
constexpr uint8_t WIFI_STORED_MAX = 8;

uint8_t wifiKnown();
const char *wifiKnownSsid(uint8_t i);

// Whether that one came out of the store, and so can be forgotten. The ones
// built into the image cannot be, and there is nowhere to put the change if
// they could - they are in flash beside the code.
bool wifiKnownStored(uint8_t i);

// Keeps one and starts trying it. False when the store is full, when the name
// is empty or too long for the standard, or when it is already known.
bool wifiAdd(const char *ssid, const char *password);

// Forgets a stored one, password and all, and comes off it if that is what the
// board is on. False for a name that is not stored, built-in ones included.
bool wifiForget(const char *ssid);

// The network it is on, or nullptr.
const char *wifiNetwork();

// Its address on that network, or nullptr.
const char *wifiAddress();

// The radio's own address. It has one whether or not it is on a network, so
// this is the one identifier the page can always show.
const char *wifiMac();

// How strong that network is in dBm - always negative, and closer to zero is
// better. Zero exactly when it is on nothing.
int wifiRssi();
