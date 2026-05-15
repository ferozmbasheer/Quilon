/*
 * test_psf.c -- host-side unit tests for the PSF2 bitmap font loader.
 *
 * Only the pure-C static-inline helpers from psf.h are tested here:
 *   - psf2_parse()       -- header validation and font state setup
 *   - psf2_glyph_pixel() -- per-pixel accessor
 *
 * psf.c is NOT linked -- it calls vbe_draw_pixel() (hardware-only) and
 * references vbe_font8x8 from vbe.h (fine here as a static array, but the
 * kernel init paths that call paging_map_page_alloc are absent).
 *
 * The tests build minimal synthetic PSF2 images in stack/static buffers and
 * verify correct parsing and pixel access at known coordinates.
 */

#include <stddef.h>
#include "framework.h"
#include "../kernel/include/kernel/psf.h"

/* -- Helpers -------------------------------------------------------------- */

/*
 * build_psf2 -- write a valid PSF2 image for a width×height font with
 * glyph_count glyphs into buf[0..buf_len-1].  All glyph bitmaps are zeroed
 * except glyph 'A' (0x41) whose first byte in every row is set to 0x80
 * (only the leftmost pixel lit).
 *
 * Returns the total byte length of the image, or 0 if buf is too small.
 */
static uint32_t build_psf2(uint8_t *buf, uint32_t buf_len,
                             uint32_t width, uint32_t height,
                             uint32_t glyph_count)
{
    uint32_t bytes_per_row   = (width + 7u) / 8u;
    uint32_t bytes_per_glyph = bytes_per_row * height;
    uint32_t hdr_size        = (uint32_t)sizeof(psf2_header_t);
    uint32_t total           = hdr_size + glyph_count * bytes_per_glyph;

    if (buf_len < total) return 0u;

    /* Zero-fill */
    for (uint32_t i = 0; i < total; i++) buf[i] = 0u;

    /* Write header as individual uint32_t words (avoids strict-aliasing) */
    uint32_t *w = (uint32_t *)(void *)buf;
    w[0] = PSF2_MAGIC;
    w[1] = 0u;                /* version */
    w[2] = hdr_size;          /* header_size */
    w[3] = 0u;                /* flags: no unicode table */
    w[4] = glyph_count;
    w[5] = bytes_per_glyph;
    w[6] = height;
    w[7] = width;

    /* Set glyph 'A' (0x41): leftmost pixel in every row */
    if (0x41u < glyph_count) {
        uint8_t *g = buf + hdr_size + 0x41u * bytes_per_glyph;
        for (uint32_t row = 0; row < height; row++)
            g[row * bytes_per_row] = 0x80u;  /* bit 7 = leftmost pixel */
    }

    return total;
}

/* -- psf2_parse: valid image ----------------------------------------------- */

static void test_parse_valid_8x8(void)
{
    uint8_t    buf[32 + 128 * 8];
    uint32_t   sz = build_psf2(buf, sizeof(buf), 8, 8, 128);
    ASSERT_NE((int)sz, 0, "build_psf2 8x8 succeeds");

    psf2_font_t font;
    int r = psf2_parse(buf, sz, &font);
    ASSERT_EQ(r, 0, "psf2_parse: valid 8x8 returns 0");
    ASSERT_EQ((int)font.width,          8,   "font.width == 8");
    ASSERT_EQ((int)font.height,         8,   "font.height == 8");
    ASSERT_EQ((int)font.glyph_count,  128,   "font.glyph_count == 128");
    ASSERT_EQ((int)font.bytes_per_glyph, 8,  "font.bytes_per_glyph == 8");
    ASSERT_NE((int)(uintptr_t)font.glyphs, 0, "font.glyphs != NULL");
    /* glyphs must point inside buf */
    ASSERT_EQ((int)(font.glyphs >= buf && font.glyphs < buf + sz), 1,
              "font.glyphs points inside the image");
}

