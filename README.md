# claude-esp-oled

A round 360x360 display on an ESP32-S3, driven straight from the chip. Nine
half-opacity balls bounce off the inside of the glass over a grid, and where two
cross, both colours are still there.

```bash
npm install
npm run firmware:flash
```

That builds, flashes, resets the board and prints the boot log. A short press of
`BOOT` - the small button beside the USB-C - blanks the display and another
brings it back. `PWR` is not wired to the chip and firmware cannot touch it.

Hardware is a **Waveshare ESP32-S3-Touch-LCD-1.85B**. Despite the name of this
repository the panel is an IPS LCD rather than an OLED, which matters: black is
backlight leaking through liquid crystal rather than a pixel that is off.

See [AGENTS.md](AGENTS.md) for the layout and
[.agents/docs/hardware.md](.agents/docs/hardware.md) for the board - including
the things that each present as a dead board and none of which say so.

<img width="4032" height="3024" alt="IMG_2385" src="https://github.com/user-attachments/assets/a7326e64-50d3-4f40-8865-e39214a26606" />
