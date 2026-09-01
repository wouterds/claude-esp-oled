#include "info.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "audio.h"
#include "battery.h"
#include "board.h"
#include "gauge.h"
#include "load.h"
#include "nyan.h"
#include "shape.h"
#include "text.h"
#include "version.h"
#include "wifi.h"

namespace {

// The charge, as a track with a filled part rather than as a picture of a
// battery. A drawn cell says "battery" on its own and needs no title, but it is
// the one thing on this page shaped like an object rather than like a reading -
// and beside two dials it read as an illustration sat between two instruments.
// A bar and a title say the same thing in the same language as the rest.
//
// Under the dials rather than between them, and as wide as the pair of them are
// across: their arcs reach out to 46 and 314, so a bar drawn to the same two
// numbers lines its ends up with theirs and the three read as one block instead
// of as a row of three things.
//
// On the middle line of the glass, which is the one row a round panel is widest
// on - a full width bar anywhere else is a full width bar with the corners of
// the circle taken out of it.
// Everything on this page is built about this rather than about SCREEN_R. The
// three dials and the bar have to share one axis, or the outer two sit half a
// pixel off the ends of the bar they are meant to line up with.
constexpr float MIDDLE = (float)(SCREEN_W - 1) * 0.5f;
constexpr float BAR_X = MIDDLE;
// Further from its title on the LCD board, whose glass is the tighter: the bar
// goes down three and the title up three.
#if defined(BOARD_LCD_185B)
constexpr float BAR_DOWN = 3.0f;
constexpr float BAR_TITLE_UP = 3.0f;
#else
constexpr float BAR_DOWN = 0.0f;
constexpr float BAR_TITLE_UP = 0.0f;
#endif
constexpr float BAR_Y = 236.0f * SCENE + BAR_DOWN;
constexpr float BAR_HW = 134.0f * SCENE;
// Thin, because it is long now. The old block was nine half-high and eighty
// across; at three times the length the same thickness reads as a slab rather
// than as a reading.
constexpr float BAR_HH = 5.0f * SCENE;
// Fully round ends. The radius is the half height, which is what makes the
// track a lozenge rather than a rectangle with softened corners.
constexpr float BAR_R = 5.0f * SCENE;
// Above the bar, where the dials carry theirs underneath - each title sits on
// the side its own shape leaves room on.
constexpr int16_t BAR_TITLE_TOP = (int16_t)(212 * SCENE - BAR_TITLE_UP);

// The two dials, one either side of the bar. A half circle standing on its
// flat side rather than a bar: the bars down the edges of the face already mean
// "a window filling up", and these are not that - they are what the board is
// doing right now, and a different shape is what says so before the titles are
// read.
//
// Pulled in from the cell and up off the lines under it. What runs out first is
// not the glass - the far top corner of the left dial's box lands 166 out from
// the middle of a panel that stops at 180, and the band itself is nowhere near
// that - it is the star field overhead, which these grow up into. The assert
// under it is what says so.
constexpr float DIAL_R = 35.0f * SCENE;
// Half the band's thickness. The bars on the face are 3.5 the same way, so the
// two read as the same weight of line at a glance.
constexpr float DIAL_T = 3.5f * SCENE;
// A few rows up on the LCD board, whose glass is the tighter. The titles do not
// come with it - the lift goes into the gap under the number instead.
#if defined(BOARD_LCD_185B)
constexpr float DIAL_LIFT = 4.0f;
#else
constexpr float DIAL_LIFT = 0.0f;
#endif
constexpr float DIAL_Y = 154.0f * SCENE - DIAL_LIFT;
// Centre to centre. Wider than the bar under them rather than flush with its
// ends, which is what spreads the three across the glass instead of huddling
// them over it.
//
// The ceiling is the top outer corner of an outer dial's box: the glass is round
// and that corner is the furthest point from the middle of it, so it is what
// runs out of panel first. At this row it lands 166 out of the 180 there is.
constexpr float DIAL_STEP = 112.0f * SCENE;
constexpr int16_t DIAL_SCALE = 2;
// How far round from straight up each end reaches. Ninety is a half circle by
// construction - and does not read as one. The ends are round, so the last few
// degrees taper into the diameter rather than meeting it square, and the eye
// closes the shape short of the line. A few degrees over and the caps cross it,
// which is what makes it look like the half circle it already was.
//
// Written out rather than worked out: this has to be constexpr for the box below
// and there is no cosf at compile time.
constexpr float DIAL_SWEEP_SIN = 0.99027f;   // sin(98 degrees)
constexpr float DIAL_SWEEP_COS = -0.13917f;  // cos(98 degrees)
constexpr float DIAL_SWEEP_RAD = 1.71042f;   // 98 degrees
// How far below the middle line the ends now hang, cap and all. Everything under
// the dial is placed off this rather than off the radius, so the sweep stays one
// number to change.
constexpr float DIAL_DROP = DIAL_R * -DIAL_SWEEP_COS + DIAL_T;
// The glyphs do not scale with the glass - they are the same size on either
// panel, the way the figures beside the bars are - so the number is placed by
// its own height rather than by a fraction of the bowl holding it.
// Lower in the bowl than it was. The glyphs are the same size whatever the
// glass, so a smaller dial does not get a smaller number - and higher up, the
// bowl narrows to less than the 46 pixels "100%" needs.
//
// Low enough to sit with the title rather than with the band. The band curves
// away either side of the number and the title does not, so a number hung
// halfway between the two by the ruler reads as pinned to the arc and adrift
// from the word underneath it.
constexpr int16_t DIAL_NUMBER_UP = (int16_t)(13 * SCENE);
constexpr int16_t DIAL_TITLE_DOWN = (int16_t)(DIAL_DROP + 5.0f * SCENE + DIAL_LIFT);
// Its own box, which is what gets cleared and sent when only these two moved.
constexpr int16_t DIAL_HW = (int16_t)(DIAL_R + DIAL_T + 2.0f);
constexpr int16_t DIAL_TOP = (int16_t)(DIAL_Y - DIAL_R - DIAL_T - 2.0f);
constexpr int16_t DIAL_BOTTOM =
    (int16_t)(DIAL_Y + (float)DIAL_TITLE_DOWN + (float)(7 * DIAL_SCALE) + 2.0f);

constexpr int16_t NETWORK_TOP = (int16_t)(272 * SCENE);
constexpr int16_t ADDRESS_TOP = (int16_t)(296 * SCENE);
// Up a few rows on the LCD board, whose glass is the tighter down there.
#if defined(BOARD_LCD_185B)
constexpr int16_t COMMIT_UP = 3;
#else
constexpr int16_t COMMIT_UP = 0;
#endif
constexpr int16_t COMMIT_TOP = (int16_t)(338 * SCENE) - COMMIT_UP;
constexpr int16_t LINE_SCALE = 2;
// What fits between the edges of the glass down there, in glyphs. A network can
// be called anything up to thirty-two characters and the ones that long run off
// both sides of the circle.
constexpr uint8_t LINE_GLYPHS = 24;

// The sprite above the dials, at a whole multiple like the glyphs are - a
// fractional one would have to pick which of its columns to double, and pixel
// art does not survive that. Its cells and palette come out of assets/nyan.gif
// at build time; nyan.py is what knows how, and nothing here knows what it is
// drawing beyond how big it is.
constexpr int16_t NYAN_SCALE = 2;
constexpr int16_t NYAN_DRAW_W = (int16_t)(NYAN_W * NYAN_SCALE);
constexpr int16_t NYAN_DRAW_H = (int16_t)(NYAN_H * NYAN_SCALE);
constexpr float NYAN_Y = 52.0f * SCENE;

// The cat with the glass to itself, on a double tap. Three quarters of the
// biggest whole multiple that keeps the cat's own box inside the circle - full
// size left it filling the glass corner to corner with no room round it at
// all. Only the cat is measured: the trail behind it is meant to leave the
// glass, and the flame on the end of it lands well past the edge.
constexpr int16_t BIG_SCALE = (int16_t)(6.0f * SCENE);
constexpr float BIG_HW = (float)(NYAN_CAT_W * BIG_SCALE) * 0.5f;
constexpr float BIG_HH = (float)(NYAN_H * BIG_SCALE) * 0.5f;
static_assert(BIG_HW * BIG_HW + BIG_HH * BIG_HH < SCREEN_R * SCREEN_R,
              "the cat would not fit the glass");

// The field the cat is running through. Not out of the GIF - these are the
// board's own, so they can cross the whole panel rather than being stuck inside
// the sprite's box, and so they can turn round when it does.
constexpr uint8_t STARS = 14;
// The band's fourteen, over the whole panel instead of a quarter of it.
constexpr uint8_t BIG_STARS = 48;
constexpr int16_t STAR_SCALE = 2;
// A twinkle is five cells square and is drawn about its middle, so it reaches
// this far either side of wherever it is put. The band below is cleared and sent
// as a whole; a star placed hard against the edge of it would paint these rows
// past that edge, where nothing ever clears them again - which is a line of
// specks left standing on the glass.
constexpr int16_t STAR_REACH = 3 * STAR_SCALE;
// How far above and below the sprite they range. Below, raw pixels rather than
// scaled ones, because the sprite they are sized against is raw too - and wider
// than the cat, so the field reads as something it is inside rather than as a
// line it is on.
//
// Above, the glass runs out before anything else does: there is nothing up
// there for the field to collide with, and a field that stops on a straight
// line of its own has an edge the round panel never gave it. Row nought is the
// top of the bounding square rather than of the circle, so the last few rows
// hold nothing - starSpan is what says which ones, and it says it per row
// rather than being told once here.
constexpr int16_t SCENE_TOP = 0;
constexpr int16_t SCENE_BOTTOM = (int16_t)NYAN_Y + 43;
// The band is cleared and sent whole, so anything it reaches down into is
// something it wipes and does not put back. The dials are the next thing down.
static_assert(SCENE_BOTTOM < DIAL_TOP, "the star band would wipe the dials");
// Fast enough that the sliding is smooth and slow enough that this page is not
// sending a band of panel sixty times a second for a dozen specks.
constexpr uint32_t SCENE_MS = 50;

// While the cable is in, the fill itself climbs from the charge that is there
// to full and starts over - the same fill, not a second one drawn behind it, so
// what it looks like is the bar filling. Two dozen steps over a second and a
// half is slow enough to read as filling rather than as flashing.
constexpr uint8_t RISE_STEPS = 24;
constexpr uint32_t RISE_MS = 60;

constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t GREY = 0x8410;
constexpr uint16_t FAINT = 0x4A49;
// The one colour here that is not off the ramp, and the only one that means a
// state rather than a level: the cable is in. It is the green the small battery
// on the face goes for the same reason, so it means one thing on either page.
constexpr uint16_t GREEN = 0x07F6;
// A full pack is still charging as far as the gauge is concerned - the cable is
// in and nothing is being drawn from it - so this is what says the fill has
// nowhere left to climb.
constexpr uint8_t FULL_AT = 100;

struct Shown {
  uint8_t percent;
  bool charging;
  bool present;
  uint8_t rise;
  uint8_t cpu;
  uint8_t ram;
  uint8_t signal;
  char network[33];
  char address[16];
};

Shown shown = {255, false, false, 0, 255, 255, 255, {0}, {0}};
bool fresh = true;
bool big = false;

// A half circle standing on its flat side. Within the sweep it is the band
// itself: the distance to a circle of this radius, folded about zero so both
// edges of the band fall out of one number, then taken in by half its
// thickness. Past the sweep there is no band at all, only the two ends, so what
// a pixel out there is near is whichever cap is nearer - and taking it that way
// is what rounds the ends without a second shape for them.
//
// Which side of the end a pixel falls is a cross product against the ray the end
// sits on. Mirrored about the vertical first, so one ray answers for both ends.
float sdArc(float px, float py, float r, float half, float midSin, float midCos,
            float spanSin, float spanCos) {
  // Turned into the arc's own frame first, so one shape answers for both the
  // dial and the fill inside it. The dial is symmetric about straight up; the
  // fill is not - it runs from one end round to wherever the reading got to -
  // and mirroring about the wrong axis is what makes an asymmetric arc need a
  // shape of its own.
  float qx = px * midCos + py * midSin;
  float qy = py * midCos - px * midSin;
  float ax = fabsf(qx);
  if (spanSin * qy + spanCos * ax <= 0.0f) {
    return fabsf(sqrtf(ax * ax + qy * qy) - r) - half;
  }
  float ex = ax - r * spanSin;
  float ey = qy + r * spanCos;
  return sqrtf(ex * ex + ey * ey) - half;
}

float sdDial(float px, float py, float r, float half) {
  return sdArc(px, py, r, half, 0.0f, 1.0f, DIAL_SWEEP_SIN, DIAL_SWEEP_COS);
}

#if NYAN_FRAMES > 0
uint8_t nyanAt = 0;
uint32_t nyanSince = 0;
// Which way it is going. The sprite is only drawn one way round; reading its
// cells backwards is the other, and costs a subtraction on a pixel that was
// being fetched anyway.
bool leftward = false;

// Four stages of a twinkle, five by five, a bit to a column with the low one on
// the left: a speck, a cross, a bigger cross, and the ring it breaks into before
// it goes out.
constexpr uint8_t STAR_ART[4][5] = {
    {0x00, 0x00, 0x04, 0x00, 0x00},
    {0x00, 0x04, 0x0E, 0x04, 0x00},
    {0x04, 0x04, 0x1F, 0x04, 0x04},
    {0x00, 0x0A, 0x00, 0x0A, 0x00},
};

struct Star {
  float x;
  int16_t y;
  float speed;
  float twinkle;
  // How far either side of the middle this one may go and still be whole. Its
  // own, because it depends on the row it is on.
  float span;
};
Star stars[BIG_STARS];
uint8_t starCount = STARS;
uint32_t sceneSince = 0;

float wander(float lo, float hi) {
  return lo + (hi - lo) * (float)random(0, 10001) * 0.0001f;
}

// How far either side of the middle a star can sit on this row and still be all
// there. The glass is round, so a row near the top of the band is a good deal
// narrower than the panel is: a star out towards the side of one is over the
// edge of the circle, and the corners of a circle are not dark, they are absent.
// What lands there is the half of the star that is on the glass.
float starSpan(int16_t y) {
  float dy = fabsf((float)y - MIDDLE) + (float)STAR_REACH;
  float across = SCREEN_R * SCREEN_R - dy * dy;
  if (across <= 0.0f) {
    return -1.0f;
  }
  return sqrtf(across) - (float)STAR_REACH;
}

// `seed` scatters it along its row; otherwise it comes on at the edge it is
// travelling from.
void starPlace(Star &s, float dir, bool seed) {
  // Its own reach kept inside the band as well, so everything drawn is inside
  // what gets cleared. The whole panel when the cat has it.
  float topRow = (float)((big ? 0 : SCENE_TOP) + STAR_REACH);
  float bottomRow = (float)((big ? SCREEN_H - 1 : SCENE_BOTTOM) - STAR_REACH);
  float span = -1.0f;
  for (uint8_t tries = 0; tries < 8 && span < (float)STAR_REACH; tries++) {
    s.y = (int16_t)wander(topRow, bottomRow);
    span = starSpan(s.y);
  }
  // Eight rolls and none of them fit is the very top of the glass, where the
  // circle has all but closed. Put it back on the sprite's own row, which is
  // the widest there is - the row it last rolled is one a star hangs off the
  // side of, and calling its span the star's own width does not put it back on
  // the panel, it only stops the loop complaining.
  if (span < (float)STAR_REACH) {
    s.y = (int16_t)NYAN_Y;
    span = starSpan(s.y);
  }
  s.span = span;
  // A spread of speeds and not one speed, or the field moves like a wall rather
  // than like things at different distances.
  s.speed = wander(45.0f, 115.0f);
  s.twinkle = wander(0.0f, 4.0f);
  // Half a pixel inside, or one placed exactly on its own limit is off the row
  // again on the tick it arrived.
  s.x = seed ? MIDDLE + wander(-span, span)
             : MIDDLE + (dir > 0.0f ? -span + 0.5f : span - 0.5f);
}

void starsSeed() {
  float dir = leftward ? 1.0f : -1.0f;
  starCount = big ? BIG_STARS : STARS;
  for (uint8_t i = 0; i < starCount; i++) {
    starPlace(stars[i], dir, true);
  }
}

// They come from wherever the cat is heading and go the other way, which is the
// whole of what makes the cat look like the thing that is moving.
void starsStep(float dt) {
  float dir = leftward ? 1.0f : -1.0f;
  for (uint8_t i = 0; i < starCount; i++) {
    stars[i].x += dir * stars[i].speed * dt;
    stars[i].twinkle += dt * 5.0f;
    while (stars[i].twinkle >= 4.0f) {
      stars[i].twinkle -= 4.0f;
    }
    // Off at the edge of the glass rather than at the edge of the panel, so it
    // is never drawn part on and part off.
    if (fabsf(stars[i].x - MIDDLE) > stars[i].span) {
      starPlace(stars[i], dir, false);
    }
  }
}

void drawStars(uint16_t *fb) {
  for (uint8_t i = 0; i < starCount; i++) {
    const uint8_t *art = STAR_ART[(uint8_t)stars[i].twinkle & 3];
    int16_t left = (int16_t)stars[i].x - 2 * STAR_SCALE;
    int16_t top = stars[i].y - 2 * STAR_SCALE;
    for (int16_t r = 0; r < 5; r++) {
      uint8_t bits = art[r];
      if (!bits) {
        continue;
      }
      for (int16_t dy = 0; dy < STAR_SCALE; dy++) {
        int16_t y = top + r * STAR_SCALE + dy;
        if (y < 0 || y >= SCREEN_H) {
          continue;
        }
        uint16_t *row = boardRow(fb, y);
        for (int16_t c = 0; c < 5; c++) {
          if (!(bits & (1 << c))) {
            continue;
          }
          for (int16_t dx = 0; dx < STAR_SCALE; dx++) {
            int16_t x = left + c * STAR_SCALE + dx;
            if (x >= 0 && x < SCREEN_W) {
              row[boardX(x)] = boardColour(WHITE);
            }
          }
        }
      }
    }
  }
}

// On the GIF's own delays rather than on the frame rate, so it runs at the speed
// it was drawn at whatever the panel is doing.
bool nyanStep() {
  uint32_t now = millis();
  if (now - nyanSince < NYAN_DELAY_MS[nyanAt]) {
    return false;
  }
  nyanSince = now;
  nyanAt = (uint8_t)((nyanAt + 1) % NYAN_FRAMES);
  return true;
}

// Nought is nothing there rather than a colour, which is the whole of what
// leaving the background out costs at this end: the cell is skipped and whatever
// the page had there stays.
void drawNyan(uint16_t *fb, int16_t scale, int16_t left, int16_t top) {
  const uint8_t *cells = NYAN_CELLS[nyanAt];
  for (int16_t sy = 0; sy < NYAN_H; sy++) {
    for (int16_t dy = 0; dy < scale; dy++) {
      int16_t y = top + sy * scale + dy;
      if (y < 0 || y >= SCREEN_H) {
        continue;
      }
      uint16_t *row = boardRow(fb, y);
      for (int16_t sx = 0; sx < NYAN_W; sx++) {
        // Whole cells off the glass are skipped rather than clipped a pixel at
        // a time: with the cat filling the panel, most of the trail is.
        int16_t x0 = (int16_t)(left + sx * scale);
        if (x0 + scale <= 0 || x0 >= SCREEN_W) {
          continue;
        }
        uint8_t v = cells[sy * NYAN_W + (leftward ? NYAN_W - 1 - sx : sx)];
        if (!v) {
          continue;
        }
        // Already in the panel's byte order, so this is a store rather than a
        // swap and a store - which at five thousand pixels a cell is worth it.
        uint16_t ink = NYAN_PALETTE[v];
        for (int16_t dx = 0; dx < scale; dx++) {
          int16_t x = (int16_t)(x0 + dx);
          if (x >= 0 && x < SCREEN_W) {
            row[boardX(x)] = ink;
          }
        }
      }
    }
  }
}

// Stars behind, cat in front.
void drawScene(uint16_t *fb) {
  drawStars(fb);
  drawNyan(fb, NYAN_SCALE, (int16_t)MIDDLE - NYAN_DRAW_W / 2, (int16_t)NYAN_Y - NYAN_DRAW_H / 2);
}

// The whole glass: stars everywhere, and the cat in the middle of it. Centred
// on the cat rather than on the sprite, so the trail runs off whichever side
// the cat is leaving - and the flame on the end of it lands well past the edge.
void sendBig(uint16_t *fb) {
  memset(fb, 0, (size_t)SCREEN_W * SCREEN_H * 2);
  drawStars(fb);
  int16_t catLeft = (int16_t)(MIDDLE - BIG_HW);
  int16_t left = leftward ? catLeft : (int16_t)(catLeft - (NYAN_W - NYAN_CAT_W) * BIG_SCALE);
  drawNyan(fb, BIG_SCALE, left, (int16_t)(MIDDLE - BIG_HH));
  boardFlush();
}

// Its own band, which nothing else is in - it sits clear above the dials. Whole
// rows, because the stars cross all of them.
void sendScene(uint16_t *fb) {
  for (int16_t y = SCENE_TOP; y <= SCENE_BOTTOM; y++) {
    if (y < 0 || y >= SCREEN_H) {
      continue;
    }
    memset(boardRow(fb, y), 0, (size_t)SCREEN_W * 2);
  }
  drawScene(fb);
  boardFlushRows((int16_t)(SCREEN_H - 1 - SCENE_BOTTOM),
                 (int16_t)(SCREEN_H - 1 - SCENE_TOP));
}

// One clock for the whole scene. The cat keeps its own inside nyanStep, so the
// two run at their own speeds off the same tick.
bool sceneStep() {
  uint32_t now = millis();
  if (now - sceneSince < SCENE_MS) {
    return false;
  }
  float dt = (float)(now - sceneSince) * 0.001f;
  // Coming back to the page after a while away, or the stars jump the width of
  // the panel on the first tick.
  if (dt > 0.25f) {
    dt = 0.25f;
  }
  sceneSince = now;
  starsStep(dt);
  nyanStep();
  return true;
}
#endif

// The radio reports dBm, which is a logarithm and a negative one. Fifty is about
// as good as this board ever sees and ninety is where a connection stops being
// one, so those are the ends. They bracket the same span the bars on the face
// quantise, so the dial and the bars never disagree about how it is doing.
uint8_t signalPercent() {
  if (!wifiConnected()) {
    return 0;
  }
  int rssi = wifiRssi();
  if (rssi >= -50) {
    return 100;
  }
  if (rssi <= -90) {
    return 0;
  }
  return (uint8_t)((rssi + 90) * 100 / 40);
}

void drawDial(uint16_t *fb, float cx, uint8_t percent, const char *title, uint16_t ink) {
  // The fill is an arc in its own right, running from the dial's start round to
  // wherever the reading got to - not a wedge cut out of the dial by a plane.
  // A plane through the middle slices the round cap on the end of the arc in
  // half, so the fill was always missing the outer half of the cap it starts
  // on, at every reading. Its own shape has its own caps and starts where the
  // dial does.
  float edge = DIAL_SWEEP_RAD * ((float)percent * 0.02f - 1.0f);
  float mid = (edge - DIAL_SWEEP_RAD) * 0.5f;
  float span = (edge + DIAL_SWEEP_RAD) * 0.5f;
  float midSin = sinf(mid);
  float midCos = cosf(mid);
  float spanSin = sinf(span);
  float spanCos = cosf(span);

  for (int16_t y = DIAL_TOP; y <= (int16_t)(DIAL_Y + DIAL_DROP + 2.0f); y++) {
    float py = (float)y + 0.5f - DIAL_Y;
    for (int16_t x = (int16_t)(cx) - DIAL_HW; x <= (int16_t)(cx) + DIAL_HW; x++) {
      float px = (float)x + 0.5f - cx;
      float cover = 0.5f - sdDial(px, py, DIAL_R, DIAL_T);
      if (cover <= 0.02f) {
        continue;
      }
      // Nought is the one case the shape cannot speak for: there the fill has no
      // length, and what is left of it is the start cap on its own - a dot on a
      // dial that is meant to be empty.
      bool full = percent > 0 &&
                  sdArc(px, py, DIAL_R, DIAL_T, midSin, midCos, spanSin, spanCos) < 0.5f;
      // The colour crosses on the fill's own edge rather than on a fade of it,
      // the same way the bars on the face do. Faded, every antialiased pixel
      // down both long edges greys off and the fill reads as fringed rather
      // than as filled.
      plot(fb, x, y, cover, full ? ink : FAINT);
    }
  }

  char said[6];
  snprintf(said, sizeof(said), "%u%%", (unsigned)percent);
  // Inside the bowl, sat on the flat side rather than centred in the circle:
  // the bottom half of that circle is not there to be centred in.
  textDraw(fb, said, (int16_t)cx, (int16_t)DIAL_Y - DIAL_NUMBER_UP, DIAL_SCALE,
           boardColour(ink));
  textDraw(fb, title, (int16_t)cx, (int16_t)DIAL_Y + DIAL_TITLE_DOWN, DIAL_SCALE,
           boardColour(GREY));
}

// Both of them, and the box each one owns. The cell stands between them and has
// not moved, so they are cleared and sent as two boxes rather than as the rows
// they share with it.
void drawDials(uint16_t *fb, bool send) {
  const float at[3] = {MIDDLE - DIAL_STEP, MIDDLE, MIDDLE + DIAL_STEP};
  const uint8_t said[3] = {loadCpu(), loadRam(), signalPercent()};
  const char *named[3] = {"CPU", "RAM", "WIFI"};
  // The first two are read as how much has gone, which is the direction the ramp
  // is built for - a big number is the bad one. Signal is the other way about, so
  // it takes its colour from what is missing instead, and a full dial comes out
  // the same teal a quiet core does.
  const bool lowIsBad[3] = {false, false, true};
  for (uint8_t i = 0; i < 3; i++) {
    int16_t x0 = (int16_t)at[i] - DIAL_HW;
    int16_t x1 = (int16_t)at[i] + DIAL_HW;
    if (send) {
      for (int16_t y = DIAL_TOP; y <= DIAL_BOTTOM; y++) {
        if (y < 0 || y >= SCREEN_H) {
          continue;
        }
        memset(boardRow(fb, y) + boardX(x1), 0, (size_t)(x1 - x0 + 1) * 2);
      }
    }
    drawDial(fb, at[i], said[i], named[i],
             gaugeColour(lowIsBad[i] ? (uint8_t)(100 - said[i]) : said[i]));
    if (send) {
      boardFlushRect((int16_t)(SCREEN_W - 1 - x1), (int16_t)(SCREEN_W - 1 - x0),
                     (int16_t)(SCREEN_H - 1 - DIAL_BOTTOM), (int16_t)(SCREEN_H - 1 - DIAL_TOP));
    }
  }
}

uint16_t chargeColour(const BatteryState &battery) {
  if (battery.charging) {
    return GREEN;
  }
  // Off the same ramp the dials and the bars on the face take their colour
  // from, turned round. On that ramp a big number is the bad one - it is built
  // for windows filling up - and a battery is the other way about, so what it is
  // asked about is how much has gone rather than how much is left. Full comes
  // out the teal that everything on this device starts at and empty comes out
  // the brand's pink, through the same yellows in between.
  return gaugeColour((uint8_t)(100 - battery.percent));
}

void drawBar(uint16_t *fb, uint8_t percent, uint16_t ink) {
  // The fill is the track's own shape shortened from the right rather than the
  // track cut off at a line, so at a few percent what is left is a lozenge and
  // not a sliver with square shoulders hanging out of a rounded end.
  float wide = BAR_HW * ((float)percent / 100.0f);
  float from = BAR_X - BAR_HW + wide;

  for (int16_t y = (int16_t)(BAR_Y - BAR_HH - 2); y <= (int16_t)(BAR_Y + BAR_HH + 2); y++) {
    for (int16_t x = (int16_t)(BAR_X - BAR_HW - 2); x <= (int16_t)(BAR_X + BAR_HW + 2); x++) {
      float px = (float)x + 0.5f - BAR_X;
      float py = (float)y + 0.5f - BAR_Y;
      float cover = 0.5f - sdRoundBox(px, py, BAR_HW, BAR_HH, BAR_R);
      if (cover <= 0.02f) {
        continue;
      }
      // The colour crosses on the fill's own edge rather than on a fade of it,
      // the same way the dials and the bars on the face do.
      bool full = percent > 0 && sdRoundBox(px - (from - BAR_X), py, wide, BAR_HH, BAR_R) < 0.5f;
      plot(fb, x, y, cover, full ? ink : FAINT);
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

void infoForget() {
  fresh = true;
#if NYAN_FRAMES > 0
  // Turned round on the way in rather than on the way out, so the page is never
  // holding a direction it has not drawn yet - and so the first opening after a
  // boot is not always the same one.
  leftward = !leftward;
  starsSeed();
#endif
}

void infoTapped(int16_t, int16_t along) {
#if NYAN_FRAMES > 0
  // On the way in it has to be the cat's own band; on the way out, anywhere -
  // the cat is everywhere.
  if (!big && along > SCENE_BOTTOM) {
    return;
  }
  big = !big;
  fresh = true;
  starsSeed();
  audioSong(big);
#endif
}

bool infoFullscreen() { return big; }

void infoStep(uint16_t *fb) {
#if NYAN_FRAMES > 0
  if (big) {
    // Whole, every tick of the scene: everything on it moves.
    if (sceneStep() || fresh) {
      fresh = false;
      sendBig(fb);
    }
    return;
  }
#endif
  BatteryState battery = batteryRead();
  const char *network = wifiNetwork();
  const char *address = wifiAddress();
  const char *wants = network ? network : "OFFLINE";
  const char *at = address ? address : "";

  // Where the charge on its way in has got to. Nought unless the cable is in and
  // there is somewhere for the fill to go: a battery sitting on its own, or one
  // already full on the cable, compares equal every frame and this page goes on
  // costing nothing. Full on the cable used to keep counting, which moved the
  // fill from a hundred to a hundred and redrew the whole page for it sixteen
  // times a second.
  bool topped = battery.percent >= FULL_AT;
  bool filling = battery.charging && !topped;
  uint8_t rise = filling ? (uint8_t)((millis() / RISE_MS) % RISE_STEPS) : 0;
  uint8_t level = filling
                      ? (uint8_t)(battery.percent + (100 - battery.percent) * rise / RISE_STEPS)
                      : battery.percent;

  bool loadMoved = loadCpu() != shown.cpu || loadRam() != shown.ram ||
                   signalPercent() != shown.signal;
#if NYAN_FRAMES > 0
  // Asked every frame, true only when the loop has actually turned over - so on
  // the frames between, this page still costs nothing.
  bool sceneMoved = sceneStep();
#else
  bool sceneMoved = false;
#endif
  bool riseMoved = rise != shown.rise;

  bool changed = fresh || battery.percent != shown.percent ||
                 battery.charging != shown.charging || battery.present != shown.present ||
                 strncmp(wants, shown.network, sizeof(shown.network)) != 0 ||
                 strncmp(at, shown.address, sizeof(shown.address)) != 0;
  if (!changed && !riseMoved && !loadMoved && !sceneMoved) {
    return;
  }
  shown.rise = rise;
  shown.cpu = loadCpu();
  shown.ram = loadRam();
  shown.signal = signalPercent();

  // Only what moved is redrawn, and only its own rows are sent. The rest of the
  // page is words that have not changed, and sending the whole panel for a
  // rising fill or a dial that gained a percent is most of a frame spent on the
  // part that stood still.
  if (!changed) {
    if (riseMoved) {
      // Only the track. The title above it says whether the cable is in, and
      // that has not changed or this would be a whole page. The bar has these
      // rows to itself now that it is under the dials rather than between them,
      // so they go back whole rather than being cut round anything.
      constexpr int16_t TOP = (int16_t)(BAR_Y - BAR_HH - 2);
      constexpr int16_t BOTTOM = (int16_t)(BAR_Y + BAR_HH + 2);
      for (int16_t y = TOP; y <= BOTTOM; y++) {
        memset(boardRow(fb, y), 0, (size_t)SCREEN_W * 2);
      }
      drawBar(fb, level, chargeColour(battery));
      boardFlushRows((int16_t)(SCREEN_H - 1 - BOTTOM), (int16_t)(SCREEN_H - 1 - TOP));
    }
    if (loadMoved) {
      drawDials(fb, true);
    }
#if NYAN_FRAMES > 0
    if (sceneMoved) {
      sendScene(fb);
    }
#endif
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

#if NYAN_FRAMES > 0
  drawScene(fb);
#endif
  drawDials(fb, false);

  uint16_t colour = chargeColour(battery);
  if (battery.present) {
    drawBar(fb, level, colour);
    // The reading sits on the label rather than under the bar. The dials carry
    // theirs inside their own bowls, which a bar has nowhere to put - and a
    // number left standing on its own below it was the one thing on this page
    // that named nothing.
    //
    // Green while the cable is in, the way the fill is, and otherwise the same
    // grey the dials name themselves in: the labels here are labels rather than
    // readings, and only the one with something to say is lit.
    char said[24];
    snprintf(said, sizeof(said), "BATTERY - %u%%", (unsigned)battery.percent);
    // Left, on the bar's own left end rather than over its middle. Centred, the
    // line moved every time the number gained or lost a digit, and a label that
    // shifts under a bar that has not is the bar that looks like it moved.
    //
    // textDraw centres on what it is given, so the left edge is turned into the
    // middle here. The last glyph carries no gap after it, which is the one
    // place the ink is not the count times the step.
    int16_t ink = (int16_t)(strlen(said) * textStep(DIAL_SCALE) - DIAL_SCALE);
    textDraw(fb, said, (int16_t)(BAR_X - BAR_HW) + ink / 2, BAR_TITLE_TOP, DIAL_SCALE,
             boardColour(battery.charging ? GREEN : GREY));
  }

  line(fb, shown.network, NETWORK_TOP, WHITE);
  if (shown.address[0]) {
    line(fb, shown.address, ADDRESS_TOP, GREY);
  }
  line(fb, BUILD_COMMIT, COMMIT_TOP, FAINT);

  boardFlush();
}
