#!/usr/bin/env bash
# Flash every board that is plugged in, each with its own image, then reset each
# and say what came back.
#
# The images are not interchangeable. The boards' pins overlap rather than
# merely differ - GPIO5 is one board's backlight and one of the other's four
# LCD data lines - so the wrong image does not fail, it drives the wrong
# silicon on every line and looks like dead hardware. Nothing about a board
# announces which it is, so this asks the flash chip: 16MB is the LCD board and
# 32MB is the AMOLED one. esptool reads that off the chip without running
# anything on it, which is the only question that can be asked of a board that
# may be holding the wrong firmware already.
#
#   ./firmware/flash.sh          flash whatever is plugged in - all of it
#   ./firmware/flash.sh <env>    flash only the board(s) that image belongs on
set -euo pipefail

PIO="$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")"
# Not `dirname "$PIO"`/python. The pio on PATH is often a shim standing on its
# own with no interpreter beside it, and what the boot log below needs is the
# python that has pyserial - the venv's, wherever pio itself was found.
PY="$HOME/.platformio/penv/bin/python"
[ -x "$PY" ] || PY="$(dirname "$PIO")/python"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

LCD_ENV="esp32-s3-touch-lcd-185"
AMOLED_ENV="esp32-s3-touch-amoled-175c"

want="${1:-}"

ports() { ls /dev | grep '^cu\.usbmodem' || true; }

# A node renames itself between resets, so "the board that was just flashed" is
# found again as whichever port the boards left alone do not account for.
findAgain() {
  if [ -n "$1" ]; then
    ports | grep -vxF "$1" | head -1
  else
    ports | head -1
  fi
}

boards="$(ports)"
if [ -z "$boards" ]; then
  echo "no board on USB - press PWR for 1s, and check it is not the hub" >&2
  exit 1
fi

flashed=0
for p in $boards; do
  others="$(ports | grep -vx "$p" || true)"

  size="$("$PY" -m esptool --port "/dev/$p" flash-id 2>/dev/null |
          sed -n 's/.*Detected flash size: *//p' | head -1 || true)"
  case "$size" in
    16MB) found="$LCD_ENV" ;;
    32MB) found="$AMOLED_ENV" ;;
    "")   echo "skipping /dev/$p: could not read its flash chip - is it awake?" >&2; continue ;;
    *)    echo "skipping /dev/$p: $size of flash is neither of the two this builds for" >&2
          continue ;;
  esac

  if [ -n "$want" ] && [ "$want" != "$found" ]; then
    echo "skipping /dev/$p: it has $size of flash, which is $found"
    continue
  fi

  echo "board on /dev/$p: $size of flash -> $found"
  "$PIO" run -d "$HERE" -e "$found" -t upload --upload-port "/dev/$p"
  flashed=1

  # esptool resets the chip itself after an upload, and that reset lands often
  # enough in the ROM bootloader that the sketch never runs and the panel stays
  # dark - which reads as a bad flash. A second, explicit reset costs a second
  # and takes the question away.
  sleep 1
  p="$(findAgain "$others")"
  "$PY" -m esptool --port "/dev/$p" --after hard-reset flash-id >/dev/null 2>&1 || true

  sleep 2
  p="$(findAgain "$others")"
  echo "--- boot log ($found) ---"
  "$PY" - "$p" <<'BOOTLOG'
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
    if any(k in line.lower() for k in ('panel', 'brightness', 'backlight', 'battery',
                                       'touch', 'audio', 'reset:', 'boot:', 'framebuffer',
                                       'build:', 'core dump')):
        print(' ', line)
BOOTLOG
done

if [ "$flashed" = 0 ]; then
  echo "nothing flashed" >&2
  exit 1
fi
