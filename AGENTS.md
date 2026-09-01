# AGENTS.md

Firmware for a round touch panel on an ESP32-S3, driven straight from the chip.
It builds for two boards - a 360x360 IPS LCD and a 466x466 AMOLED - which take
incompatible images, so read the hardware guide before flashing either.
Everything runs on the board and there is no host software, so every change
costs a reflash rather than a restart.

Do not turn this file, or the guides under `.agents/`, into a description of
what the code does - that rots the moment anything is edited and then it lies.
They say how to work here and what will bite; the code says what it does.

## Stack

Arduino via PlatformIO, on the **pioarduino** fork of the ESP32 platform - the
official one is still on Arduino core 2.x. The panel goes through ESP-IDF's
`esp_lcd` rather than a graphics library, because none of them drive this glass
correctly - one vendored panel driver per controller, one compiled at a time.
npm is here for the commit hooks and to wrap the three commands.

## Commands

| | |
| --- | --- |
| `npm run firmware:build` | compile both boards |
| `npm run firmware:flash` | compile, flash, reset, and print the boot log |
| `npm run firmware:monitor` | watch the serial log |

`firmware:flash` works out which board is plugged in from the size of its flash
and flashes that one. It is not a convenience: the two boards' pins overlap, so
the wrong image drives the wrong silicon rather than failing.

`pio` is on `PATH` in a login shell and often not in a non-interactive one; it
is at `~/.platformio/penv/bin/pio`, with its python beside it.

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

- **Everything is on the board.** A frame at 16 bits is 253KB on the LCD board
  and 434KB on the AMOLED one, and no serial link streams either of those thirty
  times a second, so what is drawn and what paces it both live on the S3
- **The panel is round.** 360x360 of framebuffer on one board and 466x466 on the
  other, but the corners are not clipped - they are not there. Anything drawn
  outside the inscribed circle is rendered, paid for and never seen, and anything
  near the edge has to be placed against the circle rather than against a row of
  pixels. Lengths are written in the LCD board's pixels and scaled by `SCENE`
- **Black is never black on the LCD board.** It is an IPS panel: black is the
  crystal blocking a backlight that is always on, and it never blocks all of it.
  A pixel cannot be off. The backlight duty is the only control over how black
  black looks, and it dims everything else with it. The AMOLED board is the
  opposite - the pixels emit, black is off, and there is no backlight at all,
  so anything written to work around LCD black is wrong rather than redundant
- **The flush waits on its own DMA.** `esp_lcd_panel_draw_bitmap` queues a
  transfer and returns, so refilling the band buffer walks over a transfer in
  flight and the panel shows bands from two frames at once. That reads as a
  broken panel rather than as a race
- **The framebuffer is in PSRAM and the SPI DMA cannot reach it.** Every band is
  copied down into internal RAM on its way out, and that copy is also where the
  bytes get swapped, which this panel wants
- **Partial flushes only clear what they redraw.** Whatever owns a band of rows
  clears and sends that band itself, so a band sized wider than the thing being
  written takes a bite out of whatever else lives in those rows
- **The power button is not wired to the chip.** On the LCD board it switches the
  power path, supports no custom function, and cannot be read, intercepted or
  stood in for - so there is no firmware answer to "turn it off", only a deep
  sleep. It is the small button beside the USB port, not the larger one. On the
  AMOLED board it goes to the PMIC instead, which wants a six second hold rather
  than two, and which does report the press over I2C; see the hardware guide
- **The Type-C does not power the board on**, **the boot strap held low traps it
  in the bootloader**, and **opening the serial port resets it**. All three
  present as a dead or possessed board and none of them says so; see the
  hardware guide before suspecting anything in here
- **The account's session token is on the device, not in the repo.** It is typed
  into the portal and kept in NVS, and
  `esptool read-flash 0x9000 0x5000 nvs.bin` with `strings` is how to read it
  back for debugging. claude.ai answers the board but puts curl from a laptop
  behind a Cloudflare challenge unless it is asked with browser headers and
  `--compressed`
