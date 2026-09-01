"""Turns assets/nyan.gif into a header of indexed sprite frames.

Generated into the build directory rather than into src, for the same reason
version.py writes where it does: building a tree should not dirty it. The GIF
stays the asset and this stays the tool - nothing here knows what it is looking
at beyond "the big shape is the subject, the small ones are not, and the flat
end of it repeats".

A checkout without the asset still builds; it just has no cat.

Everything is done by hand rather than with Pillow. This runs before every
build, and a build that needs a pip install first is a build that fails on a
fresh machine.
"""

import math
import os
from collections import Counter, deque

Import("env")  # noqa: F821  - injected by PlatformIO

HERE = env.subst("$PROJECT_DIR")  # noqa: F821
ASSET = os.path.join(HERE, "assets", "nyan.gif")

# How far off the background colour a pixel has to be to count as the subject.
# The GIF has been resaved enough times to carry nine hundred colours, most of
# them near-identical blues, so nothing here can key on an exact value. Sixty
# sits in the middle of a plateau - 45 and 60 classify the same pixels - which
# is what says the threshold is nowhere near anything that matters.
KEY = 60
# The subject is one connected blob of about ten thousand pixels and the stars
# are islands of a hundred and thirty or fewer. Anything under this is sky.
BLOB = 400
# How many colours the sprite is allowed. The GIF carries seven hundred, nearly
# all of them dither: the art underneath is a couple of dozen flat colours and
# the rest is the noise of it having been resaved. Snapping to the commonest few
# and taking the nearest for everything else puts the flats back, and it is the
# difference between pixel art and a photograph of some.
PALETTE = 28
# How many rows of cells the art becomes. A whole number of source pixels to a
# cell would be the obvious way to shrink it, but the sizes that lands on are
# coarse - three to one and four to one are a quarter apart with nothing between
# - so the grid is resampled to a row count instead, and the panel does the only
# whole-number step, on the way out.
TALL = 21
# Source pixels of trail to add on the left. The GIF frames it tight, at about
# three quarters of a cat's width of rainbow, where the real thing runs it
# several times the cat and lets it leave the frame.
#
# Added in source pixels rather than in cells so the tiling happens before the
# resampling. The repeat is a whole number of source columns and lands on
# fractional cell boundaries; tiled after the fact it smears into something with
# no period at all, which is what the measurement says when it is tried.
TRAIL_PX = 240
# Columns of sprite over which the far end of the trail closes to a point. Cut
# square it reads as a rainbow somebody sawed the end off; drawn back to nothing
# it reads as something the cat is leaving behind. Narrow at the tip and
# widening fast, which is the shape of a flame rather than of a wedge - and it
# wanders frame to frame, or a tapered end is just a pointed one.
FLAME = 26


def _blocks(data, i):
    out = bytearray()
    while True:
        n = data[i]
        i += 1
        if n == 0:
            return bytes(out), i
        out += data[i:i + n]
        i += n


def _lzw(data, least):
    clear = 1 << least
    end = clear + 1
    size = least + 1
    table = [(i,) for i in range(clear)] + [(), ()]
    out = []
    prev = None
    acc = pos = 0
    for byte in data:
        acc |= byte << pos
        pos += 8
        while pos >= size:
            code = acc & ((1 << size) - 1)
            acc >>= size
            pos -= size
            if code == clear:
                table = [(i,) for i in range(clear)] + [(), ()]
                size = least + 1
                prev = None
                continue
            if code == end:
                return out
            if code < len(table) and table[code]:
                entry = table[code]
            elif prev is not None:
                entry = prev + (prev[0],)
            else:
                return out
            out.extend(entry)
            if prev is not None:
                table.append(prev + (entry[0],))
                if len(table) == (1 << size) and size < 12:
                    size += 1
            prev = entry
    return out


