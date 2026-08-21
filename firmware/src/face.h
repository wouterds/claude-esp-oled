#pragma once

#include <stdint.h>

// The region of the framebuffer a face last touched, so the next frame knows
// exactly how much of the panel it has to put back to black.
struct Rect {
  int16_t x0, y0, x1, y1;
};

// Everything there is to a face, in face-space pixels with the origin between
// the eyes and y running down. Nothing here reads the clock: the same params
// always draw the same pixels, which is what makes a face something you can
// hold still and look at rather than something you have to catch.
struct FaceParams {
  float x, y;              // where the face sits on the panel
  float tilt;              // head tilt, radians
  float stretch;           // >1 elongates along stretchAngle, thins across it
  float stretchAngle;
  float breath;            // whole-face scale

  float eyeGap;            // half the distance between the two eye centres
  float eyeW, eyeH;        // half extents of one eye
  float eyeRadius;         // corner rounding
  float eyeY;              // eye centres above the origin
  float lookX, lookY;      // where it is looking, as an offset of both eyes
  float happy;             // 0 open, 1 the closed arch of a ^
  float brow;              // radians, mirrored, so one sign slants both inward

  float mouthY;
  float mouthR;            // arc radius
  float mouthT;            // arc thickness
  float mouthOpen;         // arc half-angle: small is a smile, pi is an o
  float mouthSplit;        // parts the arc into the two lobes of an omega
};

// Draws the face and returns what it covered. Every pixel inside the returned
// rect is written, black included, so the caller only ever has to clear the
// rect from the frame before.
Rect faceRender(uint16_t *fb, const FaceParams &p);

void faceClear(uint16_t *fb, const Rect &r);
