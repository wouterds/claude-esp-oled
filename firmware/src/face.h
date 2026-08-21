#pragma once

#include <stdint.h>

// A pair of eyes and nothing else. They drift around the glass, and every five
// seconds they take on an expression and every five after that let it go.
void faceBegin();
void faceStep(float dt);
void faceDraw(uint16_t *fb);