static void test_parse_valid_8x16(void)
{
    uint8_t    buf[32 + 128 * 16];
    uint32_t   sz = build_psf2(buf, sizeof(buf), 8, 16, 128);
    ASSERT_NE((int)sz, 0, "build_psf2 8x16 succeeds");

    psf2_font_t font;
    int r = psf2_parse(buf, sz, &font);
    ASSERT_EQ(r, 0, "psf2_parse: valid 8x16 returns 0");
    ASSERT_EQ((int)font.width,  8,  "8x16: width==8");
    ASSERT_EQ((int)font.height, 16, "8x16: height==16");
}

static void test_parse_valid_12x24(void)
{
    /* 12-pixel-wide glyph: ceil(12/8)=2 bytes per row -> 2×24=48 bytes/glyph */
    uint8_t    buf[32 + 64 * 48];
    uint32_t   sz = build_psf2(buf, sizeof(buf), 12, 24, 64);
    ASSERT_NE((int)sz, 0, "build_psf2 12x24 succeeds");

    psf2_font_t font;
    int r = psf2_parse(buf, sz, &font);
    ASSERT_EQ(r, 0, "psf2_parse: valid 12x24 returns 0");
    ASSERT_EQ((int)font.width,         12, "12x24: width==12");
    ASSERT_EQ((int)font.height,        24, "12x24: height==24");
    ASSERT_EQ((int)font.bytes_per_glyph, 48, "12x24: bpg==48");
}

/* -- psf2_parse: error cases ---------------------------------------------- */

static void test_parse_null_data(void)
{
    psf2_font_t font;
    ASSERT_EQ(psf2_parse(NULL, 100, &font), -1, "NULL data -> -1");
}

static void test_parse_null_out(void)
{
    uint8_t buf[2080];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 16, 128);
    ASSERT_NE((int)sz, 0, "build image for null-out test");
    ASSERT_EQ(psf2_parse(buf, sz, NULL), -1, "NULL out -> -1");
}

static void test_parse_too_short(void)
{
    uint8_t  buf[16] = {0};
    psf2_font_t font;
    ASSERT_EQ(psf2_parse(buf, 16, &font), -1, "buffer < 32 bytes -> -1");
    ASSERT_EQ(psf2_parse(buf, 0,  &font), -1, "buffer == 0 bytes -> -1");
}

static void test_parse_wrong_magic(void)
{
    uint8_t  buf[32 + 128 * 8];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 8, 128);

    /* Corrupt the magic */
    buf[0] = 0xDE; buf[1] = 0xAD; buf[2] = 0xBE; buf[3] = 0xEF;

    psf2_font_t font;
    ASSERT_EQ(psf2_parse(buf, sz, &font), -1, "wrong magic -> -1");
}

static void test_parse_wrong_version(void)
{
    uint8_t  buf[32 + 128 * 8];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 8, 128);

    /* Set version to 1 (only 0 is valid) */
    uint32_t *w = (uint32_t *)(void *)buf;
    w[1] = 1u;

    psf2_font_t font;
    ASSERT_EQ(psf2_parse(buf, sz, &font), -1, "version=1 -> -1");
}

static void test_parse_zero_width(void)
{
    uint8_t  buf[32 + 128 * 8];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 8, 128);
    uint32_t *w = (uint32_t *)(void *)buf;
    w[7] = 0u;   /* width field */

    psf2_font_t font;
    ASSERT_EQ(psf2_parse(buf, sz, &font), -1, "width=0 -> -1");
}

static void test_parse_zero_height(void)
{
    uint8_t  buf[32 + 128 * 8];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 8, 128);
    uint32_t *w = (uint32_t *)(void *)buf;
    w[6] = 0u;   /* height field */

    psf2_font_t font;
    ASSERT_EQ(psf2_parse(buf, sz, &font), -1, "height=0 -> -1");
}

