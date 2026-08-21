# claude-esp-oled

A desk pet. A kawaii face that lives on a round 360x360 display on an ESP32-S3
and wanders around it - blinking, darting, hopping, and occasionally giggling.

It is not connected to anything. Everything it does is on the board.

```bash
npm install
npm run firmware:flash
```

Hardware is a **Waveshare ESP32-S3-Touch-LCD-1.85B**. Despite the name of this
repository the panel is an IPS LCD rather than an OLED, which matters: black is
backlight leaking through liquid crystal rather than a pixel that is off.

See [AGENTS.md](AGENTS.md) for the layout and
[.agents/docs/hardware.md](.agents/docs/hardware.md) for the board - including
the five things that each present as a dead board and none of which say so.
