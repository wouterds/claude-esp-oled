# Hardware

Two boards, one firmware, an env each. Both are an **ESP32-S3** with 8MB of
**octal** PSRAM driving a round touch panel over QSPI, and very little else
about them is the same.

| | ESP32-S3-Touch-LCD-1.85B | ESP32-S3-Touch-AMOLED-1.75C |
| --- | --- | --- |
| env | `esp32-s3-touch-lcd-185` | `esp32-s3-touch-amoled-175c` |
| glass | 360x360 round **IPS LCD** | 466x466 round **AMOLED** |
| controller | ST77916 | CO5300 |
| flash | **16MB** | **32MB** |
| touch | CST816S at `0x15` | CST9217 at `0x5A` |
| brightness | PWM on a backlight pin | panel register `0x51`, no backlight |
| battery | BQ27220 gauge at `0x55` | AXP2101 PMIC at `0x34` |
| QSPI clock | 80MHz | 80MHz |
| also on I2C | QMI8658 IMU, PCF85063 clock, ES8311/ES7210 audio | the same four |

**The images are not interchangeable, and the wrong one does not fail.** The
pins overlap rather than merely differ: `GPIO5` is the LCD board's backlight and
one of the AMOLED board's four LCD data lines, `GPIO11` is the LCD board's I2C
data and the AMOLED board's touch interrupt. Flashed the wrong way round, every
line drives the wrong silicon and the result looks like dead hardware.

## Telling them apart

Nothing on either board announces which it is, and a board may already be
holding the wrong firmware - so the question has to be answerable without
running anything on it. The flash chip answers it:

```bash
esptool --port /dev/cu.usbmodem1101 flash-id   # "Detected flash size: 16MB" or "32MB"
```

`npm run firmware:flash` reads that itself and flashes the matching env. Naming
an env explicitly still works and is refused when it is the other board.

**Neither is the plain-numbered board of the same size.** The
ESP32-S3-Touch-AMOLED-**1.75** has 16MB of flash where the **1.75C** has 32, and
the ESP32-S3-Touch-LCD-**1.85** puts its LCD reset behind a TCA9554 where the
**1.85B** wires it to the S3. Both differences are silent when wrong, and both
vendors document the variants as separate products. Read the page with the
suffix.

## Wiring

### ESP32-S3-Touch-LCD-1.85B

| | |
| --- | --- |
| LCD data | `GPIO46` `GPIO45` `GPIO42` `GPIO41` (D0..D3) |
| LCD clock / select | `GPIO40` / `GPIO21` |
| LCD reset | **`GPIO3`** - straight off the S3, no expander |
| LCD backlight | `GPIO5`, PWM capable |
| I2C - touch, IMU, audio, gauge, clock | `SDA GPIO11`, `SCL GPIO10` |
| Audio clocks | `MCLK GPIO2`, `BCLK GPIO48`, `LRCK GPIO38` |
| Audio data | **playback out `GPIO47`**, microphones in `GPIO39` |
| Amplifier enable | `GPIO9` |
| Touch | `CST816S` at `0x15`, reset `GPIO1`, interrupt `GPIO4` |

### ESP32-S3-Touch-AMOLED-1.75C

| | |
| --- | --- |
| LCD data | `GPIO4` `GPIO5` `GPIO6` `GPIO7` (D0..D3) |
| LCD clock / select | `GPIO38` / `GPIO12` |
| LCD reset | `GPIO1` |
| LCD backlight | **none** - the pixels emit, see below |
| I2C - touch, IMU, audio, PMIC, clock | `SDA GPIO15`, `SCL GPIO14` |
| Audio clocks | `MCLK GPIO16`, `BCLK GPIO9`, `LRCK GPIO45` |
| Audio data | **playback out `GPIO8`**, microphones in `GPIO10` |
| Amplifier enable | `GPIO46` |
| Touch | `CST9217` at `0x5A`, reset `GPIO2`, interrupt `GPIO11` |

Both: USB is native, `D- GPIO19` / `D+ GPIO20`, and the `BOOT` button is
`GPIO0`, pulled up and low when pressed.

## Building and flashing

PlatformIO, installed however you like as long as **pip is in the same
environment**. `pio` lands in `~/.platformio/penv/bin` and a non-interactive
shell will not have it on `PATH`. Installed with `uv tool install platformio` alone, PlatformIO
cannot install esptool's python dependencies and the build dies on
`MissingPackageManifestError` a long way from the cause:

```bash
uv tool install --with pip platformio

npm run firmware:build     # both envs
npm run firmware:flash     # detects the board, flashes it, prints the boot log
npm run firmware:monitor
```

A good boot log names the panel, the touch part and the codec. On the AMOLED
board it reads:

```
panel init: ESP_OK
panel up, psram free 7943584
brightness: 100% (ESP_OK)
battery: 4153 mV, 94%, external power
touch: CST9217 ready
audio: ES8311 ready at 16000 Hz, 10%
```

`psram free` in the sevens is how you know the octal PSRAM setting took.

## The things that cost a day

