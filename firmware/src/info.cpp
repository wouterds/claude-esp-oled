#include "info.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "battery.h"
#include "board.h"
#include "text.h"
#include "version.h"
#include "wifi.h"

namespace {

// The cell, stood on its end. Big enough to be the thing the page is about: a
// third of the glass tall, which is as far as it can go and still leave room
// under it for three lines and the number.
//
// The block sits a little above the middle of the glass rather than on it. Hung
// on its own full height it reads as low - the cell is what the eye takes as
// the middle of the page, and the three lines under it weigh almost nothing.
constexpr float CELL_X = 180.0f;
constexpr float CELL_Y = 116.0f;
constexpr float CELL_HW = 34.0f;
constexpr float CELL_HH = 60.0f;
constexpr float CELL_R = 14.0f;
constexpr float WALL = 3.0f;
// Between the wall and the charge inside it. Without a gap the fill welds
// itself to the shell and the whole thing reads as a solid block.
constexpr float GAP = 7.0f;
constexpr float NUB_HW = 13.0f;
constexpr float NUB_HH = 5.0f;

constexpr int16_t PERCENT_TOP = 194;
constexpr int16_t PERCENT_SCALE = 4;
constexpr int16_t NETWORK_TOP = 240;
constexpr int16_t ADDRESS_TOP = 262;
constexpr int16_t COMMIT_TOP = 290;
constexpr int16_t LINE_SCALE = 2;
// What fits between the edges of the glass down there, in glyphs. A network can
// be called anything up to thirty-two characters and the ones that long run off
// both sides of the circle.
constexpr uint8_t LINE_GLYPHS = 24;

constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t GREY = 0x8410;
constexpr uint16_t FAINT = 0x4A49;
// The same colours the small battery uses at the same thresholds, so a colour
// means one thing whichever side of the glass it is on.
constexpr uint16_t GREEN = 0x07F6;
constexpr uint16_t YELLOW = 0xFFE0;
constexpr uint16_t ORANGE = 0xFC00;
constexpr uint16_t RED = 0xF800;
constexpr uint8_t YELLOW_AT = 30;
constexpr uint8_t ORANGE_AT = 20;
constexpr uint8_t RED_AT = 10;

struct Shown {
  uint8_t percent;
  bool charging;
  bool present;
  char network[33];
  char address[16];
};

Shown shown = {255, false, false, {0}, {0}};
bool fresh = true;

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

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

uint16_t chargeColour(const BatteryState &battery) {
  if (battery.charging) {
    return GREEN;
  }
  if (battery.percent <= RED_AT) {
    return RED;
  }
  if (battery.percent <= ORANGE_AT) {
    return ORANGE;
  }
  if (battery.percent <= YELLOW_AT) {
    return YELLOW;
  }
  return WHITE;
}

void drawCell(uint16_t *fb, const BatteryState &battery, uint16_t colour) {
  constexpr float CAVITY_HW = CELL_HW - WALL - GAP;
  constexpr float CAVITY_HH = CELL_HH - WALL - GAP;
  // Down the screen is up the cell: the charge stands on the bottom of it and
  // its top edge is what moves. It is a box in its own right rather than the
  // cavity cut off at a line, so the top of it is as round as the bottom - and
  // as it empties the corners close on each other until what is left is a
  // lozenge rather than a sliver with square shoulders.
  float top = CAVITY_HH - 2.0f * CAVITY_HH * (battery.percent / 100.0f);
  float chargeHH = (CAVITY_HH - top) * 0.5f;
  float chargeY = (CAVITY_HH + top) * 0.5f;

  for (int16_t y = (int16_t)(CELL_Y - CELL_HH - NUB_HH * 2 - 2);
       y <= (int16_t)(CELL_Y + CELL_HH + 2); y++) {
    for (int16_t x = (int16_t)(CELL_X - CELL_HW - 2); x <= (int16_t)(CELL_X + CELL_HW + 2); x++) {
      float px = (float)x + 0.5f - CELL_X;
      float py = (float)y + 0.5f - CELL_Y;

      // The shell is the outline of the box: its distance folded about zero,
      // which is the band of pixels within a wall's width of the edge.
      float shell = fabsf(sdRoundBox(px, py, CELL_HW, CELL_HH, CELL_R)) - WALL;
      float nub = sdRoundBox(px, py + CELL_HH + NUB_HH, NUB_HW, NUB_HH, 3.0f);
      plot(fb, x, y, 0.5f - (shell < nub ? shell : nub), colour);

      if (battery.percent == 0) {
        continue;
      }
      // Cut to the cavity, or what is left at a few percent is a full-width
      // sliver hanging out past the rounded bottom of the space holding it.
      float cavity = sdRoundBox(px, py, CAVITY_HW, CAVITY_HH, CELL_R * 0.5f);
      float charge = sdRoundBox(px, py - chargeY, CAVITY_HW, chargeHH, CELL_R * 0.5f);
      plot(fb, x, y, 0.5f - (charge > cavity ? charge : cavity), colour);
    }
  }
}

// As much of it as the glass has room for, rather than as much of it as there
// is. A name that runs off the circle is worse than one that stops.
void line(uint16_t *fb, const char *s, int16_t top, uint16_t colour) {
  char fits[LINE_GLYPHS + 1];
  strncpy(fits, s, LINE_GLYPHS);
  fits[LINE_GLYPHS] = '\0';
  textDraw(fb, fits, (int16_t)SCREEN_R, top, LINE_SCALE, boardColour(colour));
}

}  // namespace

void infoForget() { fresh = true; }

void infoStep(uint16_t *fb) {
  BatteryState battery = batteryRead();
  const char *network = wifiNetwork();
  const char *address = wifiAddress();
  const char *wants = network ? network : "OFFLINE";
  const char *at = address ? address : "";

  bool changed = fresh || battery.percent != shown.percent ||
                 battery.charging != shown.charging || battery.present != shown.present ||
                 strncmp(wants, shown.network, sizeof(shown.network)) != 0 ||
                 strncmp(at, shown.address, sizeof(shown.address)) != 0;
  if (!changed) {
    return;
  }
  fresh = false;
  shown.percent = battery.percent;
  shown.charging = battery.charging;
  shown.present = battery.present;
  strncpy(shown.network, wants, sizeof(shown.network) - 1);
  shown.network[sizeof(shown.network) - 1] = '\0';
  strncpy(shown.address, at, sizeof(shown.address) - 1);
  shown.address[sizeof(shown.address) - 1] = '\0';

  memset(fb, 0, (size_t)SCREEN_W * SCREEN_H * 2);

  uint16_t colour = chargeColour(battery);
  if (battery.present) {
    drawCell(fb, battery, colour);
    char said[6];
    snprintf(said, sizeof(said), "%u%%", (unsigned)battery.percent);
    textDraw(fb, said, (int16_t)SCREEN_R, PERCENT_TOP, PERCENT_SCALE, boardColour(colour));
  }

  line(fb, shown.network, NETWORK_TOP, WHITE);
  if (shown.address[0]) {
    line(fb, shown.address, ADDRESS_TOP, GREY);
  }
  line(fb, BUILD_COMMIT, COMMIT_TOP, FAINT);

  boardFlush();
}
