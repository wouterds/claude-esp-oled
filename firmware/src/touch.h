#pragma once

#include <stdint.h>

// Which way a finger went. The axis the controller calls X runs from the top of
// this glass to the bottom of it and the one it calls Y runs across - a thing
// about how the panel is mounted rather than a thing about touch, and the
// reason the two are named along and across in here rather than x and y.
//
// One swipe, never two: a finger that wanders diagonally is taken to have gone
// whichever way it went furthest, so a page turn is never ambiguous.
enum class Swipe : uint8_t { None, Up, Down, Left, Right };

// The touch controller, asked one thing: which way a finger went across it.
// Where it is while it is down, and whether it stayed put, is nobody's
// business - the button says what the taps used to.
//
// Read on its own core, woken by the controller's interrupt rather than by the
// frame. The answer below is collected there and simply waiting when it is
// asked for.
void touchBegin();

// A finger crossed the glass, reported while it was still down. Cleared by the
// asking, so one swipe cannot be acted on twice.
Swipe touchSwiped();

// Where the finger is right now, in the scene's own pixels, or false with
// nobody on the glass. For the one page with something on it to hold.
bool touchFinger(int16_t *across, int16_t *along);

// Two taps in quick succession, neither of which went anywhere, and where the
// first of them landed. Cleared by the asking.
bool touchDoubleTapped(int16_t *across, int16_t *along);

// One tap, landed and lifted without going anywhere and with no second one
// behind it, at where it landed. Cleared by the asking. Reported a window late
// rather than on the way up: a tap is only a single once nothing has followed
// it, or every double tap would ring this first.
bool touchTapped(int16_t *across, int16_t *along);
