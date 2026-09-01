#include "status.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "battery.h"
#include "board.h"
#include "gauge.h"
#include "outage.h"
#include "shape.h"
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
constexpr int16_t MIDDLE = (int16_t)(17 * SCENE);
constexpr int16_t BAR_TOP = MIDDLE - (int16_t)(10 * SCENE);
constexpr int16_t BAR_BOTTOM = MIDDLE + (int16_t)(10 * SCENE);

// Its own band, and its own flush: the status page has nothing to do with the
// two icons above it and moves on a clock of its own - a minute, not a frame.
//
// It reaches further down than the note above suggests is safe, and that note is
// the conservative one: the face's box begins at HOME_Y - ROAM - 80, which is
// well under this band.
// A step lower on the AMOLED board, by eye: the bigger glass left it floating
// between the icons above it and the face below, which keeps a clear nine rows
// under the dropped band even at the top of its drift.
constexpr int16_t ALARM_DROP = SCENE > 1.0f ? 8 : 0;
constexpr int16_t ALARM_Y = (int16_t)(63 * SCENE) + ALARM_DROP;
constexpr int16_t ALARM_TOP = (int16_t)(49 * SCENE) + ALARM_DROP;
constexpr int16_t ALARM_BOTTOM = (int16_t)(77 * SCENE) + ALARM_DROP;
constexpr int16_t ALARM_HALF = (int16_t)(15 * SCENE);

// The pair centred on the panel, wifi then battery. The glass is a circle: at
// this height it gives about 80 pixels either side of the middle, and the two
// of them together come to sixty-odd.
constexpr int16_t WIFI_X = (int16_t)(160 * SCENE);
constexpr int16_t BATTERY_X = (int16_t)(199 * SCENE);

// Held off the rim rather than scaled from the top: the glyphs are the same
// size on either glass, so what reads the same is the same margin of glass
// beneath them - scaled instead, the line floats higher the bigger the panel
// gets. Fifty is the fourteen rows of type plus the thirty-six of margin it
// has always had, so on the LCD board this is still row 310.
constexpr int16_t ADDRESS_Y = (int16_t)(SCREEN_H - 50);
constexpr int16_t BOTTOM_SCALE = 2;
constexpr int16_t BOTTOM_FROM = ADDRESS_Y - 3;
constexpr int16_t BOTTOM_TO = ADDRESS_Y + 16;

constexpr uint16_t WHITE = 0xFFFF;
// Only the triangle with nothing to report wears this, and it sits well under
// the floor DIM describes below. That floor was written about the wifi ring,
// which is arcs a pixel and a half thick; a filled shape this size holds on a
// good way further down. Exactly how far is a question for the glass rather
// than for a number, and this is where looking at it left it.
constexpr uint16_t GREY = 0x3186;
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
// two are one idea seen twice rather than two animations. It says charge is on
// its way in, so there has to be somewhere for it to go: at FULL there is not,
// and a climb from full to full is a redraw every sixty milliseconds that
// changes nothing. Topped up on the cable is a state rather than something in
// progress, and it stands still and stays green.
constexpr uint8_t RISE_STEPS = 24;
constexpr uint32_t RISE_MS = 60;
constexpr uint8_t FULL_AT = 100;

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
  uint8_t alarm;
  bool alarmLit;
  char address[16];
};

Shown shown = {255, 255, false, false, false, 0, 255, false, {0}};

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

// What the bottom line is about, as opposed to what it says. The typewriter is
// keyed off this: a countdown rewrites itself every frame, and a reveal keyed off
// the text restarts with it and never reaches a second glyph.
enum class Line : uint8_t { Empty, Address, Clock, Alarm };
Line line = Line::Empty;

// One slot and two things worth putting in it, so they take turns - long enough
// on the clock to watch it move, long enough on the outage to read it twice.
constexpr uint32_t ALARM_MS = 5000;
constexpr uint32_t ALARM_CYCLE_MS = 35000;

