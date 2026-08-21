#include "text.h"

#include <math.h>

#include "board.h"

namespace {

// The stroke, and the box the glyphs are drawn in. Half a stroke either side of
// a four-two half-height puts the digits just over nine pixels tall.
constexpr float HALF_STROKE = 0.85f;
constexpr int16_t STEP = 8;
constexpr int16_t REACH_X = 5;
constexpr int16_t REACH_Y = 6;

enum Kind : uint8_t { SEG, ARC, RING };

// SEG is a capsule from a,b to c,d. ARC is a piece of a circle at a,b of radius
// c, bisected by the direction d and reaching e either side of it. RING is the
// outline of a rounded box, which is the only closed curve here.
struct Stroke {
  Kind kind;
  float a, b, c, d, e;
};

constexpr float UP = -1.5707963f;
constexpr float DOWN = 1.5707963f;
constexpr float ROUND = 3.15f;

const Stroke G0[] = {{RING, 0.0f, 0.0f, 2.0f, 3.9f, 2.0f}};
const Stroke G1[] = {{SEG, 0.0f, -3.9f, 0.0f, 3.9f}, {SEG, -1.5f, -2.5f, 0.0f, -3.95f}};
const Stroke G2[] = {{ARC, 0.0f, -1.95f, 1.95f, UP, 2.15f},
                     {SEG, 1.9f, -1.35f, -1.95f, 3.9f},
                     {SEG, -2.05f, 3.9f, 2.15f, 3.9f}};
const Stroke G3[] = {{ARC, 0.0f, -2.0f, 1.95f, 0.0f, 2.05f},
                     {ARC, 0.0f, 2.0f, 1.95f, 0.0f, 2.15f}};
const Stroke G4[] = {{SEG, 1.0f, -3.9f, -2.3f, 1.3f},
                     {SEG, -2.35f, 1.3f, 2.3f, 1.3f},
                     {SEG, 1.0f, -3.9f, 1.0f, 3.9f}};
const Stroke G5[] = {{SEG, -1.95f, -3.9f, 2.0f, -3.9f},
                     {SEG, -1.95f, -3.9f, -1.95f, -0.3f},
                     {ARC, 0.0f, 1.6f, 2.25f, 0.0f, 2.2f}};
const Stroke G6[] = {{ARC, 0.0f, 1.7f, 2.15f, DOWN, ROUND}, {SEG, -2.1f, 1.5f, 0.9f, -3.9f}};
const Stroke G7[] = {{SEG, -2.0f, -3.9f, 2.1f, -3.9f}, {SEG, 2.1f, -3.9f, -0.7f, 3.9f}};
const Stroke G8[] = {{ARC, 0.0f, -2.0f, 1.9f, UP, ROUND}, {ARC, 0.0f, 2.05f, 2.05f, DOWN, ROUND}};
const Stroke G9[] = {{ARC, 0.0f, -1.7f, 2.15f, UP, ROUND}, {SEG, 2.1f, -1.5f, -0.9f, 3.9f}};
const Stroke GDOT[] = {{SEG, 0.0f, 3.5f, 0.0f, 3.6f}};

struct Glyph {
  const Stroke *strokes;
  uint8_t count;
};

#define GLYPH(g) \
  { g, (uint8_t)(sizeof(g) / sizeof(Stroke)) }
const Glyph DIGITS[] = {GLYPH(G0), GLYPH(G1), GLYPH(G2), GLYPH(G3), GLYPH(G4),
                        GLYPH(G5), GLYPH(G6), GLYPH(G7), GLYPH(G8), GLYPH(G9)};
const Glyph DOT = GLYPH(GDOT);
#undef GLYPH

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float sdSegment(float px, float py, float ax, float ay, float bx, float by) {
  float pax = px - ax;
  float pay = py - ay;
  float bax = bx - ax;
  float bay = by - ay;
  float h = clamp01((pax * bax + pay * bay) / (bax * bax + bay * bay));
  float dx = pax - bax * h;
  float dy = pay - bay * h;
  return sqrtf(dx * dx + dy * dy);
}

// Turned so the arc's bisector points along y, and then it is symmetric: inside
// the aperture the nearest thing is the circle, outside it the nearer end.
float sdArc(float px, float py, float cx, float cy, float r, float bisector, float aperture) {
  px -= cx;
  py -= cy;
  float bx = cosf(bisector);
  float by = sinf(bisector);
  float qx = fabsf(px * by - py * bx);
  float qy = px * bx + py * by;
  float s = sinf(aperture);
  float c = cosf(aperture);
  if (c * qx > s * qy) {
    float ex = qx - s * r;
    float ey = qy - c * r;
    return sqrtf(ex * ex + ey * ey);
  }
  return fabsf(sqrtf(qx * qx + qy * qy) - r);
}

float sdRing(float px, float py, float cx, float cy, float hx, float hy, float r) {
  float qx = fabsf(px - cx) - (hx - r);
  float qy = fabsf(py - cy) - (hy - r);
  float ox = qx > 0.0f ? qx : 0.0f;
  float oy = qy > 0.0f ? qy : 0.0f;
  float inner = (qx > qy ? qx : qy);
  return fabsf(sqrtf(ox * ox + oy * oy) + (inner < 0.0f ? inner : 0.0f) - r);
}

const Glyph *glyph(char c) {
  if (c >= '0' && c <= '9') {
    return &DIGITS[c - '0'];
  }
  return c == '.' ? &DOT : nullptr;
}

float distance(const Glyph *g, float px, float py) {
  float d = 1e9f;
  for (uint8_t i = 0; i < g->count; i++) {
    const Stroke &s = g->strokes[i];
    float v;
    if (s.kind == SEG) {
      v = sdSegment(px, py, s.a, s.b, s.c, s.d);
    } else if (s.kind == ARC) {
      v = sdArc(px, py, s.a, s.b, s.c, s.d, s.e);
    } else {
      v = sdRing(px, py, s.a, s.b, s.c, s.d, s.e);
    }
    if (v < d) {
      d = v;
    }
  }
  return d - HALF_STROKE;
}

}  // namespace

int16_t textStep() { return STEP; }

void textDraw(uint16_t *fb, const char *s, int16_t centreX, int16_t centreY, uint16_t colour) {
  int16_t count = 0;
  for (const char *p = s; *p; p++) {
    count++;
  }
  if (count == 0) {
    return;
  }

  int16_t at = (int16_t)(centreX - ((count - 1) * STEP) / 2);
  for (int16_t i = 0; i < count; i++, at += STEP) {
    const Glyph *g = glyph(s[i]);
    if (!g) {
      continue;
    }
    for (int16_t y = centreY - REACH_Y; y <= centreY + REACH_Y; y++) {
      if (y < 0 || y >= SCREEN_H) {
        continue;
      }
      uint16_t *line = boardRow(fb, y);
      for (int16_t x = at - REACH_X; x <= at + REACH_X; x++) {
        if (x < 0 || x >= SCREEN_W) {
          continue;
        }
        float coverage = 0.5f - distance(g, (float)x + 0.5f - at, (float)y + 0.5f - centreY);
        if (coverage <= 0.02f) {
          continue;
        }
        coverage = clamp01(coverage);
        uint16_t r = (uint16_t)(((colour >> 11) & 0x1F) * coverage);
        uint16_t gc = (uint16_t)(((colour >> 5) & 0x3F) * coverage);
        uint16_t b = (uint16_t)((colour & 0x1F) * coverage);
        line[boardX(x)] = boardColour((uint16_t)((r << 11) | (gc << 5) | b));
      }
    }
  }
}
