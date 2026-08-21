#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "board.h"
#include "buddy.h"
#include "face.h"

// A face on a round panel, and nothing else. It is not told anything and it
// does not ask - the whole of it is here, so what it does costs a flash rather
// than a protocol.
static Buddy buddy;
static Rect painted = {0, 0, 0, 0};
static uint32_t lastFrame = 0;

#ifdef BUDDY_SELFTEST
// Three half-opacity balls loose on a round panel. The glass is a circle, so
// they bounce off it the way anything bounces off a curve - reflected about the
// radius rather than about a wall - and where they cross, the colours add.
static inline uint16_t blendHalf(uint16_t under, uint16_t over) {
  uint16_t r = (((under >> 11) & 0x1F) + ((over >> 11) & 0x1F)) >> 1;
  uint16_t g = (((under >> 5) & 0x3F) + ((over >> 5) & 0x3F)) >> 1;
  uint16_t b = ((under & 0x1F) + (over & 0x1F)) >> 1;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void selftest() {
  static const int16_t RADIUS = 46;
  static const uint16_t COLOUR[3] = {0xF800, 0x07E0, 0x001F};
  static float px[3] = {150.0f, 210.0f, 180.0f};
  static float py[3] = {140.0f, 170.0f, 220.0f};
  static float vx[3] = {132.0f, -108.0f, 76.0f};
  static float vy[3] = {94.0f, 121.0f, -143.0f};
  static bool lit = false;

  if (!lit) {
    lit = true;
    boardBacklight(1023);
    Serial.println("selftest: three bouncing balls, full brightness");
  }

  const float dt = 0.016f;
  const float limit = SCREEN_R - RADIUS - 2.0f;
  for (uint8_t i = 0; i < 3; i++) {
    px[i] += vx[i] * dt;
    py[i] += vy[i] * dt;

    float ox = px[i] - SCREEN_R;
    float oy = py[i] - SCREEN_R;
    float dist = sqrtf(ox * ox + oy * oy);
    if (dist > limit && dist > 0.001f) {
      float nx = ox / dist;
      float ny = oy / dist;
      float along = vx[i] * nx + vy[i] * ny;
      vx[i] -= 2.0f * along * nx;
      vy[i] -= 2.0f * along * ny;
      px[i] = SCREEN_R + nx * limit;
      py[i] = SCREEN_R + ny * limit;
    }
  }

  uint16_t *fb = boardFramebuffer();
  memset(fb, 0, (size_t)SCREEN_W * SCREEN_H * 2);
  for (uint8_t i = 0; i < 3; i++) {
    int16_t cx = (int16_t)px[i];
    int16_t cy = (int16_t)py[i];
    for (int16_t y = cy - RADIUS; y <= cy + RADIUS; y++) {
      if (y < 0 || y >= SCREEN_H) {
        continue;
      }
      for (int16_t x = cx - RADIUS; x <= cx + RADIUS; x++) {
        if (x < 0 || x >= SCREEN_W) {
          continue;
        }
        int32_t dx = x - cx;
        int32_t dy = y - cy;
        if (dx * dx + dy * dy <= RADIUS * RADIUS) {
          int32_t at = (int32_t)y * SCREEN_W + x;
          fb[at] = blendHalf(fb[at], COLOUR[i]);
        }
      }
    }
  }
  // A grid over everything, at the same half opacity. Every line is drawn the
  // same way, so anything that makes one line look unlike the others is the
  // panel rather than the drawing. Ten divisions rather than nine, so it does
  // not line up with the bands the flush sends and mask a fault at their seams.
  const int16_t STEP = 36;
  for (int16_t y = 0; y < SCREEN_H; y += STEP) {
    uint16_t *row = fb + (int32_t)y * SCREEN_W;
    for (int16_t x = 0; x < SCREEN_W; x++) {
      row[x] = blendHalf(row[x], 0xFFFF);
    }
  }
  for (int16_t x = 0; x < SCREEN_W; x += STEP) {
    for (int16_t y = 0; y < SCREEN_H; y++) {
      int32_t at = (int32_t)y * SCREEN_W + x;
      fb[at] = blendHalf(fb[at], 0xFFFF);
    }
  }

  boardFlush();
}
#endif

void setup() {
  Serial.begin(115200);
  if (!boardBegin()) {
    while (true) {
      delay(1000);
    }
  }
  buddyBegin(buddy);
  lastFrame = millis();
}

void loop() {
#ifdef BUDDY_SELFTEST
  selftest();
  return;
#endif

  uint32_t now = millis();
  float dt = (float)(now - lastFrame) * 0.001f;
  lastFrame = now;
  // A stall must not teleport it across the panel on the frame after.
  if (dt > 0.05f) {
    dt = 0.05f;
  }

  buddyUpdate(buddy, dt, now);
  FaceParams face = buddyFace(buddy, now);

  uint16_t *fb = boardFramebuffer();
  // Only last frame's rect can be holding anything: faceRender writes every
  // pixel of the box it returns, the black ones included.
  faceClear(fb, painted);
  painted = faceRender(fb, face);
  boardFlush();

  // Sixty is past what the panel or the eye wants; the rest goes back.
  uint32_t spent = millis() - now;
  if (spent < 16) {
    delay(16 - spent);
  }
}
