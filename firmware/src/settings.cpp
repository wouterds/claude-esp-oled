#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

#include "audio.h"
#include "board.h"
#include "gauge.h"
#include "panel.h"
#include "shape.h"
#include "text.h"
#include "touch.h"

namespace {

// Two of them about the middle line, which is the one row a round glass is
// widest on - a full width track anywhere else has the corners of the circle
// taken out of it. Built about the same axis the details page uses, for the
// same reason: the tracks have to share it or they sit half a pixel apart.
constexpr float MIDDLE = (float)(SCREEN_W - 1) * 0.5f;
constexpr float TRACK_X = MIDDLE;
constexpr float TRACK_HW = 120.0f * SCENE;
// A thin track under a knob several times its height, which is the proportion
// that reads as something to be dragged. Nothing is aimed at the track itself -
// REACH below is what answers to a thumb - so it is free to be this slight.
constexpr float TRACK_HH = 2.5f * SCENE;
// Its own half-height, so the ends are semicircles rather than a box with the
// corners taken off.
constexpr float TRACK_R = TRACK_HH;
// The knob is what says this is a control rather than a reading: the charge bar
// is the same shape and nobody is meant to touch it.
constexpr float KNOB_R = 13.0f * SCENE;
// It travels short of the track by its own radius at either end, so it stays
// inside the track rather than hanging off it - nought and a hundred are then
// the knob parked against a stop instead of half of it out over the end.
constexpr float KNOB_SPAN = TRACK_HW - KNOB_R;
constexpr float BRIGHTNESS_Y = 148.0f * SCENE;
constexpr float VOLUME_Y = 228.0f * SCENE;
// Above each track, on its left end, the way the charge bar carries its own.
constexpr int16_t TITLE_UP = (int16_t)(32 * SCENE);
constexpr int16_t SCALE = 2;
// The rows a slider answers to, either side of its track. Well past the ink in
// both directions: a thumb lands where it lands, and the track is thinner than
// a fingertip. Short of the other slider's, or a finger between them is on both.
constexpr int16_t REACH = (int16_t)(38 * SCENE);
static_assert(VOLUME_Y - BRIGHTNESS_Y > 2 * REACH, "the sliders would answer to the same finger");
// The band each one is cleared and sent as: its title down to the bottom of its
// knob, and a pixel of margin for the edge to fade into.
constexpr int16_t BAND_UP = (int16_t)(TITLE_UP + 1);
constexpr int16_t BAND_DOWN = (int16_t)(KNOB_R + 2.0f);

// What nought on the slider reaches the panel as. The slider runs the whole way
// down, the way a slider does, but the LCD's backlight is the only light there
// is and a glass with none is a glass nothing can be set on, this page
// included - and on the AMOLED the same floor is what keeps the pixels lit.
constexpr uint8_t BRIGHTNESS_MIN = 5;
constexpr uint8_t BRIGHTNESS_DEFAULT = 100;
// A third of the way up, which through the square in audio.cpp is the tenth of
// full scale the board shipped at.
constexpr uint8_t VOLUME_DEFAULT = 32;
// How long the finger has been off it before the change is worth the flash. A
// write per position of a drag is a hundred writes for one setting.
constexpr uint32_t SAVE_MS = 1000;

constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t GREY = 0x8410;
constexpr uint16_t FAINT = 0x4A49;

enum Slider : uint8_t { BRIGHTNESS, VOLUME, SLIDERS };

constexpr float AT[SLIDERS] = {BRIGHTNESS_Y, VOLUME_Y};
const char *const NAMED[SLIDERS] = {"BRIGHTNESS", "VOLUME"};

Preferences store;
uint8_t value[SLIDERS] = {BRIGHTNESS_DEFAULT, VOLUME_DEFAULT};
uint8_t shown[SLIDERS] = {255, 255};
bool fresh = true;
int8_t held = -1;
// The finger that swiped the page in is still on the glass when the page
// arrives, and wherever it is, it is not setting anything.
bool arriving = true;
bool dirty = false;
uint32_t changedAt = 0;

void apply(uint8_t which) {
  if (which == BRIGHTNESS) {
    panelBrightness(settingsBrightness());
  } else {
    audioVolume(value[which]);
  }
}

void set(uint8_t which, uint8_t to) {
  if (value[which] == to) {
    return;
  }
  value[which] = to;
  apply(which);
  dirty = true;
  changedAt = millis();
}

// Where the finger is, turned into whichever slider it landed on and how far
// along it. Taken up on the way down and kept until the finger leaves, so a
// drag that drifts off the track keeps the knob it started with.
void follow() {
  int16_t across = 0;
  int16_t along = 0;
  if (!touchFinger(&across, &along)) {
    // What was just set is worth hearing, and this is the one moment it is
    // certain the finger has finished setting it.
    if (held == VOLUME) {
      audioSampled();
    }
    held = -1;
    arriving = false;
    return;
  }
  if (arriving) {
    return;
  }
  if (held < 0) {
    for (uint8_t i = 0; i < SLIDERS; i++) {
      if (fabsf((float)along - AT[i]) <= (float)REACH) {
        held = (int8_t)i;
      }
    }
    if (held < 0) {
      return;
    }
  }
  // Against the knob's travel rather than the track's length, or the knob lands
  // up to its own radius short of the finger that put it there.
  float t = clamp01(((float)across - (TRACK_X - KNOB_SPAN)) / (2.0f * KNOB_SPAN));
  set((uint8_t)held, (uint8_t)(t * 100.0f + 0.5f));
}

void drawSlider(uint16_t *fb, uint8_t which, bool send) {
  float cy = AT[which];
  uint8_t percent = value[which];
  int16_t top = (int16_t)(cy - (float)BAND_UP);
  int16_t bottom = (int16_t)(cy + (float)BAND_DOWN);
  // Its own band, which nothing else is in - whole rows, the way the charge bar
  // on the details page takes its own.
  if (send) {
    for (int16_t y = top; y <= bottom; y++) {
      memset(boardRow(fb, y), 0, (size_t)SCREEN_W * 2);
    }
  }

  float knob = TRACK_X - KNOB_SPAN + 2.0f * KNOB_SPAN * ((float)percent / 100.0f);
  // Up to the knob's middle, so the two cannot come apart. The fill is the
  // track's own shape shortened from the right rather than the track cut off at
  // a line, so what is left is a lozenge and not a sliver with square shoulders
  // hanging out of a rounded end.
  float wide = (knob - (TRACK_X - TRACK_HW)) * 0.5f;
  float from = TRACK_X - TRACK_HW + wide;
  uint16_t ink = gaugeColour(0);

  for (int16_t y = (int16_t)(cy - KNOB_R - 2.0f); y <= bottom; y++) {
    float py = (float)y + 0.5f - cy;
    for (int16_t x = (int16_t)(TRACK_X - TRACK_HW - 2.0f);
         x <= (int16_t)(TRACK_X + TRACK_HW + 2.0f); x++) {
      float px = (float)x + 0.5f - TRACK_X;
      // The knob sits over the track rather than being cut into it, so it is
      // drawn last and wins wherever it lands.
      float kx = (float)x + 0.5f - knob;
      float cover = 0.5f - (sqrtf(kx * kx + py * py) - KNOB_R);
      if (cover > 0.02f) {
        plot(fb, x, y, cover, WHITE);
        continue;
      }
      cover = 0.5f - sdRoundBox(px, py, TRACK_HW, TRACK_HH, TRACK_R);
      if (cover <= 0.02f) {
        continue;
      }
      bool full = sdRoundBox(px - (from - TRACK_X), py, wide, TRACK_HH, TRACK_R) < 0.5f;
      plot(fb, x, y, cover, full ? ink : FAINT);
    }
  }

  char said[20];
  snprintf(said, sizeof(said), "%s - %u%%", NAMED[which], (unsigned)percent);
  // Left, on the track's own left end, so the line holds still when the number
  // gains or loses a digit. textDraw centres on what it is given and the last
  // glyph carries no gap after it.
  int16_t width = (int16_t)(strlen(said) * textStep(SCALE) - SCALE);
  textDraw(fb, said, (int16_t)(TRACK_X - TRACK_HW) + width / 2, (int16_t)cy - TITLE_UP, SCALE,
           boardColour(GREY));

  if (send) {
    boardFlushRows((int16_t)(SCREEN_H - 1 - bottom), (int16_t)(SCREEN_H - 1 - top));
  }
}

}  // namespace

