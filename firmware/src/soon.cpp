#include "soon.h"

#include "board.h"
#include "shape.h"
#include "text.h"

namespace {

// The warning triangle off the face's status line, three times the size. The
// same shape says the same thing at any size, and this page has nothing else
// to say - so it is drawn in that sign's own units and sampled larger, the way
// the status line samples it for either glass.
constexpr float SIGN_SCALE = 3.0f * SCENE;
constexpr float SIGN_HW = 8.5f;
constexpr float SIGN_HH = 7.5f;
constexpr float SIGN_R = 5.0f;
// Above the middle line by about the height of the line under it, so the pair
// sit centred on the glass rather than the sign alone.
constexpr float SIGN_Y = 150.0f * SCENE;
constexpr int16_t SIGN_REACH_X = (int16_t)((SIGN_HW + SIGN_R) * SIGN_SCALE) + 2;
constexpr int16_t SIGN_REACH_Y = (int16_t)((SIGN_HH + SIGN_R) * SIGN_SCALE) + 2;
constexpr int16_t LINE_TOP = (int16_t)(214 * SCENE);
constexpr int16_t LINE_SCALE = 2;

constexpr uint16_t WHITE = 0xFFFF;
// The status line's yellow, so the sign is the sign and not a picture of one.
constexpr uint16_t YELLOW = 0xFE48;

}  // namespace

void soonDraw(uint16_t *fb) {
  for (int16_t y = (int16_t)SIGN_Y - SIGN_REACH_Y; y <= (int16_t)SIGN_Y + SIGN_REACH_Y; y++) {
    for (int16_t x = (int16_t)SCREEN_R - SIGN_REACH_X; x <= (int16_t)SCREEN_R + SIGN_REACH_X; x++) {
      float px = ((float)x + 0.5f - SCREEN_R) / SIGN_SCALE;
      float py = ((float)y + 0.5f - SIGN_Y) / SIGN_SCALE;

      // Filled, with the bang taken out of it rather than laid over it - the
      // LCD cannot switch a pixel off, and a black glyph on a lit shape comes
      // out grey there.
      float shell = sdTriangle(px, py, SIGN_HW, SIGN_HH) - SIGN_R;
      float bar = sdRoundBox(px, py + 1.9f, 1.7f, 4.0f, 1.7f);
      float dot = sqrtf(px * px + (py - 6.4f) * (py - 6.4f)) - 1.8f;
      float bang = bar < dot ? bar : dot;
      plot(fb, x, y, 0.5f - (shell > -bang ? shell : -bang) * SIGN_SCALE, YELLOW);
    }
  }
  textDraw(fb, "UNDER CONSTRUCTION", (int16_t)SCREEN_R, LINE_TOP, LINE_SCALE, boardColour(WHITE));
}
