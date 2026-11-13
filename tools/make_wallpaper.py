#!/usr/bin/env python3
"""
make_wallpaper.py -- write a desktop wallpaper BMP for Quilon (ROADMAP3 18.5).

Usage:
    python3 tools/make_wallpaper.py OUT.bmp            the demo scene (default)
    python3 tools/make_wallpaper.py OUT.bmp --tile     a 64x64 tiling weave

The demo is a 256x160 (16:10) dusk scene -- layered mountains over a lake
under a sunset sky.  The WM scales it (bilinear) to fill the screen; it is
kept small because the FAT16 disk.img is only 1 MB.  Images of at most
128x128 are tiled instead, which is what --tile is for.

`make disk` / `make ext2disk` put this demo on the disk as /desktop.bmp.
To use your own picture: `make disk WALLPAPER=photo.bmp` (any 24/32-bit
uncompressed BMP); for none: `make disk WALLPAPER=none`.

Pure Python, deterministic (same bytes every run), no dependencies.
"""

import math, struct, sys


def write_bmp(path, w, h, pixel):
    """pixel(x, y) -> (r, g, b), y = 0 at the top."""
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(h - 1, -1, -1):           # BMP rows are bottom-up
        row = bytearray()
        for x in range(w):
            r, g, b = pixel(x, y)
            row += bytes((clamp(b), clamp(g), clamp(r)))
        row += b'\0' * (stride - w * 3)
        rows.append(bytes(row))
    data = b''.join(rows)
    hdr = struct.pack('<2sIHHI', b'BM', 54 + len(data), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(data),
                       2835, 2835, 0, 0)
    with open(path, 'wb') as f:
        f.write(hdr + info + data)
    return 54 + len(data)


def clamp(v):
    return max(0, min(255, int(round(v))))


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def smooth(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)


# ── The demo scene ──────────────────────────────────────────────────────────

W, H = 256, 160
HORIZON = 104                       # lake surface (y)
SUN = (150.0, 74.0, 11.0)           # x, y, radius

SKY = [                             # (y fraction of the horizon, colour)
    (0.00, (16, 20, 52)),           # night indigo
    (0.40, (58, 48, 112)),          # violet
    (0.72, (168, 88, 128)),         # rose
    (0.90, (238, 140, 104)),        # peach
    (1.00, (252, 196, 132)),        # gold at the horizon
]

# Mountain ranges, back to front: base height above the horizon, colour,
# and (amplitude, wavelength, phase) terms.
RANGES = [
    (22, (122, 86, 138), [(9, 71, 1.9), (4, 29, 1.7), (1, 11, 0.4)]),
    (13, (74, 52, 104),  [(10, 89, 3.4), (5, 37, 0.9), (1.2, 13, 2.6)]),
    (4,  (34, 26, 62),   [(8, 107, 1.0), (4, 41, 3.1), (1, 17, 1.2)]),
]


def sky(y):
    t = y / HORIZON
    for (t0, c0), (t1, c1) in zip(SKY, SKY[1:]):
        if t <= t1:
            return mix(c0, c1, smooth((t - t0) / (t1 - t0)))
    return SKY[-1][1]


def sun_glow(x, y, c):
    sx, sy, r = SUN
    d = math.hypot(x - sx, (y - sy) * 1.15)
    glow = mix(c, (255, 200, 150), 0.55 * math.exp(-max(0.0, d - r) / 16.0))
    if d < r + 1:                                 # the disc, softly edged
        return mix(glow, (255, 236, 186), smooth((r + 1 - d) / 2.0))
    return glow


def ridge(rng, x):
    base, _, terms = rng
    return HORIZON - base - sum(a * math.sin(x * 2 * math.pi / wl + ph)
                                for a, wl, ph in terms)


def above_water(x, y):
    """Colour of the scene above the lake at (x, y), y < HORIZON."""
    c = sun_glow(x, y, sky(y))
    # A few stars high up, fading towards the dusk.
    h = (int(x) * 2654435761 + int(y) * 40503 * 97) & 0xFFFFFFFF
    h = ((h ^ (h >> 15)) * 2246822519) & 0xFFFFFFFF
    h ^= h >> 13
    if y < HORIZON * 0.5 and h % 1000 < 6:
        c = mix(c, (255, 250, 235), 0.9 - 1.6 * y / HORIZON)
    for i, rng in enumerate(RANGES):
        top = ridge(rng, x)
        cover = y - top + 0.5                     # anti-aliased edge
        if cover > 0:
            col = rng[1]
            # Haze: lighter toward the base of each range, more for far ones.
            haze = 0.35 * (1 - i / len(RANGES)) * smooth((y - top) / 30.0)
            c = mix(c, mix(col, (200, 140, 150), haze), cover)
    return c


def scene(x, y):
    if y < HORIZON:
        return above_water(x, y)
    # The lake: the mirrored scene, darkened, broken by gentle ripples.
    depth = (y - HORIZON) / (H - HORIZON)
    wobble = 1.6 * math.sin(y * 1.9 + x * 0.05) * (0.4 + depth)
    my = HORIZON - 1 - (y - HORIZON) * 1.4
    c = above_water(x + wobble, max(0.0, my))
    c = mix(c, (14, 16, 40), 0.30 + 0.45 * depth)
    # The sun's glitter path on the water.
    path = math.exp(-((x - SUN[0]) / (6 + 22 * depth)) ** 2)
    rip = 0.5 + 0.5 * math.sin(y * 3.1 + math.sin(x * 0.7) * 2)
    return mix(c, (255, 206, 150), 0.55 * path * rip * (1 - 0.6 * depth))


# ── A small tile (the 18.5 sample) ──────────────────────────────────────────

def weave(x, y, n=64):
    base = (96, 138, 175)                         # WM_DESKTOP_COL
    a = ((x + y) * 4 // n) % 2
    b = ((x - y) * 4 // n) % 2
    d = 10 if a and b else (-8 if not a and not b else 0)
    return tuple(c + d for c in base)


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith('-') or len(args) > 2:
        print("usage: make_wallpaper.py OUT.bmp [--tile]", file=sys.stderr)
        sys.exit(1)
    out = args[0]
    if '--tile' in args[1:]:
        size = write_bmp(out, 64, 64, weave)
        print(f"wrote {out}: 64x64 tile, {size} bytes")
    else:
        size = write_bmp(out, W, H, scene)
        print(f"wrote {out}: {W}x{H} demo scene, {size} bytes")


if __name__ == '__main__':
    main()
