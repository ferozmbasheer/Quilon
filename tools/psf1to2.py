#!/usr/bin/env python3
"""Convert a PSF1 font to PSF2.

Usage: psf1to2.py INPUT.psf OUTPUT.psf
"""
import struct, sys

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} INPUT.psf OUTPUT.psf", file=sys.stderr)
        sys.exit(1)

    d = open(sys.argv[1], 'rb').read()
    if d[0:2] != bytes([0x36, 0x04]):
        print(f"Error: not a PSF1 file (magic={d[0:2].hex()})", file=sys.stderr)
        sys.exit(1)

    mode     = d[2]
    charsize = d[3]
    n_glyphs = 512 if (mode & 0x01) else 256

    glyph_data = d[4 : 4 + n_glyphs * charsize]

    hdr = struct.pack('<8I',
        0x864AB572,  # PSF2 magic
        0,           # version
        32,          # header_size
        0,           # flags
        n_glyphs,    # glyph_count
        charsize,    # bytes_per_glyph
        charsize,    # height (PSF1 width is always 8, height == charsize)
        8,           # width
    )

    with open(sys.argv[2], 'wb') as f:
        f.write(hdr)
        f.write(glyph_data)

    print(f"converted: 8x{charsize}  {n_glyphs} glyphs -> {sys.argv[2]}")

if __name__ == '__main__':
    main()
