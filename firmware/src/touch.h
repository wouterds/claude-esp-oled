#pragma once

#include <stdint.h>

// Which way a finger went. Up and down rather than left and right: the axis the
// controller calls X runs from the top of this glass to the bottom of it, which
// is a thing about how the panel is mounted rather than a thing about touch.
enum class Swipe : uint8_t { None, Up, Down };

// The CST816S, asked two things: whether the glass was tapped, and which way a
// finger went across it. Where it is while it is down is nobody's business.
//
// Read on its own core, woken by the controller's interrupt rather than by the
// frame. Both answers below are collected there and simply waiting when they
// are asked for.
void touchBegin();

// The glass was tapped: a finger went down and came up without going anywhere.
// Cleared by the asking.
bool touchTapped();

// A finger crossed the glass, reported while it was still down. Cleared by the
// asking, so one swipe cannot be acted on twice.
Swipe touchSwiped();