static void test_parse_zero_glyph_count(void)
{
    uint8_t  buf[32 + 128 * 8];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 8, 128);
    uint32_t *w = (uint32_t *)(void *)buf;
    w[4] = 0u;   /* glyph_count field */

    psf2_font_t font;
    ASSERT_EQ(psf2_parse(buf, sz, &font), -1, "glyph_count=0 -> -1");
}

static void test_parse_header_too_small(void)
{
    uint8_t  buf[32 + 128 * 8];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 8, 128);
    uint32_t *w = (uint32_t *)(void *)buf;
    w[2] = 16u;  /* header_size < 32 */

    psf2_font_t font;
    ASSERT_EQ(psf2_parse(buf, sz, &font), -1, "header_size<32 -> -1");
}

static void test_parse_header_beyond_image(void)
{
    uint8_t  buf[32 + 128 * 8];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 8, 128);
    uint32_t *w = (uint32_t *)(void *)buf;
    w[2] = sz + 1u;  /* header_size > total */

    psf2_font_t font;
    ASSERT_EQ(psf2_parse(buf, sz, &font), -1, "header_size>len -> -1");
}

static void test_parse_bpg_inconsistent(void)
{
    /* bytes_per_glyph=4 but width=8,height=8 requires bpg >= 8 */
    uint8_t  buf[32 + 128 * 8];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 8, 128);
    uint32_t *w = (uint32_t *)(void *)buf;
    w[5] = 4u;   /* bytes_per_glyph too small */

    psf2_font_t font;
    ASSERT_EQ(psf2_parse(buf, sz, &font), -1, "bpg too small -> -1");
}

static void test_parse_glyphs_overflow(void)
{
    /* Claim 1000 glyphs but image only has room for 128 */
    uint8_t  buf[32 + 128 * 8];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 8, 128);
    uint32_t *w = (uint32_t *)(void *)buf;
    w[4] = 1000u;   /* glyph_count */

    psf2_font_t font;
    ASSERT_EQ(psf2_parse(buf, sz, &font), -1, "glyph data overflow -> -1");
}

/* -- psf2_glyph_pixel: basic pixel access --------------------------------- */

static void test_pixel_space_all_zero(void)
{
    uint8_t buf[32 + 128 * 8];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 8, 128);
    psf2_font_t font;
    psf2_parse(buf, sz, &font);

    int all_zero = 1;
    for (uint32_t row = 0; row < 8; row++) {
        for (uint32_t col = 0; col < 8; col++) {
            if (psf2_glyph_pixel(&font, 0x20, col, row))
                all_zero = 0;
        }
    }
    ASSERT_EQ(all_zero, 1, "space glyph (0x20): all pixels are zero");
}

static void test_pixel_A_leftmost_column(void)
{
    /* 'A' (0x41) was set with bit 7 in every row byte -> leftmost pixel lit */
    uint8_t buf[32 + 128 * 8];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 8, 128);
    psf2_font_t font;
    psf2_parse(buf, sz, &font);

    int all_set = 1;
    for (uint32_t row = 0; row < 8; row++) {
        if (!psf2_glyph_pixel(&font, 0x41, 0, row))  /* col=0: leftmost */
            all_set = 0;
    }
    ASSERT_NE(all_set, 0, "'A': leftmost pixel lit in every row");
}

static void test_pixel_A_second_column_zero(void)
{
    /* Only bit 7 (col=0) was set; col=1 must be zero */
    uint8_t buf[32 + 128 * 8];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 8, 128);
    psf2_font_t font;
    psf2_parse(buf, sz, &font);

    int all_zero = 1;
    for (uint32_t row = 0; row < 8; row++) {
        if (psf2_glyph_pixel(&font, 0x41, 1, row))
            all_zero = 0;
    }
    ASSERT_EQ(all_zero, 1, "'A': col=1 is zero (only col=0 was set)");
}

