#pragma once

#include <stdint.h>

// Which way a finger crossed the glass on its way off it.
enum class Swipe : uint8_t { None, Left, Right };

// The CST816S, asked two things: where a finger landed, and which way it went
// before it left. Where it is while it is still down is nobody's business here.
void touchBegin();

// Takes whatever the controller has reported since the last frame. Both answers
// below are only ever as new as this, so it comes first.
void touchStep();

// A finger has just landed. The landing, not the holding: a finger left on the
// glass is one tap.
bool touchTapped();

// How the last finger left, once. Cleared by the asking, so one swipe cannot be
// acted on twice.
Swipe touchSwiped();
