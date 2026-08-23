#include "status.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "battery.h"
#include "board.h"
#include "gauge.h"
#include "text.h"
#include "usage.h"
#include "wifi.h"

namespace {

// Above where the face can ever reach. The face clears its own box before it
// draws, and that box starts at y=50 when it has drifted as high as it goes -
// anything below this line gets wiped by it and, since status only redraws when
// what it says changes, never comes back.
//
// The band is the taller of the two icons plus a pixel, because a row of glyph
// left outside it is a row that never gets cleared.
constexpr int16_t MIDDLE = 17;
constexpr int16_t BAR_TOP = MIDDLE - 10;
constexpr int16_t BAR_BOTTOM = MIDDLE + 10;

// The pair centred on the panel, wifi then battery. The glass is a circle: at
// this height it gives about 80 pixels either side of the middle, and the two
// of them together come to sixty-odd.
constexpr int16_t WIFI_X = 160;
constexpr int16_t BATTERY_X = 199;

constexpr int16_t ADDRESS_Y = 310;
constexpr int16_t BOTTOM_SCALE = 2;
constexpr int16_t BOTTOM_FROM = ADDRESS_Y - 3;
constexpr int16_t BOTTOM_TO = ADDRESS_Y + 16;

constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t GREY = 0x630C;
// The same teal the gauges start at, so one green means one thing here.
constexpr uint16_t GREEN = 0x07F6;
// Warmed off pure yellow and given a little blue. Full red and full green with
// none at all is an acid colour on a panel that is lit from behind - it reads
// as a warning when this level is only a notice, and it is the one colour on
// here that has to sit next to white without looking broken.
constexpr uint16_t YELLOW = 0xFE48;
constexpr uint16_t ORANGE = 0xFC00;
constexpr uint16_t RED = 0xF800;
// Not as dark as it looks like it should be. Black here is a crystal failing
// to block a backlight that is always on, so anything under about a third
// sinks into it and the ring may as well not have been drawn.
constexpr uint16_t DIM = 0x738E;
// What a ring lights up to while it is still looking. Grey rather than white,
// because white is what being on a network looks like.
constexpr uint16_t SEEKING = 0xA534;

// Where a window has nothing left to give. The endpoint rounds to whole
// percent, so this is the whole of what being out looks like from here.
constexpr uint8_t SPENT = 100;

// The fill climbing from the charge that is there to full and starting over,
// while the cable is in. The same rate as the big one on the other page, so the
// two are one idea seen twice rather than two animations.
constexpr uint8_t RISE_STEPS = 24;
constexpr uint32_t RISE_MS = 60;

constexpr uint8_t YELLOW_AT = 30;
constexpr uint8_t ORANGE_AT = 20;
constexpr uint8_t RED_AT = 10;

struct Shown {
  uint8_t bars;
  uint8_t percent;
  bool charging;
  bool present;
  bool blink;
  uint8_t rise;
  char address[16];
};

Shown shown = {255, 255, false, false, false, 0, {0}};

// HH:MM:SS.MS. The hours run as wide as they have to rather than rolling into
// days: a week out is a hundred and sixty-odd of them, and one field growing a
// glyph reads better than three fields that mean different things depending on
// how far away the reset is.
void clockOf(char *out, size_t size, uint32_t ms) {
  uint32_t hundredths = ms / 10;
  snprintf(out, size, "%02u:%02u:%02u.%02u", (unsigned)(hundredths / 360000),
           (unsigned)((hundredths / 6000) % 60), (unsigned)((hundredths / 100) % 60),
           (unsigned)(hundredths % 100));
}

// The address arrives a letter at a time, the way the mood used to.
char typing[16] = {0};
uint8_t typed = 0;
uint32_t typedAt = 0;
// Half the width of what was last put down there, so a shorter line still takes
// the longer one it replaces off the glass.
int16_t wiped = 0;

// Framebuffer rows run the other way to screen rows.
inline int16_t bandFrom(int16_t screenTo) { return (int16_t)(SCREEN_H - 1 - screenTo); }
inline int16_t bandTo(int16_t screenFrom) { return (int16_t)(SCREEN_H - 1 - screenFrom); }

bool overlaps(int16_t aFrom, int16_t aTo, int16_t bFrom, int16_t bTo) {
  return aFrom <= bTo && bFrom <= aTo;
}

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

uint16_t fade(uint16_t colour, float by) {
  uint16_t r = (uint16_t)(((colour >> 11) & 0x1F) * by);
  uint16_t g = (uint16_t)(((colour >> 5) & 0x3F) * by);
  uint16_t b = (uint16_t)((colour & 0x1F) * by);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Coverage rather than a hard edge, the same way the face is drawn. At this size
// a jagged corner is most of what you see of an icon.
void plot(uint16_t *fb, int16_t x, int16_t y, float coverage, uint16_t colour) {
  if (coverage <= 0.02f || x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) {
    return;
  }
  boardRow(fb, y)[boardX(x)] = boardColour(fade(colour, clamp01(coverage)));
}

// Only as wide as what is being redrawn. The bars run down both edges of the
// glass and nothing here puts them back, so clearing a whole row to repaint
// something in the middle of it takes a bite out of them.
void clearBand(uint16_t *fb, int16_t from, int16_t to, int16_t x0, int16_t x1) {
  for (int16_t y = from; y <= to; y++) {
    if (y < 0 || y >= SCREEN_H) {
      continue;
    }
    memset(boardRow(fb, y) + boardX(x1), 0, (size_t)(x1 - x0 + 1) * 2);
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

// Offline it is the same glyph in a dark grey rather than a struck-through one.
// It is next to a battery that says how it is doing by colour, so a colour is
// already the language of that corner.
void drawWifi(uint16_t *fb, uint8_t bars, bool online) {
  // How far round each arc carries. Wider than it needs to be to read as an
  // arc, because at a narrow sweep the three of them stack into a column of
  // dashes rather than into something radiating.
  constexpr float APERTURE = 0.95f;
  const float sinA = sinf(APERTURE);
  const float cosA = cosf(APERTURE);
  // Two arcs and a dot is three bars of signal. The ones the signal does not
  // reach are drawn dim rather than left out, so it is the fill that says how
  // much and the glyph keeps the same shape however weak it gets.
  uint16_t lit = online ? WHITE : SEEKING;
  uint16_t outer = bars >= 3 ? lit : DIM;
  uint16_t middle = bars >= 2 ? lit : DIM;
  uint16_t centre = bars >= 1 ? lit : DIM;

  for (int16_t y = MIDDLE - 9; y <= MIDDLE + 9; y++) {
    for (int16_t x = WIFI_X - 12; x <= WIFI_X + 12; x++) {
      // Measured from the bottom of the glyph, which is where the arcs and the
      // dot are all centred - a pixel above the middle line, so the icon sits
      // level with the battery rather than hanging under it.
      float px = (float)x + 0.5f - WIFI_X;
      float py = (float)y + 0.5f - (MIDDLE + 5.0f);

      // A pixel further off than the rest. The outer arc is the longest of the
      // three and reads as crowding the one below it at the same spacing.
      plot(fb, x, y, 0.5f - sdArc(px, py + 1.0f, sinA, cosA, 11.0f, 1.6f), outer);
      // Closer in than it looks like it should be. The gap that reads as right
      // is the one between the arc's inner edge and the dot, not between their
      // centres, and the thickness of the arc eats most of it.
      plot(fb, x, y, 0.5f - sdArc(px, py, sinA, cosA, 6.4f, 1.6f), middle);
      plot(fb, x, y, 0.5f - (sqrtf(px * px + py * py) - 2.5f), centre);
    }
  }
}

uint16_t batteryColour(const BatteryState &battery, bool blink) {
  if (battery.charging) {
    return GREEN;
  }
  if (battery.percent <= RED_AT) {
    return blink ? RED : 0x3800;
  }
  if (battery.percent <= ORANGE_AT) {
    return ORANGE;
  }
  if (battery.percent <= YELLOW_AT) {
    return YELLOW;
  }
  return WHITE;
}

// Where the bars fall. Anything at all is the dot, and each ring above it is
// roughly another dozen decibels of margin - a room away, and a floor away.
//
// With nothing joined they fill outward and start over instead, because the
// radio is still scanning and this is the only thing on the glass that says
// so. A still icon there reads as having given up.
uint8_t wifiBars(bool online) {
  if (!online) {
    return (uint8_t)((millis() / 350) % 4);
  }
  int rssi = wifiRssi();
  if (rssi >= -60) {
    return 3;
  }
  return rssi >= -72 ? 2 : 1;
}

}  // namespace

void statusDraw(uint16_t *fb, int16_t faceFrom, int16_t faceTo) {
  BatteryState battery = batteryRead();
  // Only worth the room while somebody still has to type the token into it.
  const char *address = usageReady() ? nullptr : wifiAddress();
  bool online = wifiConnected();
  uint8_t bars = wifiBars(online);
  bool blink = battery.percent <= RED_AT && !battery.charging
                   ? ((millis() / 450) & 1) != 0
                   : false;

  uint8_t rise = battery.charging ? (uint8_t)((millis() / RISE_MS) % RISE_STEPS) : 0;
  uint8_t level = battery.charging
                      ? (uint8_t)(battery.percent + (100 - battery.percent) * rise / RISE_STEPS)
                      : battery.percent;

  bool topChanged = battery.percent != shown.percent || battery.charging != shown.charging ||
                    battery.present != shown.present || bars != shown.bars ||
                    blink != shown.blink || rise != shown.rise;
  // Whatever the face just painted over is gone, whether it changed or not.
  topChanged |= overlaps(bandFrom(BAR_BOTTOM), bandTo(BAR_TOP), faceFrom, faceTo);

  // The countdown stands where the address does, which costs nothing: the
  // address is only up until a token is, and the figures have nothing to say
  // before that, so the two are never both wanted. It is the session window
  // that is counted down - the week only takes it over once the week is the
  // one that has run out.
  // It comes up as the bars go in and goes out as they come back, which is the
  // same movement read from the other end - and it is why this is not a state
  // but a level.
  char clock[16] = {0};
  float up = gaugeFiguresLevel();
  if (up > 0.02f) {
    uint32_t left = usageWeekly() >= SPENT ? usageWeeklyResetsIn() : usageSessionResetsIn();
    if (left > 0) {
      clockOf(clock, sizeof(clock), left);
    }
  }
  bool ticking = clock[0] != '\0';

  const char *want = ticking ? clock : (address ? address : "");
  // An address going away is a change like any other, and the only one where
  // nothing new is typed - so it has to say so itself, or the old one is left
  // sitting there with no clock running to wipe it.
  bool rewrite = strncmp(want, shown.address, sizeof(shown.address)) != 0;
  if (rewrite) {
    strncpy(shown.address, want, sizeof(shown.address) - 1);
    shown.address[sizeof(shown.address) - 1] = '\0';
    typed = 0;
    typedAt = millis();
  }

  uint8_t full = (uint8_t)strlen(shown.address);
  // Clamped before it is narrowed, not after. A byte of it wraps every two
  // hundred and fifty-six steps, which is a whole address retyping itself out
  // of nowhere every eleven seconds.
  uint32_t steps = (millis() - typedAt) / 45;
  // A clock is not typed out. It rewrites itself every frame, and a reveal that
  // starts over with it would never get past its first glyph.
  uint8_t reveal = ticking || steps >= full ? full : (uint8_t)steps;
  bool bottomChanged = reveal != typed || rewrite;
  bottomChanged |= overlaps(bandFrom(BOTTOM_TO), bandTo(BOTTOM_FROM), faceFrom, faceTo);

  if (!topChanged && !bottomChanged) {
    return;
  }
  typed = reveal;

  if (topChanged) {
    clearBand(fb, BAR_TOP, BAR_BOTTOM, WIFI_X - 14, BATTERY_X + 18);
    if (battery.present) {
      // The bar inside it already says how much, so the number said it twice.
      drawBattery(fb, level, batteryColour(battery, blink));
    }
    drawWifi(fb, bars, online);
    boardFlushRows(bandFrom(BAR_BOTTOM), bandTo(BAR_TOP));
    shown.percent = battery.percent;
    shown.charging = battery.charging;
    shown.present = battery.present;
    shown.bars = bars;
    shown.blink = blink;
    shown.rise = rise;
  }

  if (bottomChanged) {
    // Only as wide as the words themselves, rather than as wide as the longest
    // line there could be. The bars' bottom ends reach in to about x=89 through
    // these rows once they are at full length, and a band sized for the worst
    // case takes a bite out of them for the sake of room nothing is using.
    int16_t step = textStep(BOTTOM_SCALE);
    int16_t half = full > 0 ? (int16_t)((full * step) / 2 + 2) : 0;
    int16_t widest = half > wiped ? half : wiped;
    wiped = half;
    if (widest > 0) {
      clearBand(fb, BOTTOM_FROM, BOTTOM_TO, (int16_t)(SCREEN_R - widest),
                (int16_t)(SCREEN_R + widest));
    }
    if (typed > 0) {
      memcpy(typing, shown.address, typed);
      typing[typed] = '\0';
      // Centred on where the whole line will be, so it does not slide left as
      // it arrives.
      int16_t left = (int16_t)(SCREEN_R - (full * step) / 2);
      textDraw(fb, typing, (int16_t)(left + (typed * step) / 2), ADDRESS_Y, BOTTOM_SCALE,
               boardColour(fade(WHITE, ticking ? up : 1.0f)));
    }
    boardFlushRows(bandFrom(BOTTOM_TO), bandTo(BOTTOM_FROM));
  }
}