static void test_pixel_out_of_bounds(void)
{
    uint8_t buf[32 + 128 * 8];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 8, 128);
    psf2_font_t font;
    psf2_parse(buf, sz, &font);

    ASSERT_EQ(psf2_glyph_pixel(&font, 0x41, 8, 0), 0,
              "col==width -> 0");
    ASSERT_EQ(psf2_glyph_pixel(&font, 0x41, 0, 8), 0,
              "row==height -> 0");
    ASSERT_EQ(psf2_glyph_pixel(&font, 0x41, 255, 255), 0,
              "col=row=255 -> 0");
    ASSERT_EQ(psf2_glyph_pixel(&font, 128, 0, 0), 0,
              "ch==glyph_count -> 0");
    ASSERT_EQ(psf2_glyph_pixel(&font, 9999, 0, 0), 0,
              "ch=9999 -> 0");
}

static void test_pixel_null_font(void)
{
    ASSERT_EQ(psf2_glyph_pixel(NULL, 'A', 0, 0), 0,
              "NULL font -> 0");
}

static void test_pixel_null_glyphs(void)
{
    psf2_font_t font = { NULL, 8, 8, 8, 128 };
    ASSERT_EQ(psf2_glyph_pixel(&font, 'A', 0, 0), 0,
              "font.glyphs==NULL -> 0");
}

/* -- psf2_glyph_pixel: multi-byte-row font (width > 8) ------------------- */

static void test_pixel_wide_glyph(void)
{
    /* 12-wide glyph: ceil(12/8)=2 bytes per row.
     * Set bit 0 of byte 1 of row 0 for glyph 'A' -> pixel (col=11, row=0) lit.
     * That is: glyph_data[row=0][byte=1] |= 0x01 -> bit 0 = col 8+7=15? No wait.
     * bit 7 of byte 0 = col 0
     * bit 6 of byte 0 = col 1
     * ...
     * bit 0 of byte 0 = col 7
     * bit 7 of byte 1 = col 8
     * bit 6 of byte 1 = col 9
     * bit 5 of byte 1 = col 10
     * bit 4 of byte 1 = col 11
     * ...
     * So to light col=11: byte 1, bit 4 -> mask = 0x10
     */
    const uint32_t W = 12u, H = 8u, N = 128u;
    const uint32_t bpr = 2u;     /* ceil(12/8) */
    const uint32_t bpg = bpr * H;  /* = 16 */
    const uint32_t total = 32u + N * bpg;

    static uint8_t buf[32 + 128 * 16];
    if (total > sizeof(buf)) {
        ASSERT_EQ(0, 1, "test_pixel_wide_glyph: buffer too small");
        return;
    }

    uint32_t sz = build_psf2(buf, sizeof(buf), W, H, N);
    ASSERT_NE((int)sz, 0, "wide-glyph: build succeeds");

    /* Set col=11 in row=0 of glyph 'A' */
    uint8_t *g = buf + 32u + 0x41u * bpg;
    g[0 * bpr + 1] |= 0x10u;   /* row=0, byte=1, bit4 -> col=11 */

    psf2_font_t font;
    int r = psf2_parse(buf, sz, &font);
    ASSERT_EQ(r, 0, "wide-glyph: parse ok");

    ASSERT_EQ(psf2_glyph_pixel(&font, 0x41, 11, 0), 1,
              "wide: col=11,row=0 lit after setting byte1 bit4");
    ASSERT_EQ(psf2_glyph_pixel(&font, 0x41, 10, 0), 0,
              "wide: col=10 not lit");
    ASSERT_EQ(psf2_glyph_pixel(&font, 0x41, 12, 0), 0,
              "wide: col=12 (>= width) -> 0");
}

/* -- psf2_glyph_pixel: large glyph_count --------------------------------- */

