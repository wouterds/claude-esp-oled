# claude-esp-oled

A round display on an ESP32-S3, driven straight from the chip. A face
wanders the glass wearing how much of your claude.ai allowance is gone, with a
bar down each edge for the five hour window and the week.

```bash
uv tool install --with pip platformio   # see the hardware guide for the --with
npm install
cp firmware/.env.example firmware/.env  # a network to join: there is no AP
npm run firmware:flash
```

That builds, flashes, resets the board and prints the boot log. Networks come
from `firmware/.env`; once it has joined one it writes its address on the glass,
and the page there takes a claude.ai session token - which is kept on the device
and only ever sent back to the origin it came from.

`BOOT` puts the two percentages up, with a countdown to the next reset under the
face; press it again to take them away. It used to be a double-tap on the glass
and is not any more - a sleeve managed the first of them often enough to be a
nuisance.

Swipe up for the charge, the network, the address and the commit it is running,
and down again to send it back. Swipe down from the face for brightness and
volume - tap or drag either slider, and it remembers - and up to put it away.
Double-tap the cat and it takes the whole glass; double-tap again to give it
back.

Sideways there is a row: five market screens one way - three coins and two index
futures, each with a day's trace - and the box in the corner of the room the
other, with its load, its temperature and what it is drawing. That last one
reads `nuc.cpp`'s own hardcoded address, which is a machine of mine; with
nothing answering there the page sits at `NO DATA`.

`PWR` switches the power path and the chip cannot see it - it is the small one
beside the USB-C, pressed for a second to power on and held for three to power
off. `BOOT` is the strapping pin, and held down at reset it traps the board in
the bootloader - but nothing about the strap applies to a board that is already
up, so the firmware reads it as a button. Press it and it means whatever page is
in front of it. Hold it for three seconds and the board photographs its own
glass, says so with a shutter, and keeps the last ten PNGs on its flash; they
are listed on the page above the request log, and open in a tab from there.

Hardware is either a **Waveshare ESP32-S3-Touch-LCD-1.85B** - a 360x360 IPS LCD,
where despite the name of this repository black is backlight leaking through
liquid crystal rather than a pixel that is off - or a **Waveshare
ESP32-S3-Touch-AMOLED-1.75C**, a 466x466 AMOLED where it really is off.

`npm run firmware:flash` works out which board each port is by the size of its
flash and writes that board's image to it - every one plugged in, so two
attached get one each. It has to work it out: the two boards' pins overlap rather than
merely differ, so the wrong image drives the wrong silicon on every line and
looks like dead hardware.

See [.agents/docs/hardware.md](.agents/docs/hardware.md) for the board -
including the things that each present as a dead board and none of which say so.

<img width="4032" height="3024" alt="IMG_2385" src="https://github.com/user-attachments/assets/a7326e64-50d3-4f40-8865-e39214a26606" />
