#include "scene.h"

#include <math.h>
#include <string.h>

#include "board.h"

namespace {

constexpr uint8_t COUNT = 18;
constexpr int16_t RADIUS = 12;
constexpr int16_t GRID_STEP = 36;
constexpr uint16_t COLOUR[3] = {0xF800, 0x07E0, 0x001F};

// Ten divisions rather than nine, so the grid does not land on the seams
// between the bands the flush sends and hide a fault at them.
static_assert(360 % GRID_STEP == 0, "the grid has to close on the last column");

float px[COUNT];
float py[COUNT];
float vx[COUNT];
float vy[COUNT];

// Half of each channel of one colour over the other, packed as it lies. Where
// two balls cross, both are still there.
inline uint16_t blendHalf(uint16_t under, uint16_t over) {
  uint16_t r = (((under >> 11) & 0x1F) + ((over >> 11) & 0x1F)) >> 1;
  uint16_t g = (((under >> 5) & 0x3F) + ((over >> 5) & 0x3F)) >> 1;
  uint16_t b = ((under & 0x1F) + (over & 0x1F)) >> 1;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

void fillDisc(uint16_t *fb, float atX, float atY, uint16_t colour) {
  int16_t cx = (int16_t)atX;
  int16_t cy = (int16_t)atY;
  int16_t top = cy - RADIUS < 0 ? 0 : cy - RADIUS;
  int16_t bottom = cy + RADIUS >= SCREEN_H ? SCREEN_H - 1 : cy + RADIUS;
  int16_t left = cx - RADIUS < 0 ? 0 : cx - RADIUS;
  int16_t right = cx + RADIUS >= SCREEN_W ? SCREEN_W - 1 : cx + RADIUS;

  for (int16_t y = top; y <= bottom; y++) {
    uint16_t *row = fb + (int32_t)y * SCREEN_W;
    int32_t dy = y - cy;
    for (int16_t x = left; x <= right; x++) {
      int32_t dx = x - cx;
      if (dx * dx + dy * dy <= RADIUS * RADIUS) {
        row[x] = blendHalf(row[x], colour);
      }
    }
  }
}

}  // namespace

void sceneBegin() {
  // Spread around a ring and each thrown off at its own angle, so they take a
  // while to fall into step with one another.
  for (uint8_t i = 0; i < COUNT; i++) {
    float around = (float)i * 0.3490659f;
    px[i] = SCREEN_R + cosf(around) * 92.0f;
    py[i] = SCREEN_R + sinf(around) * 92.0f;
    float heading = around * 2.3f + 0.7f;
    float speed = 210.0f + (float)(i % 3) * 45.0f;
    vx[i] = cosf(heading) * speed;
    vy[i] = sinf(heading) * speed;
  }
}

void sceneStep(float dt) {
  const float limit = SCREEN_R - RADIUS - 2.0f;
  for (uint8_t i = 0; i < COUNT; i++) {
    px[i] += vx[i] * dt;
    py[i] += vy[i] * dt;

    float ox = px[i] - SCREEN_R;
    float oy = py[i] - SCREEN_R;
    float dist = sqrtf(ox * ox + oy * oy);
    if (dist <= limit || dist < 0.001f) {
      continue;
    }
    // The glass is a circle, so it reflects about the radius rather than about
    // a wall, and the ball is put back on the edge rather than left outside it
    // - otherwise a fast one bounces again on the next frame and sticks.
    float nx = ox / dist;
    float ny = oy / dist;
    float along = vx[i] * nx + vy[i] * ny;
    vx[i] -= 2.0f * along * nx;
    vy[i] -= 2.0f * along * ny;
    px[i] = SCREEN_R + nx * limit;
    py[i] = SCREEN_R + ny * limit;
  }
}

void sceneDraw(uint16_t *fb) {
  memset(fb, 0, (size_t)SCREEN_W * SCREEN_H * 2);

  for (uint8_t i = 0; i < COUNT; i++) {
    fillDisc(fb, px[i], py[i], COLOUR[i % 3]);
  }

  for (int16_t y = 0; y < SCREEN_H; y += GRID_STEP) {
    uint16_t *row = fb + (int32_t)y * SCREEN_W;
    for (int16_t x = 0; x < SCREEN_W; x++) {
      row[x] = blendHalf(row[x], 0xFFFF);
    }
  }
  for (int16_t x = 0; x < SCREEN_W; x += GRID_STEP) {
    for (int16_t y = 0; y < SCREEN_H; y++) {
      uint16_t *at = fb + (int32_t)y * SCREEN_W + x;
      *at = blendHalf(*at, 0xFFFF);
    }
  }
}
