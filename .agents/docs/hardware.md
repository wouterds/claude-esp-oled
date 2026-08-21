# Hardware

A Waveshare **ESP32-S3-Touch-LCD-1.85B**: an **ESP32-S3R8** - 16MB of flash and
8MB of octal PSRAM - behind a 1.85" **round** 360x360 IPS panel in a machined
aluminium case. The panel is a **ST77916** on QSPI. A **CST816S** touch
controller, a **QMI8658** IMU, **ES8311**/**ES7210** audio, a **PCF85063** clock
and a **BQ27220** battery gauge are also on it, all on one I2C bus, and nothing
here talks to any of them.

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
| Touch | `CST816S` at `0x15`, reset `GPIO1`, interrupt `GPIO4` |
| USB | native, `D- GPIO19`, `D+ GPIO20` |
| BOOT button | `GPIO0`, pulled up, low when pressed |

## Building and flashing

PlatformIO, installed however you like as long as **pip is in the same
environment**. Installed with `uv tool install platformio` alone, PlatformIO
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
a soft power path, so `PWR` is a power button and not a reset: **hold it 2s to
toggle power, or press it for 1s to power on**. An unpowered board presents *no*
USB device at all - not a broken one, none - so this reads as a dead cable or a
dead board rather than as a board that is simply switched off. `ls /dev | grep
cu.usbmodem` empty, and `ioreg -p IOUSB -w0 -l | grep 12346` finding nothing, is
what it looks like.

**`boot:0x2 (DOWNLOAD(USB/UART0))` in the log means `GPIO0` was low at reset**,
which almost always means `BOOT` is still being held. It will sit at `waiting for
download` forever and never reach the sketch. Let go of the button.

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

**The official `espressif32` platform is still on Arduino core 2.x.**
Arduino_GFX's ESP32 backend includes `esp32-hal-periman.h`, which only exists
from core 3.0, so the build dies inside the library's own headers with nothing
pointing at the core version as the cause. `platformio.ini` pins the
**pioarduino** fork instead.

## The panel is round

360x360 of framebuffer, but the glass is a circle. The corners are not clipped,
they are simply not there - anything drawn outside the inscribed circle is
rendered, paid for and never seen. Scenes keep themselves inside it, which is
what the balls bounce off.

## When the port disappears

Flashing ends with `Hard resetting via RTS pin`, and that reset sometimes leaves
macOS with the USB device enumerated but **no CDC interface and no tty** - so
esptool has nothing to reflash through and it reads as a dead board. It is not.

`ioreg -w0 -r -n "USB JTAG/serial debug unit" -l` shows the device present with
no `IOUSBHostInterface` under it, which is the tell.

A power cycle fixes it, and on this board that is the `PWR` button rather than
the cable.
