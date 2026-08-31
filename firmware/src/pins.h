#pragma once

#include <stdint.h>

// Both boards' pins, side by side, because what bites here is not that they
// differ but that they overlap: GPIO5 is the LCD board's backlight and one of
// the AMOLED board's four data lines, and GPIO11 is the LCD board's I2C data
// and the AMOLED board's touch interrupt. Firmware built for one and flashed to
// the other does not fail - it drives the wrong silicon on every line it has.
//
// Which board is picked by the env in platformio.ini, one each.

#if !defined(BOARD_LCD_185B) && !defined(BOARD_AMOLED_175C)
#error "no board selected - build through one of platformio.ini's envs"
#endif

#if defined(BOARD_LCD_185B)

// Waveshare ESP32-S3-Touch-LCD-1.85B: 16MB of flash, 8MB of octal PSRAM, and a
// 360x360 round IPS panel - a ST77916 on QSPI - in a machined aluminium case.
//
// The panel is round. Everything drawn on it is clipped by the glass rather
// than by the framebuffer, so a scene has to keep itself inside the inscribed
// circle - the corners of the buffer are simply not there.
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

#else

// Waveshare ESP32-S3-Touch-AMOLED-1.75C: 32MB of flash, 8MB of PSRAM, and a
// 466x466 round AMOLED - a CO5300 on QSPI - with CST9217 touch.
//
// Not the 1.75. That one is 16MB of flash where this is 32, which is the same
// shape of trap the B suffix is on the other board, and the flash header being
// wrong is silent. `esptool flash-id` is what tells them apart.
//
// The pin numbers below are Waveshare's own, and their repository states they
// were cross-checked against the board schematic.
static constexpr int16_t SCREEN_W = 466;
static constexpr int16_t SCREEN_H = 466;
static constexpr float SCREEN_R = 233.0f;

static constexpr int LCD_CS = 12;
static constexpr int LCD_SCK = 38;
static constexpr int LCD_D0 = 4;
static constexpr int LCD_D1 = 5;
static constexpr int LCD_D2 = 6;
static constexpr int LCD_D3 = 7;
static constexpr int LCD_RST = 1;
// No LCD_BL. The pixels emit their own light, so there is no backlight to dim
// and brightness is a register on the panel - see panel_co5300.cpp.

static constexpr int I2C_SDA = 15;
static constexpr int I2C_SCL = 14;

static constexpr int TOUCH_RST = 2;
static constexpr int TOUCH_INT = 11;
static constexpr uint8_t TOUCH_ADDR = 0x5A;

static constexpr int AUDIO_MCLK = 16;
static constexpr int AUDIO_BCLK = 9;
static constexpr int AUDIO_LRCK = 45;
// Named from the codec's end in Waveshare's header - ES8311_DOUT for the pin
// the S3 plays out of, ES7210_DIN for the one the microphones arrive on. The
// other board documents the same two pins from the MCU's end one line apart,
// and taking either at face value is how playback ends up in the microphones.
static constexpr int AUDIO_DOUT = 8;
static constexpr int AUDIO_DIN = 10;
static constexpr int AUDIO_PA = 46;
static constexpr uint8_t CODEC_ADDR = 0x18;

// No fuel gauge on this board. The pack is behind an AXP2101 PMIC at 0x34 and
// nothing answers at the address below, which battery.cpp reads - correctly -
// as no pack attached. Reading the charge through the PMIC is not written yet.
static constexpr uint8_t GAUGE_ADDR = 0x55;

#endif

// Pulled up on the board, so it reads low while it is held. The strapping pin
// on both, which is why it is read and never driven.
static constexpr int BUTTON_PIN = 0;
