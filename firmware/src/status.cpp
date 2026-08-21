#include "status.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "battery.h"
#include "board.h"
#include "text.h"
#include "wifi.h"

namespace {

// Above where the face can ever reach. The face clears its own box before it
// draws, and that box starts at y=50 when it has drifted as high as it goes -
// anything below this line gets wiped by it and, since status only redraws when
// what it says changes, never comes back.
constexpr int16_t BAR_TOP = 26;
constexpr int16_t BAR_BOTTOM = 44;
constexpr int16_t MIDDLE = 34;

// The glass is a circle. At y=26 there are only 93 pixels either side of the
// centre line, so everything here has to live between 87 and 273.
constexpr int16_t BATTERY_X = 126;
constexpr int16_t PERCENT_X = 141;
constexpr int16_t WIFI_X = 232;

constexpr int16_t ADDRESS_Y = 298;
constexpr int16_t BOTTOM_FROM = ADDRESS_Y - 3;
constexpr int16_t BOTTOM_TO = ADDRESS_Y + 17;

constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t GREEN = 0x07E0;
constexpr uint16_t AMBER = 0xFD20;
constexpr uint16_t RED = 0xF800;
constexpr uint16_t DIM = 0x4208;

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

// The address arrives a letter at a time, the way the mood used to.
char typing[16] = {0};
uint8_t typed = 0;
uint32_t typedAt = 0;

// Framebuffer rows run the other way to screen rows.
inline int16_t bandFrom(int16_t screenTo) { return (int16_t)(SCREEN_H - 1 - screenTo); }
inline int16_t bandTo(int16_t screenFrom) { return (int16_t)(SCREEN_H - 1 - screenFrom); }

bool overlaps(int16_t aFrom, int16_t aTo, int16_t bFrom, int16_t bTo) {
  return aFrom <= bTo && bFrom <= aTo;
}

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Coverage rather than a hard edge, the same way the face is drawn. At this size
// a jagged corner is most of what you see of an icon.
void plot(uint16_t *fb, int16_t x, int16_t y, float coverage, uint16_t colour) {
  if (coverage <= 0.02f || x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) {
    return;
  }
  coverage = clamp01(coverage);
  uint16_t r = (uint16_t)(((colour >> 11) & 0x1F) * coverage);
  uint16_t g = (uint16_t)(((colour >> 5) & 0x3F) * coverage);
  uint16_t b = (uint16_t)((colour & 0x1F) * coverage);
  boardRow(fb, y)[boardX(x)] = boardColour((uint16_t)((r << 11) | (g << 5) | b));
}

void clearBand(uint16_t *fb, int16_t from, int16_t to) {
  for (int16_t y = from; y <= to; y++) {
    if (y < 0 || y >= SCREEN_H) {
      continue;
    }
    memset(boardRow(fb, y), 0, (size_t)SCREEN_W * 2);
  }
}

float sdRoundBox(float px, float py, float hx, float hy, float r) {
  float limit = hx < hy ? hx : hy;
  if (r > limit) {
    r = limit;
  }
  float qx = fabsf(px) - hx + r;
  float qy = fabsf(py) - hy + r;
  float ax = qx > 0.0f ? qx : 0.0f;
  float ay = qy > 0.0f ? qy : 0.0f;
  float inner = qx > qy ? qx : qy;
  return sqrtf(ax * ax + ay * ay) + (inner < 0.0f ? inner : 0.0f) - r;
}

// An arc opening by `aperture` either side of straight up, with rounded ends.
float sdArc(float px, float py, float sinA, float cosA, float ra, float rb) {
  px = fabsf(px);
  py = -py;
  if (cosA * px > sinA * py) {
    float dx = px - sinA * ra;
    float dy = py - cosA * ra;
    return sqrtf(dx * dx + dy * dy) - rb;
  }
  return fabsf(sqrtf(px * px + py * py) - ra) - rb;
}

void drawBattery(uint16_t *fb, uint8_t percent, uint16_t colour) {
  constexpr float HW = 11.0f;
  constexpr float HH = 6.0f;
  constexpr float R = 3.0f;
  constexpr float WALL = 1.4f;
  float fillTo = -HW + 1.6f + (2.0f * HW - 3.2f) * (percent / 100.0f);

  for (int16_t y = MIDDLE - 8; y <= MIDDLE + 8; y++) {
    for (int16_t x = BATTERY_X - 14; x <= BATTERY_X + 16; x++) {
      float px = (float)x + 0.5f - BATTERY_X;
      float py = (float)y + 0.5f - MIDDLE;

      // The shell is the outline of a rounded box: its distance folded about
      // zero, which is the band of pixels within a wall's width of the edge.
      float shell = fabsf(sdRoundBox(px, py, HW, HH, R)) - WALL;
      // And the tip, a little rounded stub off the end.
      float nub = sdRoundBox(px - HW - 1.8f, py, 1.6f, 2.6f, 1.2f);
      float outline = shell < nub ? shell : nub;

      float charge = 1.0f;
      if (percent > 0) {
        float inside = sdRoundBox(px, py, HW - 3.0f, HH - 3.0f, R * 0.5f);
        charge = px <= fillTo ? inside : 1.0f;
      }
      float d = outline < charge ? outline : charge;
      plot(fb, x, y, 0.5f - d, colour);
    }
  }
}

void drawWifi(uint16_t *fb, bool online) {
  constexpr float APERTURE = 0.72f;
  const float sinA = sinf(APERTURE);
  const float cosA = cosf(APERTURE);
  uint16_t colour = online ? WHITE : DIM;

  for (int16_t y = MIDDLE - 9; y <= MIDDLE + 9; y++) {
    for (int16_t x = WIFI_X - 12; x <= WIFI_X + 12; x++) {
      // Measured from the bottom of the glyph, which is where the arcs and the
      // dot are all centred.
      float px = (float)x + 0.5f - WIFI_X;
      float py = (float)y + 0.5f - (MIDDLE + 6.0f);

      float d = sdArc(px, py, sinA, cosA, 11.0f, 1.5f);
      float mid = sdArc(px, py, sinA, cosA, 7.0f, 1.5f);
      if (mid < d) {
        d = mid;
      }
      float dot = sqrtf(px * px + py * py) - 1.8f;
      if (dot < d) {
        d = dot;
      }
      float coverage = 0.5f - d;

      if (!online) {
        // Struck through rather than hidden. An icon that vanishes when it has
        // something to say is one you have to notice the absence of.
        float along = (px + py) * 0.70710678f;
        float across = (px - py) * 0.70710678f;
        float bar = sdRoundBox(across, along, 1.4f, 11.0f, 1.4f);
        plot(fb, x, y, 0.5f - bar, WHITE);
      }
      plot(fb, x, y, coverage, colour);
    }
  }
}

uint16_t batteryColour(const BatteryState &battery, bool blink) {
  if (battery.charging) {
    return GREEN;
  }
  if (battery.percent <= CRITICAL_PERCENT) {
    return blink ? RED : 0x3800;
  }
  if (battery.percent <= LOW_PERCENT) {
    return AMBER;
  }
  return WHITE;
}

}  // namespace