**Plugging in the USB-C does not turn the board on.** It has a battery gauge and
a soft power path, so the power button is a power button and not a reset:
**press it for 1s to power on, hold it 2-3s to power off**. An unpowered board
presents *no* USB device at all - not a broken one, none - so this reads as a
dead cable or a dead board rather than as a board that is simply switched off.
`ls /dev | grep cu.usbmodem` empty, and `ioreg -p IOUSB -w0 -l | grep 12346`
finding nothing, is what it looks like.

**The AMOLED board holds off for longer than you will.** Its button goes to the
AXP2101 rather than to a power path, and the PMIC decides what a press means:
**a short press turns it on, and a hold of about six seconds turns it off**. Six
is the figure in the part, not a guess - `0x27` reads `0x14`, which is its
power-off level of 6s and its power-on level of 128ms - and it is twice the hold
the LCD board wants. Let go at the two or three seconds that work on the other
board and nothing happens at all, which reads as a button that does not work.

If it still will not go off, read `0x21` (`PWROFF_STATUS`) and `0x20`
(`PWRON_STATUS`) off the PMIC, and enable the power key's four interrupts in
`0x41` to watch `0x49` for them. The key's presses, releases, short and long
holds are all bits in there, so unlike the LCD board's button this one is
visible to the firmware - which is worth knowing before anything is built on
"there is no second button here". Whether they actually fire on this board has
not been confirmed.

**The power button is the small one, beside the USB port.** Not the larger one
next to it, whatever the schematic calls them: pressed short, for one second,
and held for three, the large button did nothing observable on this unit while
USB was connected, and no reset ever reached the log from it. The small one
powers the board up and down on the timings above. Written down because it is
the reverse of what the product page implies and it costs an evening to
rediscover.

**The audio pins are named from both ends, one line apart.** On the LCD board
Waveshare's summary line gives `DOUT=GPIO47, DIN=GPIO39` from the *codec's*
point of view; the pin table directly beneath it gives `GPIO47 I2S_DOUT -
playback data output` from the *MCU's*. The AMOLED board's header names them
from the codec's end only - `ES8311_DOUT` for the pin the S3 plays out of.
Take either at face value and playback is wired into the microphone input.
Nothing errors: every codec register still acknowledges, the I2S peripheral
still reports every byte written, and the amplifier still comes up - so what you
get is hiss and no sound, which reads as a dead speaker.

**Start I2S before configuring the codec.** The ES8311's clock manager is set up
against an MCLK that has to already be arriving. Configured in silence it takes
every register write, acknowledges all of them, and does nothing with them.

**`boot:0x2 (DOWNLOAD(USB/UART0))` in the log means `GPIO0` was low at reset**,
which means the boot strap was held down as the board came up. It will sit at
`waiting for download` forever and never reach the sketch. Let go of the button
and power it on again.

**The flash header must say what the chip is, and the PSRAM must say octal.**
There is no PlatformIO board definition for either module, so both are the
generic S3 devkit narrowed down - and that profile assumes 8MB of flash and quad
PSRAM. Both are silent when wrong. The wrong flash header stops the sketch
before it reaches `setup()`; `qio_qspi` instead of `qio_opi` leaves the
framebuffer failing to allocate with the panel already lit and nothing on it.
`esptool flash-id` reports the size the chip actually is, which is the thing to
check it against.

**`ARDUINO_USB_CDC_ON_BOOT=1` is what points `Serial` at USB.** Left at the
devkit default of 0, `Serial` goes to the UART pins and the board looks mute
over USB while running perfectly well.

**Light the panel after the first frame, not before.** Both panels power up
holding whatever was last in their RAM and show that to the room for as long as
it takes to reach the first flush. The LCD board raises a backlight to do it and
the AMOLED board writes a brightness register, but the ordering is the same and
`boardBegin()` owns it.

**On the AMOLED board, black is black.** The constraint in `AGENTS.md` about an
IPS panel never fully blocking its backlight does not apply to emissive pixels -
there is no backlight, brightness is panel register `0x51`, and an unlit pixel
is off. Anything written to work around LCD black will look wrong here rather
than merely unnecessary.

**The AMOLED panel needs nothing from the AXP2101.** The board carries a PMIC
and it is reasonable to assume the display rails hang off it, which is a day
spent bringing up a PMIC that was never in the way. Waveshare's own
`01_HelloWorld` drives the panel with nothing but `Wire.begin()` and
`gfx->begin()`. The PMIC does own the battery, which is why there is no charge
on that board's status line yet.

**The CO5300's visible columns do not start at zero.** Its RAM is wider than the
466 that are lit and the panel is addressed from column six, which is the
`X_GAP` in `panel_co5300.cpp`. Wrong, this is a scene sitting a few pixels off
centre rather than anything that looks like a fault - and eight is the other
candidate, the same offset counted from the far side.

**The AMOLED panel's tearing-effect line is on `GPIO13`.** It is not in
Waveshare's pin header; it was found by watching every pin the board leaves
alone for one that ticks, and it carries a clean 60Hz pulse from the moment the
panel is initialised. Waiting for it before a flush costs eleven frames a second
here - a frame takes about 25ms and a scan 16.7, so the wait lands on every
second scan - and it did **not** stop static pixels appearing to move. Writing
across the scan is not what that is. The pin is written down because it is worth
knowing and hard to find, not because syncing to it helped.

