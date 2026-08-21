#include "status.h"

#include <Arduino.h>
#include <string.h>

#include "battery.h"
#include "board.h"
#include "text.h"
#include "wifi.h"

namespace {

// The panel is a circle, so the top of the frame is not usable - at y=44 there
// are only 118 pixels either side of the middle, and it narrows fast above it.
constexpr int16_t BAR_Y = 44;
constexpr int16_t BAR_H = 16;
constexpr int16_t BATTERY_X = 96;
constexpr int16_t PERCENT_X = 134;
constexpr int16_t WIFI_X = 246;
constexpr int16_t ADDRESS_Y = 298;

constexpr int16_t TOP_FROM = BAR_Y - 2;
constexpr int16_t TOP_TO = BAR_Y + BAR_H + 2;
constexpr int16_t BOTTOM_FROM = ADDRESS_Y - 3;
constexpr int16_t BOTTOM_TO = ADDRESS_Y + 17;

constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t GREEN = 0x07E0;
constexpr uint16_t AMBER = 0xFD20;
constexpr uint16_t RED = 0xF800;

constexpr uint8_t LOW_PERCENT = 20;
constexpr uint8_t CRITICAL_PERCENT = 10;

struct Shown {
  uint8_t percent;
  bool charging;
  bool present;
  bool online;
  bool blink;
  char address[16];
};

Shown shown = {255, false, false, false, false, {0}};

void fillRect(uint16_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t colour) {
  uint16_t packed = boardColour(colour);
  for (int16_t row = y; row < y + h; row++) {
    if (row < 0 || row >= SCREEN_H) {
      continue;
    }
    uint16_t *line = boardRow(fb, row);
    for (int16_t col = x; col < x + w; col++) {
      if (col >= 0 && col < SCREEN_W) {
        line[boardX(col)] = packed;
      }
    }
  }
}

void drawBattery(uint16_t *fb, const BatteryState &battery, uint16_t colour) {
  constexpr int16_t W = 26;
  constexpr int16_t H = 14;
  fillRect(fb, BATTERY_X, BAR_Y, W, H, colour);
  fillRect(fb, BATTERY_X + 2, BAR_Y + 2, W - 4, H - 4, 0x0000);
  // The nub, so it is a battery and not a box.
  fillRect(fb, BATTERY_X + W, BAR_Y + 4, 3, 6, colour);

  int16_t fill = (int16_t)((W - 4) * battery.percent / 100);
  if (fill > 0) {
    fillRect(fb, BATTERY_X + 2, BAR_Y + 2, fill, H - 4, colour);
  }
}

void drawWifi(uint16_t *fb, bool online) {
  // Four bars climbing. Offline they are still drawn, dimmed, with a stroke
  // through them - an icon that disappears when it has something to say is one
  // you have to remember the absence of.
  constexpr int16_t BAR_W = 4;
  constexpr int16_t STEP = 6;
  uint16_t colour = online ? WHITE : 0x39E7;
  for (int16_t i = 0; i < 4; i++) {
    int16_t height = 4 + i * 3;
    fillRect(fb, WIFI_X + i * STEP, BAR_Y + BAR_H - height, BAR_W, height, colour);
  }
  if (online) {
    return;
  }
  for (int16_t i = 0; i < BAR_H; i++) {
    fillRect(fb, WIFI_X + i, BAR_Y + BAR_H - 1 - i, 2, 2, WHITE);
  }
}

uint16_t batteryColour(const BatteryState &battery, bool blink) {
  if (battery.charging) {
    return GREEN;
  }
  if (battery.percent <= CRITICAL_PERCENT) {
    return blink ? RED : 0x2000;
  }
  if (battery.percent <= LOW_PERCENT) {
    return AMBER;
  }
  return WHITE;
}

}  // namespace

void statusDraw(uint16_t *fb) {
  BatteryState battery = batteryRead();
  const char *address = wifiAddress();
  bool online = address != nullptr;
  // Only the critical one flashes, and only it needs the clock.
  bool blink = battery.percent <= CRITICAL_PERCENT && !battery.charging
                   ? ((millis() / 450) & 1) != 0
                   : false;

  bool topChanged = battery.percent != shown.percent || battery.charging != shown.charging ||
                    battery.present != shown.present || online != shown.online ||
                    blink != shown.blink;
  bool bottomChanged = strncmp(address ? address : "", shown.address, sizeof(shown.address)) != 0;
  if (!topChanged && !bottomChanged) {
    return;
  }

  if (topChanged) {
    fillRect(fb, 60, TOP_FROM, 240, TOP_TO - TOP_FROM, 0x0000);
    if (battery.present) {
      uint16_t colour = batteryColour(battery, blink);
      drawBattery(fb, battery, colour);
      char label[6];
      snprintf(label, sizeof(label), "%u%%", battery.percent);
      textDraw(fb, label, PERCENT_X + 24, BAR_Y + 1, 2, boardColour(colour));
    }
    drawWifi(fb, online);
    boardFlushRows((int16_t)(SCREEN_H - 1 - TOP_TO), (int16_t)(SCREEN_H - 1 - TOP_FROM));
    shown.percent = battery.percent;
    shown.charging = battery.charging;
    shown.present = battery.present;
    shown.online = online;
    shown.blink = blink;
  }

  if (bottomChanged) {
    fillRect(fb, 0, BOTTOM_FROM, SCREEN_W, BOTTOM_TO - BOTTOM_FROM, 0x0000);
    if (address) {
      textDraw(fb, address, (int16_t)SCREEN_R, ADDRESS_Y, 2, boardColour(WHITE));
    }
    boardFlushRows((int16_t)(SCREEN_H - 1 - BOTTOM_TO), (int16_t)(SCREEN_H - 1 - BOTTOM_FROM));
    strncpy(shown.address, address ? address : "", sizeof(shown.address) - 1);
    shown.address[sizeof(shown.address) - 1] = '\0';
  }
}
