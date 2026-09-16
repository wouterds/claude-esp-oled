#pragma once

#include <math.h>
#include <stdint.h>

#include "board.h"

// The shapes the pages are drawn out of, as signed distances: a pixel's
// coverage falls out of how far it sits from the edge, which is what makes an
// edge smooth without a second pass over it. One copy, so every page draws in
// the same language rather than in its own dialect of it.

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

inline uint16_t fade(uint16_t colour, float by) {
  uint16_t r = (uint16_t)(((colour >> 11) & 0x1F) * by);
  uint16_t g = (uint16_t)(((colour >> 5) & 0x3F) * by);
  uint16_t b = (uint16_t)((colour & 0x1F) * by);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Coverage rather than a hard edge, the same way the face is drawn. At this
// size a jagged corner is most of what you see of an icon. Faded into black
// rather than into what was there, which is what every page puts behind it.
inline void plot(uint16_t *fb, int16_t x, int16_t y, float coverage, uint16_t colour) {
  if (coverage <= 0.02f || x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) {
    return;
  }
  boardRow(fb, y)[boardX(x)] = boardColour(fade(colour, clamp01(coverage)));
}

inline float sdRoundBox(float px, float py, float hx, float hy, float r) {
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

// Apex up, and exact - which the max of the two half-planes is not. That is
// only right along the edges; outside a corner it reads about half the true
// distance, so subtracting a radius from it drew the apex out into a spike
// instead of capping it. Rounding anything by this needs the real distance.
inline float sdTriangle(float px, float py, float hx, float hy) {
  px = fabsf(px);
  // Outside, the nearest point on the right edge taken as a segment, so the
  // apex and the corner answer for their own neighbourhoods.
  float ex = hx;
  float ey = 2.0f * hy;
  float vx = px;
  float vy = py + hy;
  float t = (vx * ex + vy * ey) / (ex * ex + ey * ey);
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  float dx = vx - ex * t;
  float dy = vy - ey * t;
  float edge = sqrtf(dx * dx + dy * dy);
  float bx = px > hx ? px - hx : 0.0f;
  float by = py - hy;
  float base = sqrtf(bx * bx + by * by);
  // Inside, the half-planes are exact and cost less than picking a corner.
  float slant = (2.0f * hy * px - hx * (py + hy)) / sqrtf(4.0f * hy * hy + hx * hx);
  float inside = slant > by ? slant : by;
  return inside < 0.0f ? inside : (edge < base ? edge : base);
}