void settingsBegin() {
  store.begin("settings", false);
  value[BRIGHTNESS] = store.getUChar("brightness", BRIGHTNESS_DEFAULT);
  value[VOLUME] = store.getUChar("volume", VOLUME_DEFAULT);
  for (uint8_t i = 0; i < SLIDERS; i++) {
    if (value[i] > 100) {
      value[i] = 100;
    }
  }
  Serial.printf("settings: brightness %u%%, volume %u%%\n", value[BRIGHTNESS], value[VOLUME]);
}

uint8_t settingsBrightness() {
  return (uint8_t)(BRIGHTNESS_MIN + (100 - BRIGHTNESS_MIN) * value[BRIGHTNESS] / 100);
}

uint8_t settingsVolume() { return value[VOLUME]; }

void settingsForget() {
  fresh = true;
  arriving = true;
}

bool settingsHolding() { return held >= 0; }

void settingsStep(uint16_t *fb) {
  follow();
  if (fresh) {
    fresh = false;
    memset(fb, 0, (size_t)SCREEN_W * SCREEN_H * 2);
    for (uint8_t i = 0; i < SLIDERS; i++) {
      shown[i] = value[i];
      drawSlider(fb, i, false);
    }
    boardFlush();
    return;
  }
  // Only the one that moved, and only its own rows: the other is words and a
  // track that have not changed.
  for (uint8_t i = 0; i < SLIDERS; i++) {
    if (shown[i] == value[i]) {
      continue;
    }
    shown[i] = value[i];
    drawSlider(fb, i, true);
  }
}

void settingsKeep() {
  if (!dirty || millis() - changedAt < SAVE_MS) {
    return;
  }
  dirty = false;
  store.putUChar("brightness", value[BRIGHTNESS]);
  store.putUChar("volume", value[VOLUME]);
  Serial.printf("settings: saved brightness %u%%, volume %u%%\n", value[BRIGHTNESS],
                value[VOLUME]);
}