def read_gif(path):
    """Composited RGB frames, and how long each is meant to be up."""
    data = open(path, "rb").read()
    if data[:6] not in (b"GIF87a", b"GIF89a"):
        raise ValueError("not a gif")
    w = data[6] | (data[7] << 8)
    h = data[8] | (data[9] << 8)
    packed = data[10]
    i = 13
    gct = None
    if packed & 0x80:
        n = 2 << (packed & 7)
        gct = [tuple(data[i + k * 3:i + k * 3 + 3]) for k in range(n)]
        i += n * 3

    canvas = [(0, 0, 0)] * (w * h)
    frames = []
    delay = 100
    transparent = None
    disposal = 0

    while i < len(data):
        b = data[i]
        if b == 0x3B:
            break
        if b == 0x21:
            label = data[i + 1]
            i += 2
            if label == 0xF9:
                n = data[i]
                blk = data[i + 1:i + 1 + n]
                disposal = (blk[0] >> 2) & 7
                delay = (blk[1] | (blk[2] << 8)) * 10 or 100
                transparent = blk[3] if blk[0] & 1 else None
                i += 1 + n
                while data[i] != 0:
                    i += data[i] + 1
                i += 1
            else:
                _, i = _blocks(data, i)
            continue
        if b != 0x2C:
            i += 1
            continue

        fx = data[i + 1] | (data[i + 2] << 8)
        fy = data[i + 3] | (data[i + 4] << 8)
        fw = data[i + 5] | (data[i + 6] << 8)
        fh = data[i + 7] | (data[i + 8] << 8)
        flags = data[i + 9]
        i += 10
        table = gct
        if flags & 0x80:
            n = 2 << (flags & 7)
            table = [tuple(data[i + k * 3:i + k * 3 + 3]) for k in range(n)]
            i += n * 3
        least = data[i]
        i += 1
        raw, i = _blocks(data, i)
        idx = _lzw(raw, least)

        rows = list(range(fh))
        if flags & 0x40:
            rows = (list(range(0, fh, 8)) + list(range(4, fh, 8)) +
                    list(range(2, fh, 4)) + list(range(1, fh, 2)))

        keep = list(canvas)
        for n, row in enumerate(rows):
            for col in range(fw):
                p = n * fw + col
                if p >= len(idx):
                    break
                v = idx[p]
                if transparent is not None and v == transparent:
                    continue
                x, y = fx + col, fy + row
                if 0 <= x < w and 0 <= y < h:
                    canvas[y * w + x] = table[v]
        frames.append((list(canvas), delay))
        if disposal == 2:
            for row in range(fy, min(fy + fh, h)):
                for col in range(fx, min(fx + fw, w)):
                    canvas[row * w + col] = (0, 0, 0)
        elif disposal == 3:
            canvas = keep
    return w, h, frames


def subject(px, w, h, sky):
    """True where the pixel belongs to the big shape rather than to the sky."""
    off = [(p[0] - sky[0]) ** 2 + (p[1] - sky[1]) ** 2 + (p[2] - sky[2]) ** 2
           > KEY * KEY for p in px]
    keep = [False] * (w * h)
    seen = [False] * (w * h)
    for start in range(w * h):
        if not off[start] or seen[start]:
            continue
        q = deque([start])
        seen[start] = True
        found = []
        while q:
            i = q.popleft()
            found.append(i)
            x, y = i % w, i // w
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h:
                    j = ny * w + nx
                    if off[j] and not seen[j]:
                        seen[j] = True
                        q.append(j)
        # The stars are the small ones, and the sky is what was asked to go.
        if len(found) >= BLOB:
            for i in found:
                keep[i] = True
    return keep


