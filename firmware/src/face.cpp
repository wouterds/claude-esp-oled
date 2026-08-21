#include "face.h"

#include <math.h>
#include <string.h>

#include "board.h"

// Every shape below is a signed distance in pixels rather than a span of them,
// so a pixel's coverage falls straight out of the distance and the edges come
// out smooth without a second pass over them. It is also what lets one shape
// become another: an eye and a squint differ by a number, not by a branch.
namespace {

constexpr float GLOW_RADIUS = 15.0f;
constexpr float GLOW_GAIN = 0.55f;
constexpr int TILE = 16;

// Precomputed once a frame. The trig and the inverse transform do not vary per
// pixel and there are sixty thousand of those.
struct Prepared {
  const FaceParams *p;
  float m00, m01, m10, m11;
  float dscale;
  float browC, browS;
  float mouthS, mouthC;
  float archOffset, archRadius;
};

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

inline float sdRoundBox(float px, float py, float hx, float hy, float r) {
  float qx = fabsf(px) - hx + r;
  float qy = fabsf(py) - hy + r;
  float ax = qx > 0.0f ? qx : 0.0f;
  float ay = qy > 0.0f ? qy : 0.0f;
  float inner = qx > qy ? qx : qy;
  return sqrtf(ax * ax + ay * ay) + (inner < 0.0f ? inner : 0.0f) - r;
}

// An arc of radius ra and thickness rb, centred on +y and opening by the angle
// whose sine and cosine are passed in. y runs down, so a small aperture is a
// smile, a wide one is a grin, and pi closes the ring into a round o - which
// is why one mouth covers every mood without ever cutting between shapes.
inline float sdArc(float px, float py, float sinA, float cosA, float ra, float rb) {
  px = fabsf(px);
  if (cosA * px > sinA * py) {
    float dx = px - sinA * ra;
    float dy = py - cosA * ra;
    return sqrtf(dx * dx + dy * dy) - rb;
  }
  return fabsf(sqrtf(px * px + py * py) - ra) - rb;
}

inline float smoothUnion(float a, float b, float k) {
  float h = clamp01(0.5f + 0.5f * (b - a) / k);
  return b + (a - b) * h - k * h * (1.0f - h);
}

inline float eyeDistance(const Prepared &q, float x, float y, float side) {
  const FaceParams &p = *q.p;
  float ex = x - side * p.eyeGap - p.lookX;
  float ey = y - p.eyeY - p.lookY;
  float c = q.browC;
  float s = q.browS * side;
  float rx = ex * c - ey * s;
  float ry = ex * s + ey * c;

  float d = sdRoundBox(rx, ry, p.eyeW, p.eyeH, p.eyeRadius);
  if (p.happy > 0.002f) {
    // A circle rising into the eye from below leaves the arch of a ^, and
    // sliding it up is the whole of open becoming delighted.
    float cy = ry - q.archOffset;
    float bitten = -(sqrtf(rx * rx + cy * cy) - q.archRadius);
    if (bitten > d) {
      d = bitten;
    }
  }
  return d;
}

inline float mouthDistance(const Prepared &q, float x, float y) {
  const FaceParams &p = *q.p;
  float my = y - p.mouthY;
  float d = sdArc(x - p.mouthSplit, my, q.mouthS, q.mouthC, p.mouthR, p.mouthT);
  if (p.mouthSplit > 0.002f) {
    float other = sdArc(x + p.mouthSplit, my, q.mouthS, q.mouthC, p.mouthR, p.mouthT);
    // Blended over the thickness rather than the radius: wide enough that the
    // two lobes join instead of crossing, narrow enough to leave the cusp
    // between them, which is the whole of what makes it an omega.
    d = smoothUnion(d, other, p.mouthT);
  }
  return d;
}

inline float faceDistance(const Prepared &q, float sx, float sy) {
  float dx = sx - q.p->x;
  float dy = sy - q.p->y;
  float fx = q.m00 * dx + q.m01 * dy;
  float fy = q.m10 * dx + q.m11 * dy;

  float d = eyeDistance(q, fx, fy, -1.0f);
  float right = eyeDistance(q, fx, fy, 1.0f);
  if (right < d) {
    d = right;
  }
  float mouth = mouthDistance(q, fx, fy);
  if (mouth < d) {
    d = mouth;
  }
  return d * q.dscale;
}

inline uint16_t shade(float d) {
  float core = clamp01(0.5f - d);
  float halo = 0.0f;
  if (d < GLOW_RADIUS) {
    float t = clamp01(1.0f - d / GLOW_RADIUS);
    halo = t * t * GLOW_GAIN * (1.0f - core);
  }
  // White in the middle and the halo a shade cooler, so what spills past the
  // edge reads as something lit rather than as an edge that was drawn badly.
  uint8_t r = (uint8_t)(clamp01(core + halo * 0.40f) * 255.0f);
  uint8_t g = (uint8_t)(clamp01(core + halo * 0.62f) * 255.0f);
  uint8_t b = (uint8_t)(clamp01(core + halo * 0.95f) * 255.0f);
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

inline int16_t clampAxis(float v, int16_t hi) {
  if (v < 0.0f) {
    return 0;
  }
  return v > (float)hi ? hi : (int16_t)v;
}

}  // namespace

Rect faceRender(uint16_t *fb, const FaceParams &p) {
  Prepared q;
  q.p = &p;
  q.browC = cosf(p.brow);
  q.browS = sinf(p.brow);
  q.mouthS = sinf(p.mouthOpen);
  q.mouthC = cosf(p.mouthOpen);
  q.archRadius = p.eyeH * 2.5f;
  q.archOffset = p.eyeH * (3.5f - 1.65f * p.happy);

  // Screen delta back into face space: undo the tilt, then the stretch in the
  // frame it was applied in, then the breath. Folded into one 2x2 here so the
  // per-pixel cost is four multiplies.
  float cs = cosf(p.stretchAngle);
  float ss = sinf(p.stretchAngle);
  float inv = 1.0f / p.stretch;
  float s00 = inv * cs * cs + p.stretch * ss * ss;
  float s01 = cs * ss * (inv - p.stretch);
  float s11 = inv * ss * ss + p.stretch * cs * cs;
  float ct = cosf(p.tilt);
  float st = sinf(p.tilt);
  float b = 1.0f / p.breath;
  q.m00 = (s00 * ct - s01 * st) * b;
  q.m01 = (s00 * st + s01 * ct) * b;
  q.m10 = (s01 * ct - s11 * st) * b;
  q.m11 = (s01 * st + s11 * ct) * b;
  // A distance in face space is at least this many pixels on the panel. Under
  // rather than over, so the tile test below can never skip a tile it should
  // have drawn.
  q.dscale = p.breath * (p.stretch < 1.0f ? p.stretch : inv);

  float spanX = p.eyeGap + p.eyeW + fabsf(p.lookX);
  float mouthSpan = p.mouthSplit + p.mouthR + p.mouthT;
  if (mouthSpan > spanX) {
    spanX = mouthSpan;
  }
  float top = fabsf(p.eyeY + p.lookY) + p.eyeH;
  float bottom = p.mouthY + p.mouthR + p.mouthT;
  float spanY = top > bottom ? top : bottom;
  float reach = sqrtf(spanX * spanX + spanY * spanY) * p.breath * p.stretch + GLOW_RADIUS + 2.0f;

  Rect box;
  box.x0 = clampAxis(p.x - reach, SCREEN_W);
  box.x1 = clampAxis(p.x + reach + 1.0f, SCREEN_W);
  box.y0 = clampAxis(p.y - reach, SCREEN_H);
  box.y1 = clampAxis(p.y + reach + 1.0f, SCREEN_H);

  for (int16_t ty = box.y0; ty < box.y1; ty += TILE) {
    int16_t th = (int16_t)((ty + TILE < box.y1) ? TILE : box.y1 - ty);
    for (int16_t tx = box.x0; tx < box.x1; tx += TILE) {
      int16_t tw = (int16_t)((tx + TILE < box.x1) ? TILE : box.x1 - tx);

      // A signed distance says how far the face is from anywhere near a point,
      // not just from the point, so one probe at the centre of a tile clears
      // the whole tile when the answer is big enough. On most frames that is
      // most of the box, and it is the difference between this running and not.
      float centre = faceDistance(q, tx + tw * 0.5f, ty + th * 0.5f);
      float diagonal = 0.5f * sqrtf((float)(tw * tw + th * th));
      if (centre > diagonal + GLOW_RADIUS + 1.0f) {
        for (int16_t y = 0; y < th; y++) {
          memset(fb + (int32_t)(ty + y) * SCREEN_W + tx, 0, (size_t)tw * sizeof(uint16_t));
        }
        continue;
      }

      for (int16_t y = 0; y < th; y++) {
        uint16_t *row = fb + (int32_t)(ty + y) * SCREEN_W + tx;
        float sy = ty + y + 0.5f;
        for (int16_t x = 0; x < tw; x++) {
          row[x] = shade(faceDistance(q, tx + x + 0.5f, sy));
        }
      }
    }
  }
  return box;
}

void faceClear(uint16_t *fb, const Rect &r) {
  for (int16_t y = r.y0; y < r.y1; y++) {
    memset(fb + (int32_t)y * SCREEN_W + r.x0, 0, (size_t)(r.x1 - r.x0) * sizeof(uint16_t));
  }
}
