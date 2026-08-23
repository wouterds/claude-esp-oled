#include "pokemon.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "board.h"
#include "text.h"

namespace {

// Sprites are written out as the pictures they are, a character to a pixel, the
// way the font a few files over is. Anything can be read off them by eye and
// changed with a text editor, which is the whole point of a format for a thing
// that is drawn by hand.
//
//   .  nothing      K  outline      Y  yellow
//   W  white        D  dark red     R  red
//
// PIKACHU is traced off a photograph of the sprite from the games rather than
// drawn from nothing, so it is somebody else's shape - which is worth knowing
// before this repository is anything more than one board on one desk.
const char *const PIKACHU[] = {
    ".............KK.............................",
    "............KKK.............................",
    "...........KKKK......KK.....................",
    "...........KKKK...KKKKK.....................",
    "..........KWKKK..KYKKKK.....................",
    ".........KWWYKKKKKKKKK......................",
    ".........KWYYYKYYKKKKK......................",
    ".........KYYYYKYYKKKK.......................",
    "........KYYYYKYYYYKKK.......................",
    ".......KKYYYKWYYYYKK........................",
    ".....KKYKKKKWYYYYYK.........................",
    "....KDWWWWYYYYYYYKK....................KK...",
    "....KWWWWYYYYYYYDKK.................KKKYYK..",
    "....KWWWYYYYYYYDDDK...............KKYWYYYK..",
    ".KKKWWYYYYYYYYYYYYK............K..KYYYYYYK..",
    ".KWWYYYYYYYYYYYYYYYK........KKKYKKYYYYYYYK..",
    ".KYYYYYYYYYYYYYYYYYK......KKYYYYYYYYYYYYYYK.",
    "KYYYYYYYYYYYYYYYYYYYK.....KWYYYYYYYYYYYYYYYK",
    "KYYYYYYYYYYYYYYYYYYYK......KYYYYYYYYYYYYYYYK",
    "KYYYYYYYYYYYYYYYYYYYYK.....KYYYYYYYYYYYYYKKK",
    ".KYYYYYYYYYYYYYYYYYYYK......KYYYYYYYYYYKK...",
    ".KDDKYYYDDYYYYYYYYYYYK......KYYYYYKKKKK.....",
    ".KDYKKYDDDDYYYYYYYYYYK.......KYYKK..........",
    ".KKYYYYDDDDYYYYYYYYYYK.......KYK............",
    ".KYYYYYDDDDYYYYYYYYYYK.......KYK............",
    ".KYYYYYYDDYYYYYYYYYYYK.......KYYK...........",
    "..KYYYYYYYYDYYYYYYYYYYK....KKKYYK...........",
    "...KKKKKKDDYYYYYYYYYYYK..KKKYYKKK...........",
    "...KKKKKYYYYYYYYYYYYYYK..KYYKK..............",
    "...KYKYYYYYYYYYYYYYYYYK..KYYK...............",
    "...KDDYYYYYYYYYYYYYYYYK...KYK...............",
    "..KYDYYYYYYYYYYYYYYYYYK...KDK...............",
    "..KYYYYYYYYYDYYYYYYYYYKKKKKDDK..............",
    "..KKKYYYYYYYKDYYYYYYYYYKYYKKKK..............",
    "..KKKKKKKKKKYYYYYYYYYYYKKKKKK...............",
    ".KKKKYYYYYYYYYYYYYYYYYYK....................",
    ".KYKYYYYYYYYYYYYYYYYYYK.....................",
    ".KYYYYYYYYYYYYYYYYYYYK......................",
    ".KYYYYYYYYYYYYYYYYYYK.......................",
    "..KYKYYYYYYYYYYYYYYYK.......................",
    "..KYKKYYYYYYYYYYYYKKK.......................",
    "...KKKKKKKKKKKKKKK..........................",
};

const char *const BALL[] = {
    "........................................",
    "..............KKKKKKKKKKKK..............",
    "...........KKKKKKKKKKKKKKKKKK...........",
    "..........KKKKRRRRRRRRRRRRKKKK..........",
    "........KKKKRRRRRRRRRRRRRRRRKKKK........",
    ".......KKKRRRRRRRRRRRRRRRRRRRRKKK.......",
    "......KKKRRRRRRRRRRRRRRRRRRRRRRKKK......",
    ".....KKKRRRRRRRRRRRRRRRRRRRRRRRRKKK.....",
    "....KKKRRRRRRRRRRRRRRRRRRRRRRRRRRKKK....",
    "....KKRRRRRRRRRRRRRRRRRRRRRRRRRRRRKK....",
    "...KKRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRKK...",
    "..KKKRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRKKK..",
    "..KKRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRKK..",
    "..KKRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRKK..",
    ".KKRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRKK.",
    ".KKRRRRRRRRRRRRRRKKKKKKRRRRRRRRRRRRRRKK.",
    ".KKRRRRRRRRRRRRRKKKKKKKKRRRRRRRRRRRRRKK.",
    ".KKRRRRRRRRRRRRKKKWWWWKKKRRRRRRRRRRRRKK.",
    ".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
    ".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
    ".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
    ".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
    ".KKWWWWWWWWWWWWKKKWWWWKKKWWWWWWWWWWWWKK.",
    ".KKWWWWWWWWWWWWWKKKKKKKKWWWWWWWWWWWWWKK.",
    ".KKWWWWWWWWWWWWWWKKKKKKWWWWWWWWWWWWWWKK.",
    ".KKWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWKK.",
    "..KKWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWKK..",
    "..KKWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWKK..",
    "..KKKWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWKKK..",
    "...KKWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWKK...",
    "....KKWWWWWWWWWWWWWWWWWWWWWWWWWWWWKK....",
    "....KKKWWWWWWWWWWWWWWWWWWWWWWWWWWKKK....",
    ".....KKKWWWWWWWWWWWWWWWWWWWWWWWWKKK.....",
    "......KKKWWWWWWWWWWWWWWWWWWWWWWKKK......",
    ".......KKKWWWWWWWWWWWWWWWWWWWWKKK.......",
    "........KKKKWWWWWWWWWWWWWWWWKKKK........",
    "..........KKKKWWWWWWWWWWWWKKKK..........",
    "...........KKKKKKKKKKKKKKKKKK...........",
    "..............KKKKKKKKKKKK..............",
    "........................................",
};

struct Sprite {
  const char *const *rows;
  uint8_t wide;
  uint8_t tall;
  const char *name;
};

// The ball first, because it is the lid on the rest of them.
constexpr Sprite SPRITES[] = {
    {BALL, 40, 40, "POKEMON"},
    {PIKACHU, 44, 42, "PIKACHU"},
};
constexpr uint8_t COUNT = sizeof(SPRITES) / sizeof(SPRITES[0]);

// Four screen pixels to a sprite pixel. Five would fill more of the glass and
// leave the window with a margin thin enough to read as a mistake.
constexpr int16_t SCALE = 4;

// The sprites are drawn with a black outline, and this panel's black is the
// same black the page is - so on the page itself an outline is invisible and
// the ears, which are solid black at the tips, simply stop existing. They are
// put on something pale for the same reason they are in the games.
constexpr int16_t PANE_X0 = 66;
constexpr int16_t PANE_X1 = 294;
constexpr int16_t PANE_Y0 = 44;
constexpr int16_t PANE_Y1 = 252;
constexpr float PANE_R = 18.0f;
constexpr uint16_t PANE = 0xA5B6;

constexpr int16_t NAME_TOP = 272;
constexpr int16_t NAME_SCALE = 3;
constexpr uint16_t WHITE = 0xFFFF;

uint8_t showing = 0;
bool fresh = true;

uint16_t inkOf(char c) {
  switch (c) {
    case 'K':
      return 0x18E3;
    case 'Y':
      return 0xFE40;
    case 'W':
      return 0xFFDF;
    case 'D':
      return 0xA986;
    default:
      return 0xE0C3;
  }
}

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

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

void drawPane(uint16_t *fb) {
  constexpr float CX = (PANE_X0 + PANE_X1) * 0.5f;
  constexpr float CY = (PANE_Y0 + PANE_Y1) * 0.5f;
  constexpr float HX = (PANE_X1 - PANE_X0) * 0.5f;
  constexpr float HY = (PANE_Y1 - PANE_Y0) * 0.5f;

  for (int16_t y = PANE_Y0 - 1; y <= PANE_Y1 + 1; y++) {
    uint16_t *line = boardRow(fb, y);
    for (int16_t x = PANE_X0 - 1; x <= PANE_X1 + 1; x++) {
      float coverage = clamp01(0.5f - sdRoundBox((float)x + 0.5f - CX, (float)y + 0.5f - CY, HX,
                                                 HY, PANE_R));
      if (coverage <= 0.02f) {
        continue;
      }
      uint16_t r = (uint16_t)(((PANE >> 11) & 0x1F) * coverage);
      uint16_t g = (uint16_t)(((PANE >> 5) & 0x3F) * coverage);
      uint16_t b = (uint16_t)((PANE & 0x1F) * coverage);
      line[boardX(x)] = boardColour((uint16_t)((r << 11) | (g << 5) | b));
    }
  }
}

// Square and hard edged, with none of the coverage the rest of the panel is
// drawn with. Smoothing a sprite is smoothing away the only thing it is.
void drawSprite(uint16_t *fb, const Sprite &sprite) {
  int16_t left = (int16_t)(SCREEN_R - sprite.wide * SCALE / 2);
  int16_t top = (int16_t)((PANE_Y0 + PANE_Y1) / 2 - sprite.tall * SCALE / 2);

  for (uint8_t row = 0; row < sprite.tall; row++) {
    const char *cells = sprite.rows[row];
    for (uint8_t col = 0; col < sprite.wide; col++) {
      if (cells[col] == '.') {
        continue;
      }
      uint16_t ink = boardColour(inkOf(cells[col]));
      for (int16_t dy = 0; dy < SCALE; dy++) {
        int16_t y = top + row * SCALE + dy;
        if (y < 0 || y >= SCREEN_H) {
          continue;
        }
        uint16_t *line = boardRow(fb, y);
        for (int16_t dx = 0; dx < SCALE; dx++) {
          int16_t x = left + col * SCALE + dx;
          if (x >= 0 && x < SCREEN_W) {
            line[boardX(x)] = ink;
          }
        }
      }
    }
  }
}

}  // namespace

void pokemonOpen() {
  showing = 0;
  fresh = true;
}

void pokemonTurn(bool onward) {
  showing = onward ? (uint8_t)((showing + 1) % COUNT) : (uint8_t)((showing + COUNT - 1) % COUNT);
  fresh = true;
}

void pokemonStep(uint16_t *fb) {
  // Nothing on this page moves on its own, so there is one reason to draw it
  // and it is that somebody asked for a different one.
  if (!fresh) {
    return;
  }
  fresh = false;

  memset(fb, 0, (size_t)SCREEN_W * SCREEN_H * 2);
  drawPane(fb);
  drawSprite(fb, SPRITES[showing]);
  textDraw(fb, SPRITES[showing].name, (int16_t)SCREEN_R, NAME_TOP, NAME_SCALE, boardColour(WHITE));
  boardFlush();
}
