# claude-esp-oled

A round 360x360 display on an ESP32-S3, driven straight from the chip. A face
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
charge, the network, the address and the commit it is running, up again for a
shelf of bitmaps to go along with left and right, and down to come back.

Neither button is wired to anything the firmware can use. `PWR` switches the
power path and the chip cannot see it - it is the small one beside the USB-C,
pressed for a second to power on and held for three to power off. `BOOT` is the
strapping pin, and held down at reset it traps the board in the bootloader.

Hardware is a **Waveshare ESP32-S3-Touch-LCD-1.85B**. Despite the name of this
repository the panel is an IPS LCD rather than an OLED, which matters: black is
backlight leaking through liquid crystal rather than a pixel that is off.

See [.agents/docs/hardware.md](.agents/docs/hardware.md) for the board -
including the things that each present as a dead board and none of which say so.

<img width="4032" height="3024" alt="IMG_2385" src="https://github.com/user-attachments/assets/a7326e64-50d3-4f40-8865-e39214a26606" />
