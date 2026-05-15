/*
 * Quilon OS -- PSF2 Bitmap Font Loader  (section 11.2)
 *
 * PC Screen Font 2 (PSF2) is the format used by the Linux console.
 * This header provides:
 *
 *   psf2_header_t   -- the 32-byte on-disk header (packed struct)
 *   psf2_font_t     -- runtime font state (pointer + geometry)
 *   psf2_parse()    -- pure-C header validator; fills psf2_font_t (host testable)
 *   psf2_glyph_pixel() -- pure-C pixel accessor (host testable)
 *
 * Kernel-only functions (declared here, defined in psf.c):
 *   psf2_load()          -- validate + register as the active font
 *   psf2_get_font()      -- return the active font (NULL if none)
 *   psf2_draw_glyph()    -- render one glyph via vbe_draw_pixel
 *   psf2_make_from_builtin() -- build a PSF2 image from the built-in 8×8 data
 *
 * Format notes
 * ------------
 * A PSF2 file is:  [psf2_header_t]  [glyph bitmaps]  [unicode table (optional)]
 *
 * Each glyph occupies exactly bytes_per_glyph bytes.  Within the glyph, each
 * pixel row is ceil(width/8) bytes wide, MSB first (bit 7 = leftmost pixel).
 * Glyph for codepoint c starts at offset c * bytes_per_glyph in the glyph data.
 */

#ifndef _KERNEL_PSF_H
#define _KERNEL_PSF_H

#include <stdint.h>

/* -- PSF2 on-disk header -------------------------------------------------- */

#define PSF2_MAGIC 0x864AB572u

typedef struct {
    uint32_t magic;           /* must be PSF2_MAGIC                         */
    uint32_t version;         /* must be 0                                  */
    uint32_t header_size;     /* byte offset to first glyph (usually 32)   */
    uint32_t flags;           /* 0 = no unicode table, 1 = has unicode table */
    uint32_t glyph_count;     /* number of glyphs in the file               */
    uint32_t bytes_per_glyph; /* bytes per glyph (≥ ceil(width/8)*height)   */
    uint32_t height;          /* glyph height in pixels                     */
    uint32_t width;           /* glyph width in pixels                      */
} __attribute__((packed)) psf2_header_t;

/* -- Runtime font state --------------------------------------------------- */

typedef struct {
    const uint8_t *glyphs;        /* pointer to glyph bitmap data (not owned) */
    uint32_t       width;         /* glyph width in pixels                    */
    uint32_t       height;        /* glyph height in pixels                   */
    uint32_t       bytes_per_glyph;
    uint32_t       glyph_count;
} psf2_font_t;

/* -- psf2_parse -- pure C, host testable ---------------------------------- */
/*
 * Parse and validate a PSF2 image.  On success, fills *out with pointers and
 * geometry derived from the header; *out->glyphs points directly into data
 * (no copy is made).  The caller must keep data alive as long as the font is
 * in use.
 *
 * Returns  0 on success.
 * Returns -1 if the image is too short, the magic is wrong, or geometry fields
 *           are inconsistent.
 */
static inline int psf2_parse(const uint8_t *data, uint32_t len,
                              psf2_font_t *out)
{
    if (!data || !out || len < (uint32_t)sizeof(psf2_header_t))
        return -1;

    const psf2_header_t *hdr = (const psf2_header_t *)data;

    if (hdr->magic != PSF2_MAGIC)                               return -1;
    if (hdr->version != 0)                                      return -1;
    if (hdr->width == 0 || hdr->height == 0)                    return -1;
    if (hdr->glyph_count == 0 || hdr->bytes_per_glyph == 0)    return -1;
    if (hdr->header_size < (uint32_t)sizeof(psf2_header_t))     return -1;
    if (hdr->header_size > len)                                 return -1;

    /* bytes_per_glyph must be at least ceil(width/8)*height */
    uint32_t bytes_per_row = (hdr->width + 7u) / 8u;
    if (hdr->bytes_per_glyph < bytes_per_row * hdr->height)     return -1;

    /* glyph data must fit within the image */
    uint32_t glyph_size = hdr->glyph_count * hdr->bytes_per_glyph;
    if (hdr->header_size + glyph_size > len)                    return -1;

    out->glyphs          = data + hdr->header_size;
    out->width           = hdr->width;
    out->height          = hdr->height;
    out->bytes_per_glyph = hdr->bytes_per_glyph;
    out->glyph_count     = hdr->glyph_count;
    return 0;
}

/* -- psf2_glyph_pixel -- pure C, host testable ---------------------------- */
/*
 * Return 1 if pixel (col, row) is lit in glyph ch, 0 otherwise.
 *
 * col  : 0 = leftmost pixel, width-1 = rightmost
 * row  : 0 = topmost, height-1 = bottommost
 * ch   : Unicode codepoint (clamped to glyph_count by caller or here)
 *
 * Out-of-bounds col, row, or ch -> 0.
 */
static inline int psf2_glyph_pixel(const psf2_font_t *font,
                                    uint32_t ch, uint32_t col, uint32_t row)
{
    if (!font || !font->glyphs)                  return 0;
    if (ch  >= font->glyph_count)                return 0;
    if (col >= font->width || row >= font->height) return 0;

    const uint8_t *glyph = font->glyphs + ch * font->bytes_per_glyph;
    uint32_t bytes_per_row = (font->width + 7u) / 8u;
    uint32_t byte_idx = row * bytes_per_row + col / 8u;
    uint32_t bit_idx  = 7u - (col % 8u);  /* MSB = leftmost pixel */
    return (int)((glyph[byte_idx] >> bit_idx) & 1u);
}

/* -- Kernel-side API (implemented in psf.c) ------------------------------- */

/*
 * psf2_load -- validate and register a PSF2 image as the active font.
 *
 * data must remain valid for the kernel's lifetime (e.g. a buffer in initrd
 * memory, a .bss static array, or a kmalloc'd region that is never freed).
 *
 * Returns  0 on success; psf2_get_font() will return non-NULL afterwards.
 * Returns -1 if the image is invalid.
 *
 * After a successful load, call vbe_terminal_init() to recompute the terminal
 * grid if the new font has different dimensions from the current font.
 */
int psf2_load(const uint8_t *data, uint32_t len);

/* Returns a pointer to the active font, or NULL if psf2_load has not been
 * called successfully. */
const psf2_font_t *psf2_get_font(void);

/*
 * psf2_draw_glyph -- render glyph ch at pixel (x, y) using the active font.
 *
 * Calls vbe_draw_pixel() for each pixel.  Must only be called after a
 * successful vbe_init() and psf2_load().
 */
void psf2_draw_glyph(uint32_t ch, uint32_t x, uint32_t y,
                     uint32_t fg, uint32_t bg);

/*
 * psf2_make_from_builtin -- build a valid PSF2 image from the built-in 8×8
 * bitmaps, padded to 8×16 cell height (rows 0–7: data; rows 8–15: blank).
 *
 * Writes up to buf_len bytes into buf.  Returns the number of bytes written,
 * or 0 if buf is too small (minimum: 32 + 128*16 = 2080 bytes).
 *
 * Useful for demos and boot-time testing when no external .psf file is present.
 */
uint32_t psf2_make_from_builtin(uint8_t *buf, uint32_t buf_len);

#endif /* _KERNEL_PSF_H */