// Five quick dips and then ten seconds steady, over and over. It has to catch an
// eye that is not looking, and a shape that never stops blinking stops being a
// warning after the first minute of an outage that runs for hours.
//
// A dip is three frames at the rate this panel actually runs. Much under that
// and which frames it lands on starts to show as a stutter rather than a beat.
constexpr uint32_t ALARM_DIP_MS = 120;
constexpr uint32_t ALARM_DIPS = 5;
constexpr uint32_t ALARM_STEADY_MS = 10000;
constexpr uint32_t ALARM_BEAT_MS = ALARM_DIPS * 2 * ALARM_DIP_MS + ALARM_STEADY_MS;
// When the turn-taking started. Reset whenever the level moves, so a fresh
// outage says so at once instead of sitting out the rest of the clock's turn.
uint32_t alarmSince = 0;

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
  // Measured in the scene's own units, with the sampling scaled rather than the
  // shape. This glyph is a dozen offsets hung off one centre - a wall, a fill
  // inset, a nub - and scaling each of them by hand is a dozen chances to get
  // one wrong, which is a battery whose wall is thinner than its own outline on
  // one board and not the other.
  constexpr float HW = 11.0f;
  constexpr float HH = 6.0f;
  constexpr float R = 3.0f;
  constexpr float WALL = 1.4f;
  float fillTo = -HW + 1.6f + (2.0f * HW - 3.2f) * (percent / 100.0f);

  // Reach in the panel's pixels, far enough ahead to hold the nub off the end.
  // Reach enough to hold the shell's wall, the nub off its end and the pixel of
  // edge past both. A box that stops on the shape cuts it off square.
  constexpr int16_t BACK = (int16_t)(14 * SCENE);
  constexpr int16_t AHEAD = (int16_t)(17 * SCENE);
  constexpr int16_t TALL = (int16_t)(9 * SCENE);

  for (int16_t y = MIDDLE - TALL; y <= MIDDLE + TALL; y++) {
    for (int16_t x = BATTERY_X - BACK; x <= BATTERY_X + AHEAD; x++) {
      float px = ((float)x + 0.5f - BATTERY_X) / SCENE;
      float py = ((float)y + 0.5f - MIDDLE) / SCENE;

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
      // Back into panel pixels, so the soft edge stays one pixel wide.
      plot(fb, x, y, 0.5f - d * SCENE, colour);
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

  // The outer arc is the widest thing here: eleven of radius and most of two of
  // thickness, and a pixel of edge past that again - so the box has to reach
  // nearly fourteen, and higher still because the arcs sit above the middle.
  // Short, the arc is cut off flat at the top and the ends, which reads as a
  // notch taken out of the glyph rather than as a box that is too small.
  constexpr int16_t WIDE = (int16_t)(14 * SCENE);
  constexpr int16_t TALL = (int16_t)(11 * SCENE);
  for (int16_t y = MIDDLE - TALL; y <= MIDDLE + TALL; y++) {
    for (int16_t x = WIFI_X - WIDE; x <= WIFI_X + WIDE; x++) {
      // Measured from the bottom of the glyph, which is where the arcs and the
      // dot are all centred - a pixel above the middle line, so the icon sits
      // level with the battery rather than hanging under it.
      float px = ((float)x + 0.5f - WIFI_X) / SCENE;
      float py = ((float)y + 0.5f - MIDDLE) / SCENE - 5.0f;

      // A pixel further off than the rest. The outer arc is the longest of the
      // three and reads as crowding the one below it at the same spacing.
      // Nearest wins. Drawn one after another, each arc paints its own faint
      // edge over whatever the one before left solid, and where they meet that
      // is a dark seam rather than a join.
      float dOuter = sdArc(px, py + 1.0f, sinA, cosA, 11.0f, 1.6f);
      // Closer in than it looks like it should be. The gap that reads as right
      // is the one between the arc's inner edge and the dot, not between their
      // centres, and the thickness of the arc eats most of it.
      float dMiddle = sdArc(px, py, sinA, cosA, 6.4f, 1.6f);
      float dCentre = sqrtf(px * px + py * py) - 2.5f;
      float d = dOuter;
      uint16_t ink = outer;
      if (dMiddle < d) { d = dMiddle; ink = middle; }
      if (dCentre < d) { d = dCentre; ink = centre; }
      plot(fb, x, y, 0.5f - d * SCENE, ink);
    }
  }
}

// Filled, with the bang taken out of it rather than laid over it. The panel
// cannot switch a pixel off, so a black glyph on a lit shape comes out grey -
// taking the shape away is the only black on offer here, and it leaves the
// backlight to answer for it.
void drawAlarm(uint16_t *fb, uint16_t colour) {
  // Getting on for a quarter of the height goes on the corners. Reach is HH + R
  // down and HW + R across, which is what the band and ALARM_HALF come from.
  constexpr float HW = 8.5f;
  constexpr float HH = 7.5f;
  constexpr float R = 5.0f;

  for (int16_t y = ALARM_TOP; y <= ALARM_BOTTOM; y++) {
    for (int16_t x = (int16_t)(SCREEN_R - ALARM_HALF); x <= (int16_t)(SCREEN_R + ALARM_HALF); x++) {
      float px = ((float)x + 0.5f - SCREEN_R) / SCENE;
      float py = ((float)y + 0.5f - ALARM_Y) / SCENE;

      float shell = sdTriangle(px, py, HW, HH) - R;
      float bar = sdRoundBox(px, py + 1.9f, 1.7f, 4.0f, 1.7f);
      float dot = sqrtf(px * px + (py - 6.4f) * (py - 6.4f)) - 1.8f;
      float bang = bar < dot ? bar : dot;
      plot(fb, x, y, 0.5f - (shell > -bang ? shell : -bang) * SCENE, colour);
    }
  }
}

uint16_t alarmColour(Outage level) {
  switch (level) {
    case Outage::Major:
      return RED;
    case Outage::Partial:
      return YELLOW;
    default:
      return GREY;
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

  bool topped = battery.percent >= FULL_AT;
  uint8_t rise = battery.charging && !topped ? (uint8_t)((millis() / RISE_MS) % RISE_STEPS) : 0;
  uint8_t level = battery.charging
                      ? (uint8_t)(battery.percent + (100 - battery.percent) * rise / RISE_STEPS)
                      : battery.percent;

  bool topChanged = battery.percent != shown.percent || battery.charging != shown.charging ||
                    battery.present != shown.present || bars != shown.bars ||
                    blink != shown.blink || rise != shown.rise;
  // Whatever the face just painted over is gone, whether it changed or not.
  topChanged |= overlaps(bandFrom(BAR_BOTTOM), bandTo(BAR_TOP), faceFrom, faceTo);

  Outage alarm = outageLevel();
  bool outage = alarm == Outage::Partial || alarm == Outage::Major;
  bool levelMoved = (uint8_t)alarm != shown.alarm;
  if (levelMoved) {
    alarmSince = millis();
  }
  // Dips on the odd beats of the opening run, then holds lit for the rest. It
  // dims rather than goes out: a pixel cannot be switched off on this panel, so
  // dipping to black dips to grey and reads as a fault rather than a warning.
  uint32_t beat = (millis() - alarmSince) % ALARM_BEAT_MS;
  bool lit = !outage || beat >= ALARM_DIPS * 2 * ALARM_DIP_MS ||
             ((beat / ALARM_DIP_MS) & 1) == 0;
  bool alarmChanged = levelMoved || lit != shown.alarmLit;
  alarmChanged |= overlaps(bandFrom(ALARM_BOTTOM), bandTo(ALARM_TOP), faceFrom, faceTo);

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

  // The outage borrows the line for five seconds in every thirty-five, and only
  // while the clock is up to lend it. Off the clock there is nothing to hand it
  // back to, so the turn would be a five second blink at an empty line - and
  // when nobody has asked for the text, the triangle is what says so.
  bool sounding = ticking && outage;
  bool alarmTurn = sounding && (millis() - alarmSince) % ALARM_CYCLE_MS < ALARM_MS;

  Line subject = Line::Empty;
  const char *want = "";
  if (alarmTurn) {
    subject = Line::Alarm;
    // Fourteen glyphs at this size come to 86 either side of the middle and the
    // bars' bottom ends reach in to about 89, so that is the ceiling for a line
    // here. These two are twelve.
    want = alarm == Outage::Major ? "MAJOR OUTAGE" : "MINOR OUTAGE";
  } else if (ticking) {
    subject = Line::Clock;
    want = clock;
  } else if (address) {
    subject = Line::Address;
    want = address;
  }

  // A line going away is a change like any other, and the only one where nothing
  // new is typed - so it has to say so itself, or the old one is left sitting
  // there with nothing running to wipe it.
  bool rewrite = strncmp(want, shown.address, sizeof(shown.address)) != 0;
  if (rewrite) {
    strncpy(shown.address, want, sizeof(shown.address) - 1);
    shown.address[sizeof(shown.address) - 1] = '\0';
  }
  if (subject != line) {
    line = subject;
    typed = 0;
    typedAt = millis();
  }

  uint8_t full = (uint8_t)strlen(shown.address);
  // Clamped before it is narrowed, not after. A byte of it wraps every two
  // hundred and fifty-six steps, which is a whole line retyping itself out of
  // nowhere every eleven seconds.
  uint32_t steps = (millis() - typedAt) / 45;
  uint8_t reveal = steps >= full ? full : (uint8_t)steps;
  bool bottomChanged = reveal != typed || rewrite;
  bottomChanged |= overlaps(bandFrom(BOTTOM_TO), bandTo(BOTTOM_FROM), faceFrom, faceTo);

  if (!topChanged && !alarmChanged && !bottomChanged) {
    return;
  }
  typed = reveal;

  if (topChanged) {
    clearBand(fb, BAR_TOP, BAR_BOTTOM, WIFI_X - (int16_t)(14 * SCENE),
            BATTERY_X + (int16_t)(19 * SCENE));
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

  if (alarmChanged) {
    clearBand(fb, ALARM_TOP, ALARM_BOTTOM, (int16_t)(SCREEN_R - ALARM_HALF),
              (int16_t)(SCREEN_R + ALARM_HALF));
    // Up from the first frame, grey, and grey covers both not having looked yet
    // and having looked and found nothing. It is the shape that says where the
    // news will appear, so it is worth more standing there dark than it is
    // missing - and an empty space is not a quieter warning, it is no warning.
    uint16_t ink = alarmColour(alarm);
    drawAlarm(fb, lit ? ink : fade(ink, 0.25f));
    boardFlushRows(bandFrom(ALARM_BOTTOM), bandTo(ALARM_TOP));
    shown.alarm = (uint8_t)alarm;
    shown.alarmLit = lit;
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
      // The outage line wears the triangle's own colour, so the two are one
      // thing said twice rather than two things that happen to agree.
      uint16_t ink = line == Line::Alarm ? fade(alarmColour(alarm), up)
                                        : fade(WHITE, ticking ? up : 1.0f);
      textDraw(fb, typing, (int16_t)(left + (typed * step) / 2), ADDRESS_Y, BOTTOM_SCALE,
               boardColour(ink));
    }
    boardFlushRows(bandFrom(BOTTOM_TO), bandTo(BOTTOM_FROM));
  }
}
