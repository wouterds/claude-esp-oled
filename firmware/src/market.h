#pragma once

#include <stdint.h>

// The screens to the left of the face: two index futures and then three coins
// by market cap. All of them are the same shape of thing - a name, a price, how
// far it has moved and a line of where it has been - so all of them are one
// cache and one poller rather than five.
//
// Held coins first and indices after, which is not the order they are walked
// in. The coin screens are however many the ranking left, so counting from the
// end of the list is counting from somewhere that moves.
//
// Nothing is asked for unless its screen is the one up. What was last read is
// kept when the screen goes away, so coming back to it shows those figures at
// once and reads again behind them - an empty screen for the length of a
// handshake is the one thing worth avoiding here, and the handshake is seven
// seconds on this chip.

// Points a line is drawn from. Upstream gives an hour apiece over a week for a
// coin and a quarter of an hour over a day for an index, so 168 and 96 - both
// more than a chart this wide can show a difference between, and both thinned
// to this on the way in.
constexpr uint8_t MARKET_POINTS = 60;

// The three largest worth a screen. The ranking reads past them to get there -
// what sits near the top of that list is mostly priced in dollars rather than
// against the market, and none of that is worth swiping to.
constexpr uint8_t MARKET_COINS = 3;
// The futures only. The cash indices are shut for two thirds of the day and a
// screen showing Friday's close all weekend is a screen saying nothing; the
// contracts on them trade nearly around the clock and quote the same number.
constexpr uint8_t MARKET_INDICES = 2;
constexpr uint8_t MARKET_SCREENS = MARKET_COINS + MARKET_INDICES;

struct Market {
  char ticker[12];
  char name[24];
  // What the change is quoted over and what the line covers - "24H" against a
  // week of line for a coin, a session against a session for an index. They are
  // not the same span and saying which is the difference between a chart and a
  // decoration.
  char over[4];
  char span[4];
  float price;
  float change;
  // The ends of the line, worked out where it is parsed rather than per frame.
  float low;
  float high;
  float points[MARKET_POINTS];
  uint8_t count;
  // Read at least once. Until then there is nothing to show but the fact that
  // something is on its way.
  bool ready;
  bool loading;
  bool failed;
};

void marketBegin();

// How many coin screens there actually are. Ten until the ranking lands, and
// after that however many of the largest survived being pinned to a currency -
// there is no screen worth spending on a dollar being worth a dollar.
uint8_t marketCoins();

// Which screen is up, or -1 for none. Only that one is read, and only while it
// is up. Coming to a screen whose figures are older than the interval reads it
// at once rather than waiting the rest of the interval out.
void marketWatching(int8_t screen);

// A copy of what a screen has, or false past the end. Copied rather than
// pointed at because the poller writes these from its own task, and a price
// read half-written is a wrong price rather than a wrong pixel.
bool marketAt(uint8_t screen, Market *out);

// Moves whenever any of it does, so a page can tell it has something new to
// draw without comparing three hundred bytes to find out.
uint32_t marketRevision();
