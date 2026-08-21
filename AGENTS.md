# AGENTS.md

A desk pet. A face that lives on a 360x360 round display on an ESP32-S3, and
does nothing else - it is not told anything, it does not ask, and it has no
menus. The whole of it is on the board.

| | |
| --- | --- |
| `firmware/src/board.*` | the panel: QSPI bring-up, the framebuffer, the flush |
| `firmware/src/face.*` | what a face is, as signed distances |
| `firmware/src/buddy.*` | what it does: wander, dart, hop, look, giggle |
| `firmware/src/main.cpp` | setup, frame pacing, and the self-test |
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
| `npm run firmware:flash:test` | the same, for the self-test |
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

- **The whole pet is on the board.** A 360x360 frame at 16 bits is 253KB and no
  serial link streams that thirty times a second, so the scene, the timing and
  the mood are all on the S3 and iterating on behaviour costs a reflash rather
  than a restart. The resolution forced that rather than a preference
- **A shape is a distance, not a span of pixels.** Every part of the face is a
  signed distance field, so coverage falls out of the distance and the edges are
  smooth without a second pass - and an eye becomes a squint, or a smile a
  surprised o, by moving a number rather than branching to another shape. It is
  also what makes it affordable: one probe at the centre of a tile bounds the
  whole tile, which clears most of the panel without touching it
- **Scenes are pure functions of time and state.** No scene carries anything
  between frames, which is what lets the same `FaceParams` always draw the same
  pixels and a face be something you can hold still and look at
- **The panel is round.** 360x360 of framebuffer, but the corners are not
  clipped - they are not there. Anything drawn outside the inscribed circle is
  rendered, paid for and never seen, which is what the roam radius is for
- **Black is never black.** It is an IPS LCD: black is the crystal blocking a
  backlight that is always on, and it never blocks all of it. A pixel cannot be
  off. The backlight duty is the only control over how black black looks, and it
  dims the face with it - which is the whole trade on a face drawn in white on
  black
- **The flush waits on its own DMA.** `esp_lcd_panel_draw_bitmap` queues a
  transfer and returns, so refilling the band buffer walks over a transfer in
  flight and the panel shows bands from two frames at once. That reads as a
  broken panel rather than as a race
- **The framebuffer is in PSRAM and the SPI DMA cannot reach it.** Every band is
  copied down into internal RAM on its way out, and that copy is also where the
  bytes get swapped, which this panel wants
- **The Type-C does not power the board on** and **BOOT held low traps it in the
  bootloader**. Both present as a dead board and neither says so; see the
  hardware guide before suspecting anything in here
