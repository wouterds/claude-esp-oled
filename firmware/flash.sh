#!/usr/bin/env bash
# Flash, then reset, then say what came back.
#
# esptool resets the chip itself after an upload, and on this board that reset
# lands often enough in the ROM bootloader that the sketch never runs and the
# panel stays dark - which reads as a bad flash. A second, explicit reset costs
# a second and takes the question away.
set -euo pipefail

PIO="$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")"
PY="$(dirname "$PIO")/python"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

port() { ls /dev | grep '^cu\.usbmodem' | head -1; }

# The node renames itself between resets, so it is looked up every time rather
# than once.
p="$(port)"
if [ -z "$p" ]; then
  echo "no board on USB - press PWR for 1s, and check it is not the hub" >&2
  exit 1
fi

"$PIO" run -d "$HERE" -t upload --upload-port "/dev/$p"

sleep 1
p="$(port)"
"$PY" -m esptool --port "/dev/$p" --after hard-reset flash-id >/dev/null 2>&1 || true

sleep 2
p="$(port)"
echo "--- boot log ---"
"$PY" - "$p" <<'PYEOF'
import serial, sys, time
s = serial.Serial()
s.port = '/dev/' + sys.argv[1]
s.baudrate = 115200
s.timeout = 0.5
# Opening the port with either line asserted resets the chip into the ROM
# bootloader, so both are cleared before it is opened.
s.dtr = False
s.rts = False
s.open()
end = time.time() + 6
buf = ''
while time.time() < end:
    d = s.read(4096)
    if d:
        buf += d.decode(errors='replace')
s.close()
for line in buf.splitlines():
    if any(k in line.lower() for k in ('panel', 'backlight', 'display', 'boot:', 'framebuffer')):
        print(' ', line)
PYEOF
