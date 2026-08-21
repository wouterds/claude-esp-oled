#include "gauge.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <math.h>

#include "board.h"

namespace {

// Out where the glass is about to stop. The bar follows the edge because it is
// a piece of a circle concentric with it, not because it is bent to fit.
constexpr float RADIUS = 174.0f;
constexpr float HALF_THICK = 3.5f;
// Either side of the horizontal, which comes to three quarters of the panel's
// height. Four fifths was the ask and four fifths does not fit: the ends of an
// arc that long curve back in to where the address is written.
constexpr float SWEEP = 0.87f;

constexpr int16_t BOX_X0 = 0;
constexpr int16_t BOX_X1 = 80;
constexpr int16_t BOX_Y0 = 42;
constexpr int16_t BOX_Y1 = 318;

constexpr uint16_t TRACK = 0x528A;

// One bar changes every this often, so each of them lands on a new value every
// five seconds and they never move together.
constexpr uint32_t TURN_MS = 2500;

// What is left when the shapes have been resolved: every pixel a bar puts on
// the glass, with the colour it ends up. About twenty-four hundred each, and
// the cap is only there so a mistake cannot run away with PSRAM. A side has its
// own half of the buffer because either of them can be rebuilt on its own.
struct Pixel {
  int16_t x;
  int16_t y;
  uint16_t colour;
};
constexpr uint16_t SIDE_PIXELS = 6000;

Pixel *pixels = nullptr;
uint16_t count[2] = {0, 0};
uint32_t nextTurn = 0;
uint8_t turn = 0;

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// An arc of RADIUS with round ends, bisected by `bisector` and reaching
// `aperture` either side of it. The point is turned so the bisector points
// along y, and from there it is symmetric: inside the aperture the nearest
// thing is the circle, outside it the nearer end.
float sdArc(float px, float py, float bisector, float aperture) {
  float bx = cosf(bisector);
  float by = sinf(bisector);
  float qx = fabsf(px * by - py * bx);
  float qy = px * bx + py * by;
  float s = sinf(aperture);
  float c = cosf(aperture);
  if (c * qx > s * qy) {
    float ex = qx - s * RADIUS;
    float ey = qy - c * RADIUS;
    return sqrtf(ex * ex + ey * ey) - HALF_THICK;
  }
  return fabsf(sqrtf(qx * qx + qy * qy) - RADIUS) - HALF_THICK;
}

// Teal at nothing, orange halfway, a hot pink at full. The whole fill takes one
// colour off this ramp: the bar says how much by how long it is, and says it
// again by what colour it is, rather than fading along its own length.
uint16_t colourAt(uint8_t percent) {
  constexpr float STOPS[3][3] = {{0.0f, 230.0f, 165.0f}, {255.0f, 150.0f, 0.0f},
                                 {255.0f, 45.0f, 110.0f}};
  float t = clamp01(percent / 100.0f) * 2.0f;
  int lo = t < 1.0f ? 0 : 1;
  float k = t - (float)lo;
  float r = STOPS[lo][0] + (STOPS[lo + 1][0] - STOPS[lo][0]) * k;
  float g = STOPS[lo][1] + (STOPS[lo + 1][1] - STOPS[lo][1]) * k;
  float b = STOPS[lo][2] + (STOPS[lo + 1][2] - STOPS[lo][2]) * k;
  return (uint16_t)(((uint16_t)(r * 31.0f / 255.0f) << 11) |
                    ((uint16_t)(g * 63.0f / 255.0f) << 5) | (uint16_t)(b * 31.0f / 255.0f));
}

uint16_t shade(uint16_t colour, float coverage) {
  uint16_t r = (uint16_t)(((colour >> 11) & 0x1F) * coverage);
  uint16_t g = (uint16_t)(((colour >> 5) & 0x3F) * coverage);
  uint16_t b = (uint16_t)((colour & 0x1F) * coverage);
  return boardColour((uint16_t)((r << 11) | (g << 5) | b));
}

void add(uint8_t side, int16_t x, int16_t y, uint16_t colour) {
  if (count[side] < SIDE_PIXELS) {
    pixels[side * SIDE_PIXELS + count[side]++] = {x, y, colour};
  }
}

// The fill grows from the bottom end upward, so its own arc is the piece
// between the bottom end and however far along it has got.
void build(uint8_t side, uint8_t percent) {
  bool right = side == 1;
  count[side] = 0;
  uint16_t filled = colourAt(percent);
  float fraction = clamp01(percent / 100.0f);
  float bisector = SWEEP - SWEEP * fraction;
  float aperture = SWEEP * fraction;

  for (int16_t y = BOX_Y0; y <= BOX_Y1; y++) {
    for (int16_t x = BOX_X0; x <= BOX_X1; x++) {
      float px = (float)x + 0.5f - SCREEN_R;
      float py = (float)y + 0.5f - SCREEN_R;
      // One shape, drawn twice: the right hand bar is the left one mirrored,
      // and mirroring the point is cheaper than carrying two of everything.
      float track = sdArc(px, py, (float)M_PI, SWEEP);
      float fill = percent > 0 ? sdArc(px, py, (float)M_PI - bisector, aperture) : 1.0f;

      float d = fill < 0.5f ? fill : track;
      uint16_t colour = fill < 0.5f ? filled : TRACK;
      float coverage = 0.5f - d;
      if (coverage <= 0.02f) {
        continue;
      }
      add(side, right ? (int16_t)(SCREEN_W - 1 - x) : x, y, shade(colour, clamp01(coverage)));
    }
  }
}

}  // namespace

void gaugeBegin() {
  pixels = (Pixel *)heap_caps_malloc(sizeof(Pixel) * SIDE_PIXELS * 2, MALLOC_CAP_SPIRAM);
  if (!pixels) {
    Serial.println("gauges: no room");
    return;
  }
  build(0, (uint8_t)(esp_random() % 101));
  build(1, (uint8_t)(esp_random() % 101));
  nextTurn = millis() + TURN_MS;
  Serial.printf("gauges: %u and %u pixels\n", count[0], count[1]);
}

// The rows the bars live in are nowhere near the ones the face moves through,
// so a bar that has just changed has to put itself on the panel.
void gaugeStep(uint16_t *fb, uint32_t now) {
  if (!pixels || (int32_t)(now - nextTurn) < 0) {
    return;
  }
  nextTurn = now + TURN_MS;
  build(turn, (uint8_t)(esp_random() % 101));
  turn ^= 1;
  gaugeDraw(fb);
  boardFlushRows((int16_t)(SCREEN_H - 1 - BOX_Y1), (int16_t)(SCREEN_H - 1 - BOX_Y0));
}

void gaugeDraw(uint16_t *fb) {
  for (uint8_t side = 0; side < 2; side++) {
    const Pixel *p = pixels + side * SIDE_PIXELS;
    for (uint16_t i = 0; i < count[side]; i++) {
      boardRow(fb, p[i].y)[boardX(p[i].x)] = p[i].colour;
    }
  }
}
