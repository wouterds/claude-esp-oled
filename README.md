# claude-esp-oled

A round display on an ESP32-S3, driven straight from the chip. A face
wanders the glass wearing how much of your claude.ai allowance is gone, with a
bar down each edge for the five hour window and the week.

```bash
npm install
npm run firmware:flash
```

That builds, flashes, resets the board and prints the boot log. Networks come
from `firmware/.env`; once it has joined one it writes its address on the glass,
and the page there takes a claude.ai session token - which is kept on the device
and only ever sent back to the origin it came from.

Double-tap the glass to put the two percentages up, with a countdown to the next
reset under the face; double-tap again to take them away. Swipe up for the
charge, the network, the address and the commit it is running, and down again
to send it back. Swipe down from the face for brightness and volume - tap or
drag either slider, and it remembers - and up to put it away. Double-tap the cat and it takes the whole
glass; double-tap again to give it back.

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

`npm run firmware:flash` works out which one is plugged in and flashes that. It
has to: the two boards' pins overlap rather than merely differ, so the wrong
image drives the wrong silicon on every line and looks like dead hardware.

See [.agents/docs/hardware.md](.agents/docs/hardware.md) for the board -
including the things that each present as a dead board and none of which say so.

<img width="4032" height="3024" alt="IMG_2385" src="https://github.com/user-attachments/assets/a7326e64-50d3-4f40-8865-e39214a26606" />