def trail_of(masks, w, h, x0, x1, y0, y1):
    """How far the flat trail runs from the left, and the rows it sits in.

    The trail has a flat bottom all the way along and the cat's legs hang below
    it, so the run ends where the bottom edge first drops past the common one.
    The top edge would have done it too were it not for the sparkles, which sit
    above the trail and belong to neither - measured from underneath, they cost
    nothing.
    """
    extent = []
    for x in range(x0, x1 + 1):
        lo, hi = h, -1
        for m in masks:
            for y in range(y0, y1 + 1):
                if m[y * w + x]:
                    lo, hi = min(lo, y), max(hi, y)
        extent.append((lo, hi))
    # Off the leftmost columns, which are trail and nothing else - the rainbow
    # runs to the edge of the frame. Taken over the whole width instead, the cat
    # wins the vote: it is the taller thing and it is also the wider one, so the
    # commonest bottom edge in the picture is its feet rather than the trail.
    edge = [(lo, hi) for lo, hi in extent[:10] if hi >= 0]
    floor = Counter(hi for _, hi in edge).most_common(1)[0][0]
    roof = Counter(lo for lo, _ in edge).most_common(1)[0][0]
    run = len(extent)
    for i, (_, hi) in enumerate(extent):
        if hi > floor:
            run = i
            break
    return run, roof, floor


def period_of(frames, masks, w, snap, x0, run, roof, floor):
    """The width the trail repeats over, in source columns.

    Only inside the band, because the sparkles above it drift on a schedule of
    their own and nothing looking for a repeat should be shown them. And only at
    eight columns and up: a rainbow is stripes, so every small shift scores well
    without meaning anything, and the tail of that runs straight through the
    answer.
    """
    best = (0.0, 0)
    for p in range(8, max(run - 8, 9)):
        same = total = 0
        for (px, _), m in zip(frames, masks):
            for x in range(x0, x0 + run - p):
                for y in range(roof, floor + 1):
                    a = snap(px[y * w + x]) if m[y * w + x] else None
                    b = snap(px[y * w + x + p]) if m[y * w + x + p] else None
                    total += 1
                    same += a == b
        score = same / total if total else 0.0
        if score > best[0]:
            best = (score, p)
    return best


def build():
    w, h, frames = read_gif(ASSET)
    sky = Counter(p for px, _ in frames for p in px).most_common(1)[0][0]
    masks = [subject(px, w, h, sky) for px, _ in frames]

    # One box for the whole loop, or the cat bobs against its own frame instead
    # of against the panel.
    x0, y0, x1, y1 = w, h, -1, -1
    for mask in masks:
        for i, on in enumerate(mask):
            if on:
                x, y = i % w, i // w
                x0, x1 = min(x0, x), max(x1, x)
                y0, y1 = min(y0, y), max(y1, y)
    if x1 < 0:
        raise ValueError("nothing left after the sky came out")

    # The flats, found on the subject rather than on the whole picture - most of
    # which was sky and is gone. Snapped here rather than after the resampling,
    # so the repeat below is looked for in flat colour instead of in dither.
    used = Counter()
    for (px, _), m in zip(frames, masks):
        for i, on in enumerate(m):
            if on:
                used[px[i]] += 1
    flats = [c for c, _ in used.most_common(PALETTE)]
    near = {}

    def snap(c):
        if c not in near:
            near[c] = min(flats, key=lambda f: (f[0] - c[0]) ** 2 +
                          (f[1] - c[1]) ** 2 + (f[2] - c[2]) ** 2)
        return near[c]

    run, roof, floor = trail_of(masks, w, h, x0, x1, y0, y1)
    score, period = period_of(frames, masks, w, snap, x0, run, roof, floor)
    print("nyan.py: {} columns of trail in rows {}..{}, repeating every {}"
          " ({:.0f}% agree)".format(run, roof, floor, period, score * 100))

    grow = TRAIL_PX if period else 0
    left = x0 - grow

    def source(x):
        # Inside the art it is itself; out to the left it is the trail's own
        # repeat, taken from the frame being drawn - so whatever the trail does
        # between frames goes on doing it all the way out.
        if x >= x0:
            return x
        return x0 + (x - x0) % period

    # Square cells: the row count sets how many source pixels one is worth and
    # the column count follows from it rather than being chosen, or the cat comes
    # out stretched.
    step = (y1 - y0 + 1) / float(TALL)
    sh = TALL
    sw = int(round((x1 - left + 1) / step))
    palette = []
    index = {}
    out = []
    for (px, _), mask in zip(frames, masks):
        cells = []
        for sy in range(sh):
            ry0 = y0 + int(sy * step)
            ry1 = min(y0 + int((sy + 1) * step), y1 + 1)
            for sx in range(sw):
                rx0 = left + int(sx * step)
                rx1 = min(left + int((sx + 1) * step), x1 + 1)
                votes = Counter()
                total = 0
                for y in range(ry0, max(ry1, ry0 + 1)):
                    for x in range(rx0, max(rx1, rx0 + 1)):
                        if x > x1 or y > y1:
                            continue
                        at = source(x)
                        on = mask[y * w + at]
                        # The sparkles sit above and below the trail and twinkle
                        # on a schedule of their own, which reads as specks
                        # flickering off the rainbow rather than as part of it.
                        # The band was measured, so anything outside it out here
                        # is one of them - and the cat, which legitimately hangs
                        # past the band, is off to the right of this.
                        if on and at < x0 + run and not roof <= y <= floor:
                            on = False
                        total += 1
                        if on:
                            votes[snap(px[y * w + at])] += 1
                # A cell the subject only clips the corner of is sky. Taking the
                # commonest colour rather than an average is what keeps the edges
                # hard: averaged, every pixel round the outline comes out a blend
                # with a background that is not there any more.
                if not votes or sum(votes.values()) * 2 < total:
                    cells.append(0)
                    continue
                rgb = votes.most_common(1)[0][0]
                if rgb not in index:
                    index[rgb] = len(palette) + 1
                    palette.append(rgb)
                cells.append(index[rgb])
        out.append(cells)
    flame(sw, sh, out)
    return sw, sh, palette, out, [d for _, d in frames]


