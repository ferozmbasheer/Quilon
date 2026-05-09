/*
 * test_vbe.c — host-side unit tests for the VBE framebuffer driver helpers.
 *
 * Only the pure-C / static-inline functions from vbe.h are tested here:
 *   - vbe_pixel_offset()  — framebuffer byte-offset math
 *   - vbe_glyph_pixel()   — font bitmap lookup
 *   - vbe_term_cols()     — terminal column count from pixel width
 *   - vbe_term_rows()     — terminal row count from pixel height
 *   - Color constant values and uniqueness
 *   - VBE_FONT_W / VBE_FONT_H constants
 *
 * vbe.c (the actual kernel driver) is NOT linked — it depends on
 * paging_map_page_alloc() and hardware MMIO which are unavailable on the host.
 */

#include "framework.h"
#include "../kernel/include/kernel/vbe.h"

/* ── Color constants ───────────────────────────────────────────────────────── */

static void test_color_values(void)
{
    /* Black must be fully zero. */
    ASSERT_EQ((int)VBE_COLOR_BLACK, 0x00000000, "VBE_COLOR_BLACK is zero");
    /* White must have all three channels fully lit (upper byte is X/alpha). */
    ASSERT_NE(((int)VBE_COLOR_WHITE & 0xFF0000), 0, "VBE_COLOR_WHITE has red channel");
    ASSERT_NE(((int)VBE_COLOR_WHITE & 0x00FF00), 0, "VBE_COLOR_WHITE has green channel");
    ASSERT_NE(((int)VBE_COLOR_WHITE & 0x0000FF), 0, "VBE_COLOR_WHITE has blue channel");
    /* Red must have a red channel but no blue channel. */
    ASSERT_NE(((int)VBE_COLOR_RED & 0xFF0000), 0, "VBE_COLOR_RED has red channel");
    ASSERT_EQ(((int)VBE_COLOR_RED & 0x0000FF), 0, "VBE_COLOR_RED has no blue");
    /* Blue must have a blue channel but no red channel. */
    ASSERT_NE(((int)VBE_COLOR_BLUE & 0x0000FF), 0, "VBE_COLOR_BLUE has blue channel");
    ASSERT_EQ(((int)VBE_COLOR_BLUE & 0xFF0000), 0, "VBE_COLOR_BLUE has no red");
}

static void test_color_uniqueness(void)
{
    uint32_t colors[] = {
        VBE_COLOR_BLACK, VBE_COLOR_WHITE,     VBE_COLOR_RED,       VBE_COLOR_GREEN,
        VBE_COLOR_BLUE,  VBE_COLOR_CYAN,      VBE_COLOR_MAGENTA,   VBE_COLOR_YELLOW,
        VBE_COLOR_ORANGE, VBE_COLOR_DARK_GREY, VBE_COLOR_LIGHT_GREY, VBE_COLOR_DARK_BLUE
    };
    int n = (int)(sizeof(colors) / sizeof(colors[0]));
    int all_unique = 1;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (colors[i] == colors[j]) { all_unique = 0; }
        }
    }
    ASSERT_NE(all_unique, 0, "all 12 color constants are unique");
}

/* ── Font metrics ──────────────────────────────────────────────────────────── */

static void test_font_metrics(void)
{
    ASSERT_EQ((int)VBE_FONT_W, 8,  "VBE_FONT_W == 8");
    ASSERT_EQ((int)VBE_FONT_H, 16, "VBE_FONT_H == 16");
}

/* ── vbe_term_cols / vbe_term_rows ─────────────────────────────────────────── */

static void test_term_dimensions(void)
{
    ASSERT_EQ((int)vbe_term_cols(800), 100, "800px / 8 = 100 cols");
    ASSERT_EQ((int)vbe_term_rows(600),  37, "600px / 16 = 37 rows");
    ASSERT_EQ((int)vbe_term_cols(1024), 128, "1024px / 8 = 128 cols");
    ASSERT_EQ((int)vbe_term_rows(768),   48, "768px / 16 = 48 rows");
    ASSERT_EQ((int)vbe_term_cols(0), 0, "0px width = 0 cols");
    ASSERT_EQ((int)vbe_term_rows(0), 0, "0px height = 0 rows");
    ASSERT_EQ((int)vbe_term_cols(801), 100, "801px / 8 = 100 (floor)");
    ASSERT_EQ((int)vbe_term_rows(601),  37, "601px / 16 = 37 (floor)");
}

/* ── vbe_pixel_offset ──────────────────────────────────────────────────────── */

