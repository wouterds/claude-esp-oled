#include "pokemon.h"

#include <Arduino.h>
#include <string.h>

#include "board.h"
#include "text.h"

namespace {

// Sprites are written out as the pictures they are, a character to a pixel, the
// way the font a few files over is. They can be read by eye and changed in a
// text editor, which is what a format for something drawn by hand is for.
//
//   .  nothing      K  black        Y  yellow
//   W  white        D  brown        R  red
//
// Drawn for a panel whose black is the page behind them, which is the whole of
// why they look the way they do: there is no outline round the outside of any
// of these, because an outline in the background colour is not one. The
// silhouette is where the colour stops. Black is used only well inside a shape,
// where there is something either side of it - eyes, a mouth, the band across
// the ball - and never at an edge. It is also why the ears end in brown rather
// than the black they are usually drawn with: black tips on a black page are
// ears that stop halfway.
const char *const PIKACHU[] = {
    ".....D...................D..................",
    "...DDDD.................DDDD................",
    ".DDDDDD.................DDDDDD..............",
    "..DDDDDY...............YDDDDD...............",
    "..DDDDYY...............YYDDDD...............",
    "...YYYYYY.............YYYYYY..............Y.",
    "...YYYYYY.............YYYYYY.............YY.",
    "....YYYYYY..YYYYYYY..YYYYYY.............YYY.",
    "....YYYYYYYYYYYYYYYYYYYYYYY...........YYYYY.",
    ".....YYYYYYYYYYYYYYYYYYYYY...........YYYYYY.",
    ".....YYYYYYYYYYYYYYYYYYYYY..........YYYYYYY.",
    ".....YYYYYYYYYYYYYYYYYYYYY.........YYYYYYYY.",
    ".....YYYYYYYYYYYYYYYYYYYYY........YYYYYYYYY.",
    "....YYYYYKKKYYYYYYYKKKYYYYY......YYYYYYY....",
    "....YYYYKKWWKYYYYYKKWWKYYYY.....YYYYYY......",
    "....YYYYKKKWKYYYYYKKKWKYYYY...YYYYY.........",
    "...YYYYYKKKKKYYYYYKKKKKYYYYY.YYYYYYY........",
    "....YYYYYKKKYYYYYYYKKKYYYYY.YYYYYYYYY.......",
    "....YRRRYYYYKYYYYKYYYYYRRRY..YYYYYYYY.......",
    "....RRRRRYYYYKKKKYYYYYRRRRR...YYYYYYYY......",
    "...RRRRRRRYYYYYYYYYYYRRRRRRR...YYYYYYYY.....",
    "...RRRRRRRYYYYYYYYYYYRRRRRRR....YYYYY.......",
    "....RRRRRYYYYYYYYYYYYYRRRRR......YYY........",
    "....YRRRYYYYYYYYYYYYYYYRRRY.....YY..........",
    "...YYYYYYYYYYYYYYYYYYYYYYYYY..YYY...........",
    "...YYYYYYYYYYYYYYYYYYYYYYYYY.YY.............",
    "...YYYYYYYYYYYYYYYYYYYYYYYYYYY..............",
    "...YYYYYYYYYYYYYYYYYYYYYYYYY................",
    "...YYYYYYYYYYYYYYYYYYYYYYYYY................",
    "....YYYYYYYYYYYYYYYYYYYYYYY.................",
    ".....Y..YYYYYYYYYYYYYYY..Y..................",
    ".......YYYYYYYYYYYYYYYYY....................",
    "......YYYYYYYYYYYYYYYYYYY...................",
    "......YYYYYYYYYYYYYYYYYYY...................",
    "......YYYYYYYY...YYYYYYYY...................",
    ".......YYYYYY.....YYYYYY....................",
};

const char *const BALL[] = {
    "........................................",
    "................RRRRRRRR................",
    ".............RRRRRRRRRRRRRR.............",
    "...........RRRRRRRRRRRRRRRRRR...........",
    ".........RRRRRRRRRRRRRRRRRRRRRR.........",
    "........RRRRRRRRRRRRRRRRRRRRRRRR........",
    ".......RRRRRRRRRRRRRRRRRRRRRRRRRR.......",
    "......RRRRRRRRRRRRRRRRRRRRRRRRRRRR......",
    ".....RRRRRRRRRRRRRRRRRRRRRRRRRRRRRR.....",
    "....RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR....",
    "....RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR....",
    "...RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR...",
    "...RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR...",
    "..RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR..",
    "..RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR..",
    "..RRRRRRRRRRRRRRRKKKKKKRRRRRRRRRRRRRRR..",
    ".RRRRRRRRRRRRRRRKKKKKKKKRRRRRRRRRRRRRRR.",
    ".RRRRRRRRRRRRRRKKKWWWWKKKRRRRRRRRRRRRRR.",
    ".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
    ".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
    ".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
    ".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
    ".WWWWWWWWWWWWWWKKKWWWWKKKWWWWWWWWWWWWWW.",
    ".WWWWWWWWWWWWWWWKKKKKKKKWWWWWWWWWWWWWWW.",
    "..WWWWWWWWWWWWWWWKKKKKKWWWWWWWWWWWWWWW..",
    "..WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW..",
    "..WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW..",
    "...WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW...",
    "...WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW...",
    "....WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW....",
    "....WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW....",
    ".....WWWWWWWWWWWWWWWWWWWWWWWWWWWWWW.....",
    "......WWWWWWWWWWWWWWWWWWWWWWWWWWWW......",
    ".......WWWWWWWWWWWWWWWWWWWWWWWWWW.......",
    "........WWWWWWWWWWWWWWWWWWWWWWWW........",
    ".........WWWWWWWWWWWWWWWWWWWWWW.........",
    "...........WWWWWWWWWWWWWWWWWW...........",
    ".............WWWWWWWWWWWWWW.............",
    "................WWWWWWWW................",
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
    {PIKACHU, 44, 36, "PIKACHU"},
};
constexpr uint8_t COUNT = sizeof(SPRITES) / sizeof(SPRITES[0]);

// Five screen pixels to a sprite pixel. Six is wider than the glass is at the
// height the ears reach, and what that costs is the tips of them.
constexpr int16_t SCALE = 5;
// Above the middle, to leave the name the bottom of the circle.
constexpr int16_t MIDDLE = 150;

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
      return 0xFE85;
    case 'W':
      return 0xFFFF;
    case 'D':
      return 0x9306;
    default:
      return 0xE1C7;
  }
}

// Square and hard edged, with none of the coverage the rest of the panel is
// drawn with. Smoothing a sprite is smoothing away the only thing it is.
void drawSprite(uint16_t *fb, const Sprite &sprite) {
  int16_t left = (int16_t)(SCREEN_R - sprite.wide * SCALE / 2);
  int16_t top = (int16_t)(MIDDLE - sprite.tall * SCALE / 2);

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
  drawSprite(fb, SPRITES[showing]);
  textDraw(fb, SPRITES[showing].name, (int16_t)SCREEN_R, NAME_TOP, NAME_SCALE, boardColour(WHITE));
  boardFlush();
}
