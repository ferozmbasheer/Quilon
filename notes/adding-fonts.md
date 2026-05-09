# Adding Fonts

Any PSF2 font file works with Quilon's font loader.

## Steps

1. **Get a `.psf` file**
   - `/usr/share/consolefonts/` on Debian/Ubuntu
   - `terminus-font` package has clean bitmap fonts at multiple sizes
   - `kbd` package fonts

2. **Copy it to `tools/`**
   ```sh
   cp /usr/share/consolefonts/Uni2-TerminusBold16.psf.gz tools/
   gunzip tools/Uni2-TerminusBold16.psf.gz
   ```

3. **Update `iso.sh`** — change the `FONT.PSF=` argument
   ```sh
   python3 tools/make_initrd.py isodir/boot/initrd.img \
       FONT.PSF=tools/Uni2-TerminusBold16.psf
   ```

The kernel always loads whatever file is named `FONT.PSF` in the initrd.
Dimensions are picked up automatically — `vbe_terminal_init()` recomputes
`term_cols`/`term_rows` from the font's width/height.

## Caveat

If the new font has larger glyphs, confirm the GRUB resolution is big enough.
At 800×600: an 8×16 font → 100×37 chars; a 16×32 font → 50×18 chars.

## Validate a font before using it

```sh
python3 -c "
import struct, sys
d = open(sys.argv[1],'rb').read()
magic = struct.unpack_from('<I',d,0)[0]
print('magic ok' if magic==0x864AB572 else f'BAD MAGIC {magic:#x}')
print('width=%d height=%d glyphs=%d bpg=%d' % struct.unpack_from('<4I',d,24))
" tools/yourfont.psf
```

## PSF1 fonts (most Terminus console fonts)

The Terminus fonts in `/usr/share/consolefonts/` are PSF1, not PSF2.
Convert first:
```sh
python3 tools/psf1to2.py tools/Lat15-TerminusBold16.psf tools/myterm.psf
```
Then use `myterm.psf` as the `FONT.PSF=` argument in `iso.sh`.

## Currently bundled

- `tools/Tamsyn8x16r.psf` — 8×16, 256 glyphs, regular weight (original)
- `tools/Uni2-TerminusBold16-psf2.psf` — 8×16, 512 glyphs, Terminus Bold (active)