static void test_pixel_offset_32bpp(void)
{
    const uint32_t pitch     = 800 * 4;
    const uint32_t bpp_bytes = 4;

    ASSERT_EQ((int)vbe_pixel_offset(0, 0, pitch, bpp_bytes), 0,
              "pixel_offset(0,0) == 0");
    ASSERT_EQ((int)vbe_pixel_offset(0, 1, pitch, bpp_bytes), (int)pitch,
              "pixel_offset(0,1) == pitch");
    ASSERT_EQ((int)vbe_pixel_offset(1, 0, pitch, bpp_bytes), (int)bpp_bytes,
              "pixel_offset(1,0) == bpp_bytes");
    ASSERT_EQ((int)vbe_pixel_offset(3, 2, pitch, bpp_bytes),
              (int)(2 * pitch + 3 * bpp_bytes),
              "pixel_offset(3,2) == 2*pitch + 3*bpp");
}

static void test_pixel_offset_24bpp(void)
{
    const uint32_t pitch     = 800 * 3;
    const uint32_t bpp_bytes = 3;

    ASSERT_EQ((int)vbe_pixel_offset(2, 1, pitch, bpp_bytes),
              (int)(1 * pitch + 2 * bpp_bytes),
              "24bpp pixel_offset(2,1)");
    ASSERT_EQ((int)vbe_pixel_offset(0, 0, pitch, bpp_bytes), 0,
              "24bpp pixel_offset(0,0) == 0");
}

/* ── vbe_glyph_pixel ───────────────────────────────────────────────────────── */

static void test_glyph_space(void)
{
    int all_zero = 1;
    for (uint32_t row = 0; row < 8; row++) {
        for (uint32_t col = 0; col < VBE_FONT_W; col++) {
            if (vbe_glyph_pixel(0x20, col, row)) all_zero = 0;
        }
    }
    ASSERT_EQ(all_zero, 1, "space glyph is all-zero");
}

static void test_glyph_A_nonblank(void)
{
    int any_set = 0;
    for (uint32_t row = 0; row < 8; row++) {
        for (uint32_t col = 0; col < VBE_FONT_W; col++) {
            if (vbe_glyph_pixel('A', col, row)) any_set = 1;
        }
    }
    ASSERT_NE(any_set, 0, "'A' glyph has at least one lit pixel");
}

static void test_glyph_A_top_half(void)
{
    int top_half = 0;
    for (uint32_t row = 0; row < 4; row++) {
        for (uint32_t col = 0; col < VBE_FONT_W; col++) {
            if (vbe_glyph_pixel('A', col, row)) top_half = 1;
        }
    }
    ASSERT_NE(top_half, 0, "'A' has lit pixels in top half (rows 0-3)");
}

static void test_glyph_line_spacing(void)
{
    int all_zero = 1;
    for (uint32_t row = 8; row < (uint32_t)VBE_FONT_H; row++) {
        for (uint32_t col = 0; col < VBE_FONT_W; col++) {
            if (vbe_glyph_pixel('A', col, row)) all_zero = 0;
        }
    }
    ASSERT_EQ(all_zero, 1, "rows 8-15 (line spacing) are always zero");
}

static void test_glyph_boundary(void)
{
    ASSERT_EQ(vbe_glyph_pixel(128, 0, 0), 0, "char 128 (out of range) -> 0");
    ASSERT_EQ(vbe_glyph_pixel('A', VBE_FONT_W, 0), 0, "col == FONT_W -> 0");
    ASSERT_EQ(vbe_glyph_pixel('A', 255, 0), 0, "col 255 (OOB) -> 0");
    ASSERT_EQ(vbe_glyph_pixel('A', 0, 8), 0, "row 8 (line spacing) -> 0");
}

static void test_glyph_printable_nonblank(void)
{
    int all_ok = 1;
    for (unsigned char c = 0x21; c <= 0x7E; c++) {
        int any = 0;
        for (uint32_t row = 0; row < 8 && !any; row++) {
            for (uint32_t col = 0; col < VBE_FONT_W && !any; col++) {
                if (vbe_glyph_pixel(c, col, row)) any = 1;
            }
        }
        if (!any) all_ok = 0;
    }
    ASSERT_NE(all_ok, 0, "every printable ASCII glyph (0x21-0x7E) is non-blank");
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
    RUN_SUITE(test_color_values);
    RUN_SUITE(test_color_uniqueness);
    RUN_SUITE(test_font_metrics);
    RUN_SUITE(test_term_dimensions);
    RUN_SUITE(test_pixel_offset_32bpp);
    RUN_SUITE(test_pixel_offset_24bpp);
    RUN_SUITE(test_glyph_space);
    RUN_SUITE(test_glyph_A_nonblank);
    RUN_SUITE(test_glyph_A_top_half);
    RUN_SUITE(test_glyph_line_spacing);
    RUN_SUITE(test_glyph_boundary);
    RUN_SUITE(test_glyph_printable_nonblank);
    TEST_SUMMARY();
}