**This panel does not talk back over QSPI.** Every register read returns zeroes,
the same as the LCD board, so there is no reading its memory back to compare
against the framebuffer. What is on the glass cannot be checked from the board -
only what was sent to it.

**The AMOLED's controller ignores the low bit of a window coordinate.** A
window asked to start on an odd row or column does not fail - it lands a pixel
off. Waveshare's own LVGL glue rounds every area to an even start and an odd end
before it is allowed near this glass, on both axes, and nothing else in their
stack ever sends an unaligned window, which is why the requirement is invisible
until you drive the panel yourself. It cost this project days, twice over:

- The face's flush band starts wherever the face is, so its start row's parity
  changed frame to frame - and every *static* pixel in those rows was parked one
  row up or down depending on which frame last sent it. Figures and bars that
  never moved in the framebuffer visibly danced at the frame rate, the
  framebuffer checksummed identical the whole time, and syncing to the tearing
  line changed nothing, because nothing about it was timing.
- A flush cut down to the face's columns as well as its rows (see the revert of
  be54577) put odd *column* starts in front of the panel too, and the face left
  slivers of itself a pixel to the side, where nothing would ever clear them.
  The box's arithmetic was checked repeatedly and was never wrong; its parity
  was. That flush is worth re-landing - it was two thirds of the flush and
  fifteen frames a second - now that windows are aligned.

The LCD board's ST77916 addresses single pixels and has no such rule, which is
why the same firmware never showed any of this on the other glass.

**The AMOLED board does not reach sixty frames a second.** It has 1.67x the
pixels of the LCD board, and a scene drawn a pixel at a time costs all of them.
Measured over 35 seconds of ordinary running: 15ms of drawing against 12.5ms of
flush, for 36 frames a second where the loop is pacing for 60.

**The board's own examples clock this glass at 40MHz, and 80 works.** 40 is the
graphics library's default for every panel it drives rather than a number this
one asked for. Measured over the same window, the clock is worth six frames a
second - 30 against 36, with the flush going from 17.1ms to 12.5ms.

Note what did *not* happen: doubling the clock did not halve the flush. Two
reasons, and both matter before optimising the wire again. The copy down from
PSRAM into a buffer the DMA can reach is the other half of a flush and does not
care about the clock; and the figure the log calls `flush` is timed across
`statusDraw` as well, so a good part of it is drawing rather than wire.

Take short samples with suspicion. A window caught just after boot, before the
radio and the usage poll have settled and while the face happens to be quiet,
reads several frames a second better than the same build does over a minute.
The two clocks were first compared that way and the answer came out wrong.

A QSPI bus run past what the panel takes does not fail, it corrupts the picture,
so this is the first thing to put back if frames come up torn or speckled.

**The scene is measured in the LCD board's pixels.** Every length in it was
composed against a 180px radius and is scaled by `SCENE` in `board.h` to
whatever glass it lands on. Seconds, rates and percentages are not lengths and
are deliberately left out of it. The factor is exactly 1 on the LCD board, so
its binary is unaffected by anything the AMOLED board needs. Text scales are
whole numbers and do not scale, so type is relatively smaller on the bigger
panel - that is a design decision nobody has made yet rather than a bug.

**The AMOLED board has no fuel gauge.** Its pack is behind the AXP2101, which
runs the charger and keeps a gauge of its own, so the charge, the voltage and
whether the cable is in are all read off the PMIC instead. Which board carries a
BQ27220 is decided at compile time and not by probing the bus: a gauge that has
browned out with its own pack answers exactly like one that was never fitted,
and those want opposite treatment - the first asked again when a pack turns up,
the second never again.

**Neither the PMIC's ADC nor its battery detector is on out of reset**, and both
are silent when off: the voltage reads a flat zero and the pack reads as absent.
Nothing above that cares whether a board has no battery or an unasked one, so
the icon and the whole battery page are simply not drawn - which looks like a
rendering fault rather than two bits that were never set.

**The official `espressif32` platform is still on Arduino core 2.x.** Anything
expecting core 3.0 dies inside its own headers with nothing pointing at the core
version as the cause - `esp32-hal-periman.h` not existing is the usual shape of
it. `platformio.ini` pins the **pioarduino** fork instead.

## When the port disappears

Flashing ends with `Hard resetting via RTS pin`, and that reset sometimes leaves
macOS with the USB device enumerated but **no CDC interface and no tty** - so
esptool has nothing to reflash through and it reads as a dead board. It is not.

`ioreg -w0 -r -n "USB JTAG/serial debug unit" -l` shows the device present with
no `IOUSBHostInterface` under it, which is the tell.

A power cycle fixes it, and on this board that is the power button rather than
the cable. Note also that **opening the serial port resets the board** - the log
starts `rst:0x15 (USB_UART_CHIP_RESET)` every time something attaches to it - so
a board that appears to come back when a button is pressed may only be the CDC
re-enumerating.
