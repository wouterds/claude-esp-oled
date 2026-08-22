# Hardware

A Waveshare **ESP32-S3-Touch-LCD-1.85B**: an **ESP32-S3R8** - 16MB of flash and
8MB of octal PSRAM - behind a 1.85" **round** 360x360 IPS panel in a machined
aluminium case. The panel is a **ST77916** on QSPI. A **CST816S** touch
controller, a **BQ27220** battery gauge, a **QMI8658** IMU, **ES8311**/**ES7210**
audio and a **PCF85063** clock are also on it, all on one I2C bus.

**Not the same board as the ESP32-S3-Touch-LCD-1.85.** Same size, same panel,
same QSPI pins - and on the non-B, LCD reset is behind a TCA9554 IO expander
that this one does not have. Firmware written for one comes up black on the
other. Waveshare document them as separate products; the B's page is the one to
read.

## Wiring

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
| USB | native, `D- GPIO19`, `D+ GPIO20` |
| BOOT button | `GPIO0`, pulled up, low when pressed |

## Building and flashing

PlatformIO, installed however you like as long as **pip is in the same
environment**. `pio` lands in `~/.platformio/penv/bin` and a non-interactive
shell will not have it on `PATH`. Installed with `uv tool install platformio` alone, PlatformIO
cannot install esptool's python dependencies and the build dies on
`MissingPackageManifestError` a long way from the cause:

```bash
uv tool install --with pip platformio

npm run firmware:build
npm run firmware:flash
npm run firmware:monitor
```

## The things that cost a day

**Plugging in the USB-C does not turn the board on.** It has a battery gauge and
a soft power path, so the power button is a power button and not a reset:
**press it for 1s to power on, hold it 2-3s to power off**. An unpowered board
presents *no* USB device at all - not a broken one, none - so this reads as a
dead cable or a dead board rather than as a board that is simply switched off.
`ls /dev | grep cu.usbmodem` empty, and `ioreg -p IOUSB -w0 -l | grep 12346`
finding nothing, is what it looks like.

**The power button is the small one, beside the USB port.** Not the larger one
next to it, whatever the schematic calls them: pressed short, for one second,
and held for three, the large button did nothing observable on this unit while
USB was connected, and no reset ever reached the log from it. The small one
powers the board up and down on the timings above. Written down because it is
the reverse of what the product page implies and it costs an evening to
rediscover.

**The audio pins are named from both ends, one line apart.** Waveshare's summary
line gives `DOUT=GPIO47, DIN=GPIO39` from the *codec's* point of view; the pin
table directly beneath it gives `GPIO47 I2S_DOUT - playback data output` from
the *MCU's*. Take the summary at face value and playback is wired into the
microphone input. Nothing errors: every codec register still acknowledges, the
I2S peripheral still reports every byte written, and the amplifier still comes
up - so what you get is hiss and no sound, which reads as a dead speaker.

**Start I2S before configuring the codec.** The ES8311's clock manager is set up
against an MCLK that has to already be arriving. Configured in silence it takes
every register write, acknowledges all of them, and does nothing with them.

**`boot:0x2 (DOWNLOAD(USB/UART0))` in the log means `GPIO0` was low at reset**,
which means the boot strap was held down as the board came up. It will sit at
`waiting for download` forever and never reach the sketch. Let go of the button
and power it on again.

**The flash header must say 16MB and the PSRAM must say octal.** There is no
PlatformIO board definition for this module, so it is the generic S3 devkit
narrowed down - and that profile assumes 8MB of flash and quad PSRAM. Both are
silent when wrong. The wrong flash header stops the sketch before it reaches
`setup()`; `qio_qspi` instead of `qio_opi` leaves the 253KB framebuffer failing
to allocate with the panel already lit and nothing on it. `esptool flash-id`
reports the size the chip actually is, which is the thing to check it against.

**`ARDUINO_USB_CDC_ON_BOOT=1` is what points `Serial` at USB.** Left at the
devkit default of 0, `Serial` goes to the UART pins and the board looks mute
over USB while running perfectly well.

**Light the backlight after the first frame, not before.** The panel powers up
holding whatever was last in its RAM and shows that to the room for as long as
it takes to reach the first flush.

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