static void test_parse_large_glyph_count(void)
{
    /* 256 glyphs, 8×8 */
    static uint8_t buf[32 + 256 * 8];
    uint32_t sz = build_psf2(buf, sizeof(buf), 8, 8, 256);
    ASSERT_NE((int)sz, 0, "256-glyph build succeeds");

    psf2_font_t font;
    ASSERT_EQ(psf2_parse(buf, sz, &font), 0, "256-glyph parse ok");
    ASSERT_EQ((int)font.glyph_count, 256, "glyph_count==256");
    ASSERT_EQ(psf2_glyph_pixel(&font, 255, 0, 0), 0, "ch=255 last glyph ok");
    ASSERT_EQ(psf2_glyph_pixel(&font, 256, 0, 0), 0, "ch=256 OOB -> 0");
}

/* -- psf2_parse: header_size > 32 (valid extended header) --------------- */

static void test_parse_extended_header(void)
{
    /* PSF2 allows header_size > 32 (reserved extra bytes). Glyph data
     * starts at header_size, not at 32. */
    const uint32_t ext_hdr = 64u;  /* 32 extra bytes of "future fields" */
    const uint32_t bpg = 8u;
    const uint32_t n   = 64u;
    const uint32_t total = ext_hdr + n * bpg;

    static uint8_t buf[64 + 64 * 8];
    for (uint32_t i = 0; i < total; i++) buf[i] = 0u;

    uint32_t *w = (uint32_t *)(void *)buf;
    w[0] = PSF2_MAGIC;
    w[1] = 0u;
    w[2] = ext_hdr;   /* header_size = 64 */
    w[3] = 0u;
    w[4] = n;
    w[5] = bpg;
    w[6] = 8u;  /* height */
    w[7] = 8u;  /* width */

    psf2_font_t font;
    int r = psf2_parse(buf, total, &font);
    ASSERT_EQ(r, 0, "extended header (hdr_size=64) parses ok");
    ASSERT_EQ((int)(font.glyphs - buf), (int)ext_hdr,
              "glyphs pointer starts after extended header");
}

/* -- PSF2_MAGIC constant --------------------------------------------------- */

static void test_magic_value(void)
{
    ASSERT_EQ((int)PSF2_MAGIC, (int)0x864AB572u, "PSF2_MAGIC == 0x864AB572");
}

/* -- psf2_header_t size ---------------------------------------------------- */

static void test_header_size(void)
{
    ASSERT_EQ((int)sizeof(psf2_header_t), 32, "psf2_header_t is 32 bytes");
}

/* -- Main ------------------------------------------------------------------ */

int main(void)
{
    RUN_SUITE(test_magic_value);
    RUN_SUITE(test_header_size);

    RUN_SUITE(test_parse_valid_8x8);
    RUN_SUITE(test_parse_valid_8x16);
    RUN_SUITE(test_parse_valid_12x24);

    RUN_SUITE(test_parse_null_data);
    RUN_SUITE(test_parse_null_out);
    RUN_SUITE(test_parse_too_short);
    RUN_SUITE(test_parse_wrong_magic);
    RUN_SUITE(test_parse_wrong_version);
    RUN_SUITE(test_parse_zero_width);
    RUN_SUITE(test_parse_zero_height);
    RUN_SUITE(test_parse_zero_glyph_count);
    RUN_SUITE(test_parse_header_too_small);
    RUN_SUITE(test_parse_header_beyond_image);
    RUN_SUITE(test_parse_bpg_inconsistent);
    RUN_SUITE(test_parse_glyphs_overflow);
    RUN_SUITE(test_parse_large_glyph_count);
    RUN_SUITE(test_parse_extended_header);

    RUN_SUITE(test_pixel_space_all_zero);
    RUN_SUITE(test_pixel_A_leftmost_column);
    RUN_SUITE(test_pixel_A_second_column_zero);
    RUN_SUITE(test_pixel_out_of_bounds);
    RUN_SUITE(test_pixel_null_font);
    RUN_SUITE(test_pixel_null_glyphs);
    RUN_SUITE(test_pixel_wide_glyph);

    TEST_SUMMARY();
}
