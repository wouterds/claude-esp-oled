#pragma once

#include <stdint.h>

// Every pin this board carries, in one place, because the pins are the whole
// difference between the boards this firmware runs on and a pin map spread
// across the files that use it cannot be read as a map.
//
// The panel is round and 360 across. Everything drawn on it is clipped by the
// glass rather than by the framebuffer, so a scene has to keep itself inside
// the inscribed circle - the corners of the buffer are simply not there.
static constexpr int16_t SCREEN_W = 360;
static constexpr int16_t SCREEN_H = 360;
static constexpr float SCREEN_R = 180.0f;

static constexpr int LCD_CS = 21;
static constexpr int LCD_SCK = 40;
static constexpr int LCD_D0 = 46;
static constexpr int LCD_D1 = 45;
static constexpr int LCD_D2 = 42;
static constexpr int LCD_D3 = 41;
// Straight off the S3, no expander. The non-B board of this size puts this
// behind a TCA9554 and firmware written for one comes up black on the other.
static constexpr int LCD_RST = 3;
static constexpr int LCD_BL = 5;

static constexpr int I2C_SDA = 11;
static constexpr int I2C_SCL = 10;

static constexpr int TOUCH_RST = 1;
static constexpr int TOUCH_INT = 4;
static constexpr uint8_t TOUCH_ADDR = 0x15;

static constexpr int AUDIO_MCLK = 2;
static constexpr int AUDIO_BCLK = 48;
static constexpr int AUDIO_LRCK = 38;
// Playback out on 47 and the microphones in on 39, which is the way round the
// board's own pin table has it - the summary line above that table calls them
// DOUT and DIN from the codec's point of view instead, and taking that at face
// value sends the sound to the microphones. That reads as an amplifier with
// nothing arriving at it, which is to say: hiss.
static constexpr int AUDIO_DOUT = 47;
static constexpr int AUDIO_DIN = 39;
// The amplifier is only awake while there is something to say. Left on it puts
// a hiss on a board whose whole point is sitting quietly on a desk.
static constexpr int AUDIO_PA = 9;
// Seven bits. The board's own documentation gives 0x30, which is the same
// address written for a bus that counts the read/write bit as part of it.
static constexpr uint8_t CODEC_ADDR = 0x18;

static constexpr uint8_t GAUGE_ADDR = 0x55;

// Pulled up on the board, so it reads low while it is held.
static constexpr int BUTTON_PIN = 0;