void statusDraw(uint16_t *fb, int16_t faceFrom, int16_t faceTo) {
  BatteryState battery = batteryRead();
  const char *address = wifiAddress();
  bool online = address != nullptr;
  bool blink = battery.percent <= CRITICAL_PERCENT && !battery.charging
                   ? ((millis() / 450) & 1) != 0
                   : false;

  bool topChanged = battery.percent != shown.percent || battery.charging != shown.charging ||
                    battery.present != shown.present || online != shown.online ||
                    blink != shown.blink;
  // Whatever the face just painted over is gone, whether it changed or not.
  topChanged |= overlaps(bandFrom(BAR_BOTTOM), bandTo(BAR_TOP), faceFrom, faceTo);

  const char *want = address ? address : "";
  if (strncmp(want, shown.address, sizeof(shown.address)) != 0) {
    strncpy(shown.address, want, sizeof(shown.address) - 1);
    shown.address[sizeof(shown.address) - 1] = '\0';
    typed = 0;
    typedAt = millis();
  }

  uint8_t full = (uint8_t)strlen(shown.address);
  uint8_t reveal = (uint8_t)((millis() - typedAt) / 45);
  if (reveal > full) {
    reveal = full;
  }
  bool bottomChanged = reveal != typed;
  bottomChanged |= overlaps(bandFrom(BOTTOM_TO), bandTo(BOTTOM_FROM), faceFrom, faceTo);

  if (!topChanged && !bottomChanged) {
    return;
  }
  typed = reveal;

  if (topChanged) {
    clearBand(fb, BAR_TOP, BAR_BOTTOM);
    if (battery.present) {
      uint16_t colour = batteryColour(battery, blink);
      drawBattery(fb, battery.percent, colour);
      char label[6];
      snprintf(label, sizeof(label), "%u%%", battery.percent);
      // Left aligned just past the tip, and centred on the icon's own middle
      // line. textDraw centres on a point, so the point moves with the length -
      // otherwise 9% and 100% sit in different places.
      int16_t glyphs = (int16_t)strlen(label);
      int16_t left = BATTERY_X + 20;
      textDraw(fb, label, (int16_t)(left + (glyphs * 6 - 1) / 2), MIDDLE - 3, 1,
               boardColour(colour));
    }
    drawWifi(fb, online);
    boardFlushRows(bandFrom(BAR_BOTTOM), bandTo(BAR_TOP));
    shown.percent = battery.percent;
    shown.charging = battery.charging;
    shown.present = battery.present;
    shown.online = online;
    shown.blink = blink;
  }

  if (bottomChanged) {
    clearBand(fb, BOTTOM_FROM, BOTTOM_TO);
    if (typed > 0) {
      memcpy(typing, shown.address, typed);
      typing[typed] = '\0';
      // Centred on where the whole address will be, so it does not slide left
      // as it arrives.
      constexpr int16_t STEP = 12;
      int16_t left = (int16_t)(SCREEN_R - (full * STEP - 2) / 2);
      textDraw(fb, typing, (int16_t)(left + (typed * STEP - 2) / 2), ADDRESS_Y, 2,
               boardColour(WHITE));
    }
    boardFlushRows(bandFrom(BOTTOM_TO), bandTo(BOTTOM_FROM));
  }
}
