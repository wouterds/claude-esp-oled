# AGENTS.md

A 360x360 round LCD on an ESP32-S3, driven directly. It is not told anything and
it does not ask - what it draws is on the board, and right now that is nine
half-opacity balls bouncing off the inside of the glass over a grid.

| | |
| --- | --- |
| `firmware/src/board.*` | the panel: QSPI bring-up, the framebuffer, the flush |
| `firmware/src/scene.*` | what is drawn: the balls, the bounce, the grid |
| `firmware/src/main.cpp` | setup, the button, and frame pacing |
| `firmware/src/esp_lcd_st77916.*` | Waveshare's panel driver, vendored |
| `firmware/src/st77916_waveshare.h` | their init tables, verbatim |

## Stack

Arduino via PlatformIO, on the **pioarduino** fork of the ESP32 platform - the
official one is still on Arduino core 2.x. The panel goes through ESP-IDF's
`esp_lcd` rather than a graphics library, because none of them drive this glass
correctly. There is no host software: npm is here for the commit hooks and to
wrap two commands.

## Commands

| | |
| --- | --- |
| `npm run firmware:build` | compile |
| `npm run firmware:flash` | compile, flash, reset, and print the boot log |
| `npm run firmware:monitor` | watch the serial log |

## Rules

- **NEVER** bypass pre-commit hooks (`--no-verify`, `LEFTHOOK=0`)
- **NEVER** commit without being explicitly asked
- **NEVER** put a Claude or co-author trailer on a commit
- Atomic commits, conventional messages, max 100 chars per line
- Prefer the smallest change that does the job
- Find root causes - no temporary or hacky fixes

## Guides

- [Hardware](.agents/docs/hardware.md) - the board, its wiring, flashing, and
  the things that cost a day
- [Code Standards](.agents/docs/code-standards.md) - style, comments, and what
  a comment has to earn

## Constraints

Invisible in the code until they are broken.

- **Everything is on the board.** A 360x360 frame at 16 bits is 253KB and no
  serial link streams that thirty times a second, so the scene and its timing
  live on the S3 and changing either costs a reflash rather than a restart
- **The panel is round.** 360x360 of framebuffer, but the corners are not
  clipped - they are not there. Anything drawn outside the inscribed circle is
  rendered, paid for and never seen, which is why the balls reflect about the
  radius rather than off a wall
- **Black is never black.** It is an IPS LCD: black is the crystal blocking a
  backlight that is always on, and it never blocks all of it. A pixel cannot be
  off. The backlight duty is the only control over how black black looks, and it
  dims everything else with it
- **The flush waits on its own DMA.** `esp_lcd_panel_draw_bitmap` queues a
  transfer and returns, so refilling the band buffer walks over a transfer in
  flight and the panel shows bands from two frames at once. That reads as a
  broken panel rather than as a race
- **The framebuffer is in PSRAM and the SPI DMA cannot reach it.** Every band is
  copied down into internal RAM on its way out, and that copy is also where the
  bytes get swapped, which this panel wants
- **The power button is not wired to the chip.** It switches the power path,
  supports no custom function, and cannot be read, intercepted or stood in for -
  so there is no firmware answer to "turn it off", only a deep sleep. It is the
  small button beside the USB port, not the larger one; see the hardware guide
- **The Type-C does not power the board on**, **the boot strap held low traps it
  in the bootloader**, and **opening the serial port resets it**. All three
  present as a dead or possessed board and none of them says so; see the
  hardware guide before suspecting anything in here