def _noise(a, b, c):
    """A repeatable nought-to-one from three whole numbers.

    Repeatable matters: the same cell of the same frame has to break off the
    same way every build, or the loop flickers differently each time it is
    compiled and the tail stops being a shape at all.
    """
    n = (a * 73856093) ^ (b * 19349663) ^ (c * 83492791)
    n = (n * 1103515245 + 12345) & 0x7FFFFFFF
    return (n % 10000) / 10000.0


def flame(sw, sh, out):
    """Breaks the far end of the trail up instead of cutting it off.

    A taper on its own comes out a triangle - the edges are still straight, they
    just meet. What reads as burning off is losing cells rather than losing
    height: the further out a cell is the less likely it is to survive, so the
    trail goes solid, then ragged, then a scatter of bits still flying along
    behind, then nothing. The odds are lower at the edges than up the middle,
    which is what keeps a core to it.

    The outline is the half of this the eye reads first, so it is curved and it
    is rolled again every frame. Straight and standing still it comes out a
    triangle however finely the inside of it is eaten away, which is what this
    was: the holes moved and the two edges holding them never did.
    """
    if FLAME <= 0 or FLAME >= sw:
        return
    for f, cells in enumerate(out):
        for j in range(FLAME):
            rows = [y for y in range(sh) if cells[y * sw + j]]
            if not rows:
                continue
            mid = (rows[0] + rows[-1]) / 2.0
            full = max((rows[-1] - rows[0]) / 2.0, 1.0)
            t = (j + 0.5) / FLAME
            # Height comes back along a curve rather than up a slope, so the tip
            # is a point and the shoulders behind it are round - a straight rise
            # is a wedge, and a wedge is what a triangle is.
            #
            # A flame also has tongues, and a tongue is a bulge in the outline
            # that travels along it. One slow wave down the columns carried a
            # whole turn over the loop is that - a whole turn, or frame seven
            # meets frame nought with a step in it - and the wander on top is
            # what stops the wave reading as a wave. Both die away towards the
            # solid trail, which has no business rippling.
            wave = math.sin(j * 0.55 + 2.0 * math.pi * f / len(out))
            tongue = 1.0 + (1.0 - t) * (0.42 * wave + 0.34 * (_noise(f, j, 7) - 0.5))
            half = full * min(1.0, 1.5 * (t ** 0.62) * tongue)
            for y in range(sh):
                if not cells[y * sw + j]:
                    continue
                off = abs(y - mid)
                if off > half:
                    # Past the outline and still alight: the bits that have
                    # broken off it. Their odds come off a grid twice as coarse
                    # as the cells are, so these arrive in flecks of two and
                    # three - one cell on its own is dust, and dust does not
                    # read as fire.
                    spark = 0.4 * t * max(0.0, 1.0 - (off - half) / full)
                    if _noise(f, j // 2, y // 2) > spark:
                        cells[y * sw + j] = 0
                    continue
                live = (t ** 0.45) * (1.0 - 0.5 * (off / full))
                if _noise(f, j, y) > live:
                    cells[y * sw + j] = 0


def rgb565(c):
    return ((c[0] & 0xF8) << 8) | ((c[1] & 0xFC) << 3) | (c[2] >> 3)


head = ["// Generated by nyan.py from assets/nyan.gif. Do not edit.", "#pragma once", "",
        "#include <stdint.h>", ""]
try:
    SW, SH, PAL, CELLS, DELAYS = build()
except (OSError, ValueError, IndexError) as exc:
    body = "\n".join(head + [
        "// " + str(exc),
        "#define NYAN_FRAMES 0",
        "#define NYAN_W 0",
        "#define NYAN_H 0",
        "static const uint16_t NYAN_PALETTE[1] = {0};",
        "static const uint8_t NYAN_CELLS[1][1] = {{0}};",
        "static const uint16_t NYAN_DELAY_MS[1] = {100};",
        "",
    ])
    print("nyan.py: no sprite ({})".format(exc))
else:
    lines = head + [
        "#define NYAN_FRAMES {}".format(len(CELLS)),
        "#define NYAN_W {}".format(SW),
        "#define NYAN_H {}".format(SH),
        "",
        "// Nought is nothing there. Every other index is a colour below, already",
        "// in the panel's byte order so a sprite pixel is a store rather than a",
        "// swap and a store.",
        "static const uint16_t NYAN_PALETTE[{}] = {{".format(len(PAL) + 1),
        "    0x0000,",
    ]
    for i in range(0, len(PAL), 8):
        row = ", ".join("0x{:04X}".format(((rgb565(c) >> 8) | (rgb565(c) << 8)) & 0xFFFF)
                        for c in PAL[i:i + 8])
        lines.append("    {},".format(row))
    lines += ["};", ""]
    lines.append("static const uint8_t NYAN_CELLS[NYAN_FRAMES][NYAN_W * NYAN_H] = {")
    for cells in CELLS:
        lines.append("    {")
        for i in range(0, len(cells), 24):
            lines.append("        " + ", ".join(str(v) for v in cells[i:i + 24]) + ",")
        lines.append("    },")
    lines += ["};", ""]
    lines.append("static const uint16_t NYAN_DELAY_MS[NYAN_FRAMES] = {{{}}};".format(
        ", ".join(str(d) for d in DELAYS)))
    lines.append("")
    body = "\n".join(lines)
    print("nyan.py: {}x{}, {} frames, {} colours".format(SW, SH, len(CELLS), len(PAL)))

out_dir = os.path.join(env.subst("$BUILD_DIR"), "generated")  # noqa: F821
os.makedirs(out_dir, exist_ok=True)
path = os.path.join(out_dir, "nyan.h")
if not os.path.exists(path) or open(path, encoding="utf-8").read() != body:
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(body)
env.Append(CPPPATH=[out_dir])  # noqa: F821
