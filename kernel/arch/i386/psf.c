/*
 * Quilon OS -- PSF2 Bitmap Font Loader  (section 11.2)
 *
 * Implements:
 *   psf2_load()           -- validates and registers the active font
 *   psf2_get_font()       -- returns the active psf2_font_t pointer
 *   psf2_draw_glyph()     -- renders one glyph to the VBE framebuffer
 *   psf2_make_from_builtin() -- synthesizes a PSF2 image from the 8×8 built-in
 */

#include <stdint.h>
#include <stdbool.h>

#include <kernel/psf.h>
#include <kernel/vbe.h>

/* -- Module state ----------------------------------------------------------- */

static psf2_font_t g_font;
static bool        g_loaded = false;

/* -- psf2_load -------------------------------------------------------------- */

int psf2_load(const uint8_t *data, uint32_t len)
{
    int r = psf2_parse(data, len, &g_font);
    if (r == 0)
        g_loaded = true;
    return r;
}

/* -- psf2_get_font ---------------------------------------------------------- */

const psf2_font_t *psf2_get_font(void)
{
    return g_loaded ? &g_font : (const psf2_font_t *)0;
}

/* -- psf2_draw_glyph -------------------------------------------------------- */

void psf2_draw_glyph(uint32_t ch, uint32_t x, uint32_t y,
                     uint32_t fg, uint32_t bg)
{
    if (!g_loaded) return;
    if (ch >= g_font.glyph_count) ch = (uint32_t)'?';

    for (uint32_t row = 0; row < g_font.height; row++) {
        for (uint32_t col = 0; col < g_font.width; col++) {
            uint32_t color = psf2_glyph_pixel(&g_font, ch, col, row) ? fg : bg;
            vbe_draw_pixel(x + col, y + row, color);
        }
    }
}

/* -- psf2_make_from_builtin ------------------------------------------------- */

uint32_t psf2_make_from_builtin(uint8_t *buf, uint32_t buf_len)
{
    /* Produces an 8×16 PSF2 font for the 128 ASCII codepoints.
     * Rows 0–7 come from vbe_font8x8[c][row]; rows 8–15 are blank. */
    const uint32_t GLYPH_W  = 8u;
    const uint32_t GLYPH_H  = 16u;
    const uint32_t GLYPH_N  = 128u;
    const uint32_t BPG       = GLYPH_H;          /* 1 byte/row × 16 rows */
    const uint32_t HDR_SIZE  = (uint32_t)sizeof(psf2_header_t);
    const uint32_t total     = HDR_SIZE + GLYPH_N * BPG;  /* = 2080 */

    if (!buf || buf_len < total) return 0u;

    /* Write the 32-byte header */
    psf2_header_t *hdr   = (psf2_header_t *)buf;
    hdr->magic            = PSF2_MAGIC;
    hdr->version          = 0u;
    hdr->header_size      = HDR_SIZE;
    hdr->flags            = 0u;   /* no unicode table */
    hdr->glyph_count      = GLYPH_N;
    hdr->bytes_per_glyph  = BPG;
    hdr->height           = GLYPH_H;
    hdr->width            = GLYPH_W;

    /* Write glyph bitmaps */
    uint8_t *glyphs = buf + HDR_SIZE;
    for (uint32_t c = 0; c < GLYPH_N; c++) {
        uint8_t *g = glyphs + c * BPG;
        for (uint32_t row = 0; row < GLYPH_H; row++)
            g[row] = (row < 8u) ? vbe_font8x8[c][row] : 0x00u;
    }

    return total;
}
