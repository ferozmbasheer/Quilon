/*
 * test_gfx.c -- host-side unit tests for the user-space 2D graphics library.
 *
 * gfx.c is compiled directly: its only dependencies are malloc/free (resolved
 * from the host libc) and the gfx.h / stdlib.h declarations from the user
 * libc include path.  No kernel headers, no hardware, no syscalls.
 *
 * What is tested:
 *   - GFX_RGB / GFX_RGBA colour macros
 *   - canvas_create / canvas_free
 *   - gfx_fill
 *   - gfx_fill_rect  (boundary, clipping, negative coords)
 *   - gfx_draw_rect  (outline, interior untouched)
 *   - clip rectangle  (gfx_set_clip / gfx_clear_clip)
 *   - gfx_blit       (full, partial, offset)
 *   - gfx_draw_text  (pixel-level verification of one glyph)
 *   - GFX_CHAR_W / GFX_CHAR_H constants
 */

#include <stddef.h>  /* NULL */
#include "framework.h"
#include "../user/libgfx/include/gfx.h"

/* ── helper: count pixels in canvas equal to a given colour ─────────────── */
static int count_pixels(const canvas_t *c, color_t col)
{
    int n = 0;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++)
            if (c->pixels[y * c->pitch + x] == col) n++;
    return n;
}

/* ── helper: true if ALL pixels in canvas equal col ───────────────────────  */
static int all_pixels(const canvas_t *c, color_t col)
{
    return count_pixels(c, col) == c->w * c->h;
}

/* ── helper: true if pixel (x,y) equals col ──────────────────────────────── */
static int pixel_eq(const canvas_t *c, int x, int y, color_t col)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return 0;
    return c->pixels[y * c->pitch + x] == col;
}

/* ======================================================================== */

static void test_color_macros(void)
{
    /* GFX_RGB packs channels into bits 23-16 (R), 15-8 (G), 7-0 (B). */
    ASSERT_EQ((int)GFX_RGB(255, 0, 0),   0x00FF0000, "GFX_RGB pure red");
    ASSERT_EQ((int)GFX_RGB(0, 255, 0),   0x0000FF00, "GFX_RGB pure green");
    ASSERT_EQ((int)GFX_RGB(0, 0, 255),   0x000000FF, "GFX_RGB pure blue");
    ASSERT_EQ((int)GFX_RGB(0, 0, 0),     0x00000000, "GFX_RGB black");
    ASSERT_EQ((int)GFX_RGB(255,255,255), 0x00FFFFFF, "GFX_RGB white");
    ASSERT_EQ((int)GFX_RGB(18, 52, 86),
              (int)((18 << 16) | (52 << 8) | 86), "GFX_RGB arbitrary");

    /* GFX_RGBA includes alpha in bits 31-24. */
    ASSERT_EQ((int)GFX_RGBA(255, 0, 0, 128),
              (int)((128u << 24) | (255u << 16)), "GFX_RGBA alpha in high byte");

    /* Standard palette entries are non-zero and distinct. */
    ASSERT_NE((int)GFX_WHITE,     0, "GFX_WHITE non-zero");
    ASSERT_NE((int)GFX_RED,       0, "GFX_RED non-zero");
    ASSERT_NE((int)GFX_BLUE,      0, "GFX_BLUE non-zero");
    ASSERT_NE((int)GFX_BLACK, (int)GFX_WHITE, "black != white");
    ASSERT_NE((int)GFX_RED,   (int)GFX_BLUE,  "red != blue");
}

/* ── canvas_create / canvas_free ────────────────────────────────────────── */

static void test_canvas_create(void)
{
    /* Normal creation. */
    canvas_t *c = canvas_create(10, 8);
    ASSERT_NOTNULL(c,          "canvas_create returns non-NULL");
    ASSERT_NOTNULL(c->pixels,  "canvas has pixel buffer");
    ASSERT_EQ(c->w,     10,    "canvas width correct");
    ASSERT_EQ(c->h,      8,    "canvas height correct");
    ASSERT_EQ(c->pitch, 10,    "canvas pitch == width (no padding)");
    ASSERT_EQ(c->has_clip, 0,  "no clip rect after create");
    canvas_free(c);

    /* Degenerate sizes return NULL. */
    ASSERT_NULL(canvas_create(0, 10), "canvas_create(0, h) == NULL");
    ASSERT_NULL(canvas_create(10, 0), "canvas_create(w, 0) == NULL");
    ASSERT_NULL(canvas_create(0, 0),  "canvas_create(0, 0) == NULL");

    /* canvas_free(NULL) is safe. */
    canvas_free(NULL);   /* must not crash */
    ASSERT(1, "canvas_free(NULL) does not crash");
}

/* ── gfx_fill ───────────────────────────────────────────────────────────── */

static void test_gfx_fill(void)
{
    canvas_t *c = canvas_create(5, 5);

    gfx_fill(c, GFX_RGB(255, 0, 0));
    ASSERT(all_pixels(c, GFX_RGB(255, 0, 0)), "gfx_fill colours every pixel");

    gfx_fill(c, GFX_BLACK);
    ASSERT(all_pixels(c, GFX_BLACK), "gfx_fill overwrites to black");

    canvas_free(c);
}

/* ── gfx_fill_rect ──────────────────────────────────────────────────────── */

static void test_gfx_fill_rect_basic(void)
{
    canvas_t *c = canvas_create(20, 20);
    gfx_fill(c, GFX_BLACK);

    /* Fill a 4×3 rect starting at (2,5). */
    gfx_fill_rect(c, (rect_t){2, 5, 4, 3}, GFX_WHITE);

    /* All pixels inside should be white. */
    ASSERT(pixel_eq(c, 2, 5, GFX_WHITE), "fill_rect top-left corner");
    ASSERT(pixel_eq(c, 5, 5, GFX_WHITE), "fill_rect top-right corner");
    ASSERT(pixel_eq(c, 2, 7, GFX_WHITE), "fill_rect bottom-left corner");
    ASSERT(pixel_eq(c, 5, 7, GFX_WHITE), "fill_rect bottom-right corner");

    /* Pixels just outside the rect must still be black. */
    ASSERT(pixel_eq(c, 1, 5, GFX_BLACK), "pixel left of rect untouched");
    ASSERT(pixel_eq(c, 6, 5, GFX_BLACK), "pixel right of rect untouched");
    ASSERT(pixel_eq(c, 2, 4, GFX_BLACK), "pixel above rect untouched");
    ASSERT(pixel_eq(c, 2, 8, GFX_BLACK), "pixel below rect untouched");

    /* Exact pixel count: 4×3 = 12. */
    ASSERT_EQ(count_pixels(c, GFX_WHITE), 12, "fill_rect writes exactly 12 pixels");

    canvas_free(c);
}

static void test_gfx_fill_rect_clipping(void)
{
    canvas_t *c = canvas_create(10, 10);
    gfx_fill(c, GFX_BLACK);

    /* Rect that extends past the right/bottom edge — clipped to canvas. */
    gfx_fill_rect(c, (rect_t){7, 7, 10, 10}, GFX_WHITE);
    /* Only the 3×3 overlap (7..9, 7..9) should be white. */
    ASSERT_EQ(count_pixels(c, GFX_WHITE), 9, "fill_rect clipped to 3x3 = 9 pixels");

    gfx_fill(c, GFX_BLACK);

    /* Rect with negative origin — only part inside canvas drawn. */
    gfx_fill_rect(c, (rect_t){-3, -3, 6, 6}, GFX_WHITE);
    /* Visible region: (0..2, 0..2) = 3×3 = 9 pixels. */
    ASSERT_EQ(count_pixels(c, GFX_WHITE), 9, "fill_rect negative origin clipped");

    gfx_fill(c, GFX_BLACK);

    /* Rect entirely outside canvas — nothing drawn. */
    gfx_fill_rect(c, (rect_t){20, 20, 5, 5}, GFX_WHITE);
    ASSERT_EQ(count_pixels(c, GFX_WHITE), 0, "fill_rect outside canvas draws nothing");

    gfx_fill(c, GFX_BLACK);

    /* Zero-area rect — nothing drawn. */
    gfx_fill_rect(c, (rect_t){2, 2, 0, 5}, GFX_WHITE);
    ASSERT_EQ(count_pixels(c, GFX_WHITE), 0, "fill_rect width=0 draws nothing");

    gfx_fill_rect(c, (rect_t){2, 2, 5, 0}, GFX_WHITE);
    ASSERT_EQ(count_pixels(c, GFX_WHITE), 0, "fill_rect height=0 draws nothing");

    canvas_free(c);
}

/* ── gfx_draw_rect ──────────────────────────────────────────────────────── */

static void test_gfx_draw_rect(void)
{
    canvas_t *c = canvas_create(10, 10);
    gfx_fill(c, GFX_BLACK);

    /* Draw a 6×4 outline at (2,3). */
    gfx_draw_rect(c, (rect_t){2, 3, 6, 4}, GFX_WHITE);

    /* Corners must be white. */
    ASSERT(pixel_eq(c, 2, 3, GFX_WHITE), "draw_rect top-left corner");
    ASSERT(pixel_eq(c, 7, 3, GFX_WHITE), "draw_rect top-right corner");
    ASSERT(pixel_eq(c, 2, 6, GFX_WHITE), "draw_rect bottom-left corner");
    ASSERT(pixel_eq(c, 7, 6, GFX_WHITE), "draw_rect bottom-right corner");

    /* Interior pixel must be untouched (black). */
    ASSERT(pixel_eq(c, 4, 5, GFX_BLACK), "draw_rect interior untouched");
    ASSERT(pixel_eq(c, 5, 4, GFX_BLACK), "draw_rect interior untouched (2)");

    /* Outline pixel count: top+bottom = 2×6 = 12, sides (excl corners) = 2×2 = 4, total 16. */
    ASSERT_EQ(count_pixels(c, GFX_WHITE), 16, "draw_rect 6x4 outline = 16 pixels");

    canvas_free(c);
}

/* ── clip rectangle ─────────────────────────────────────────────────────── */

static void test_clip_rect(void)
{
    canvas_t *c = canvas_create(20, 20);
    gfx_fill(c, GFX_BLACK);

    /* Set a 10×10 clip in the centre. */
    gfx_set_clip(c, (rect_t){5, 5, 10, 10});
    ASSERT_EQ(c->has_clip, 1, "gfx_set_clip activates clip");

    /* Fill the entire canvas — only the clip region should change. */
    gfx_fill_rect(c, (rect_t){0, 0, 20, 20}, GFX_WHITE);
    ASSERT_EQ(count_pixels(c, GFX_WHITE), 100, "fill_rect respects 10x10 clip");

    /* Pixel inside clip. */
    ASSERT(pixel_eq(c, 10, 10, GFX_WHITE), "pixel inside clip was drawn");
    /* Pixel outside clip. */
    ASSERT(pixel_eq(c,  2,  2, GFX_BLACK), "pixel outside clip untouched");
    ASSERT(pixel_eq(c, 18, 18, GFX_BLACK), "pixel outside clip untouched (2)");

    /* Clear clip — full canvas fill now writes to all pixels. */
    gfx_clear_clip(c);
    ASSERT_EQ(c->has_clip, 0, "gfx_clear_clip deactivates clip");
    gfx_fill(c, GFX_RED);
    ASSERT_EQ(count_pixels(c, GFX_RED), 400, "fill after clear_clip covers all 400 pixels");

    /* Clip clamped to canvas bounds. */
    gfx_fill(c, GFX_BLACK);
    gfx_set_clip(c, (rect_t){-5, -5, 100, 100});   /* larger than canvas */
    gfx_fill_rect(c, (rect_t){0, 0, 20, 20}, GFX_WHITE);
    ASSERT_EQ(count_pixels(c, GFX_WHITE), 400, "oversized clip clamped to canvas");

    canvas_free(c);
}

/* ── gfx_blit ───────────────────────────────────────────────────────────── */

static void test_gfx_blit(void)
{
    canvas_t *src = canvas_create(8, 8);
    canvas_t *dst = canvas_create(16, 16);

    /* Fill src with red, dst with black. */
    gfx_fill(src, GFX_RED);
    gfx_fill(dst, GFX_BLACK);

    /* Blit entire src to top-left corner of dst. */
    gfx_blit(dst, 0, 0, src, (rect_t){0, 0, 8, 8});
    ASSERT_EQ(count_pixels(dst, GFX_RED), 64, "blit copies 8x8 = 64 pixels");

    /* Pixels outside the blit region remain black. */
    ASSERT(pixel_eq(dst, 9, 0, GFX_BLACK), "right of blit untouched");
    ASSERT(pixel_eq(dst, 0, 9, GFX_BLACK), "below blit untouched");

    gfx_fill(dst, GFX_BLACK);

    /* Blit with offset — place src at (4,4) in dst. */
    gfx_blit(dst, 4, 4, src, (rect_t){0, 0, 8, 8});
    ASSERT(pixel_eq(dst, 4,  4, GFX_RED),   "blit top-left at offset");
    ASSERT(pixel_eq(dst, 11, 4, GFX_RED),   "blit top-right at offset");
    ASSERT(pixel_eq(dst, 4, 11, GFX_RED),   "blit bottom-left at offset");
    ASSERT(pixel_eq(dst, 11,11, GFX_RED),   "blit bottom-right at offset");
    ASSERT(pixel_eq(dst, 3,  4, GFX_BLACK), "left of blit untouched");
    ASSERT(pixel_eq(dst, 4,  3, GFX_BLACK), "above blit untouched");

    gfx_fill(dst, GFX_BLACK);

    /* Blit only a 4×4 sub-rect of src. */
    gfx_fill(src, GFX_GREEN);
    gfx_blit(dst, 0, 0, src, (rect_t){2, 2, 4, 4});
    ASSERT_EQ(count_pixels(dst, GFX_GREEN), 16, "blit sub-rect copies 4x4 = 16 pixels");

    gfx_fill(dst, GFX_BLACK);

    /* Blit partly outside dst bounds — clipped. */
    gfx_fill(src, GFX_BLUE);
    gfx_blit(dst, 12, 12, src, (rect_t){0, 0, 8, 8});
    /* Only the 4×4 overlap is inside dst. */
    ASSERT_EQ(count_pixels(dst, GFX_BLUE), 16, "blit clipped at dst boundary = 16 pixels");

    canvas_free(src);
    canvas_free(dst);
}

/* ── gfx_draw_text ──────────────────────────────────────────────────────── */

static void test_gfx_draw_text(void)
{
    /* Each character cell is GFX_CHAR_W × GFX_CHAR_H pixels. */
    ASSERT_EQ(GFX_CHAR_W, 8,  "GFX_CHAR_W == 8");
    ASSERT_EQ(GFX_CHAR_H, 16, "GFX_CHAR_H == 16");

    /* Allocate a canvas wide enough for one character. */
    canvas_t *c = canvas_create(GFX_CHAR_W, GFX_CHAR_H);
    gfx_fill(c, GFX_BLACK);

    /* Draw space (0x20) — all pixels should be background (black). */
    gfx_draw_text(c, 0, 0, " ", GFX_WHITE, GFX_BLACK);
    ASSERT_EQ(count_pixels(c, GFX_WHITE), 0, "space char has no fg pixels");

    /* Draw '!' (0x21) — glyph row 0 is 0x18 = 0b00011000.
     * Fg pixels in row 0: columns 3 and 4 (0-based from left).           */
    gfx_fill(c, GFX_BLACK);
    gfx_draw_text(c, 0, 0, "!", GFX_WHITE, GFX_BLACK);
    ASSERT(pixel_eq(c, 3, 0, GFX_WHITE), "'!' row0 col3 is fg (bit 4 of 0x18)");
    ASSERT(pixel_eq(c, 4, 0, GFX_WHITE), "'!' row0 col4 is fg (bit 3 of 0x18)");
    ASSERT(pixel_eq(c, 0, 0, GFX_BLACK), "'!' row0 col0 is bg");
    ASSERT(pixel_eq(c, 7, 0, GFX_BLACK), "'!' row0 col7 is bg");

    /* Lower 8 rows of a 16-row cell are always background. */
    ASSERT(pixel_eq(c, 3, 8,  GFX_BLACK), "lower half row 8 is always bg");
    ASSERT(pixel_eq(c, 3, 15, GFX_BLACK), "lower half row 15 is always bg");

    /* Multi-character string advances x by GFX_CHAR_W per character. */
    canvas_t *wide = canvas_create(GFX_CHAR_W * 3, GFX_CHAR_H);
    gfx_fill(wide, GFX_BLACK);
    gfx_draw_text(wide, 0, 0, "   ", GFX_WHITE, GFX_BLACK);  /* 3 spaces */
    ASSERT_EQ(count_pixels(wide, GFX_WHITE), 0, "three spaces produce no fg pixels");
    canvas_free(wide);

    /* draw_text clipped — characters outside canvas are not drawn. */
    gfx_fill(c, GFX_BLACK);
    gfx_draw_text(c, 100, 100, "A", GFX_WHITE, GFX_BLACK);
    ASSERT_EQ(count_pixels(c, GFX_WHITE), 0, "text far outside canvas produces no pixels");

    canvas_free(c);
}

/* ── gfx_info_t layout ──────────────────────────────────────────────────── */

static void test_gfx_info_struct(void)
{
    /* Verify the struct matches the expected field order / sizes that the
     * kernel fills in SYS_GFX_INFO.  If field sizes or order change, the
     * kernel handler must be updated to match.                             */
    gfx_info_t info = {800, 600, 3200, 32};
    ASSERT_EQ((int)info.width,  800,  "gfx_info_t.width  readable");
    ASSERT_EQ((int)info.height, 600,  "gfx_info_t.height readable");
    ASSERT_EQ((int)info.pitch,  3200, "gfx_info_t.pitch  readable");
    ASSERT_EQ((int)info.bpp,    32,   "gfx_info_t.bpp    readable");

    /* pitch == width * (bpp/8) for a tightly-packed 32 bpp framebuffer. */
    ASSERT_EQ((int)(info.width * (info.bpp / 8)), (int)info.pitch,
              "pitch == width * bytes_per_pixel");
}

/* ── gfx_fill on NULL canvas is safe ───────────────────────────────────── */

static void test_null_safety(void)
{
    /* None of these should crash. */
    gfx_fill(NULL, GFX_RED);
    gfx_fill_rect(NULL, (rect_t){0,0,10,10}, GFX_RED);
    gfx_draw_rect(NULL, (rect_t){0,0,10,10}, GFX_RED);
    gfx_blit(NULL, 0, 0, NULL, (rect_t){0,0,1,1});
    gfx_draw_text(NULL, 0, 0, "hi", GFX_WHITE, GFX_BLACK);
    gfx_set_clip(NULL, (rect_t){0,0,1,1});
    gfx_clear_clip(NULL);
    ASSERT(1, "all drawing functions tolerate NULL canvas");
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    RUN_SUITE(test_color_macros);
    RUN_SUITE(test_canvas_create);
    RUN_SUITE(test_gfx_fill);
    RUN_SUITE(test_gfx_fill_rect_basic);
    RUN_SUITE(test_gfx_fill_rect_clipping);
    RUN_SUITE(test_gfx_draw_rect);
    RUN_SUITE(test_clip_rect);
    RUN_SUITE(test_gfx_blit);
    RUN_SUITE(test_gfx_draw_text);
    RUN_SUITE(test_gfx_info_struct);
    RUN_SUITE(test_null_safety);
    TEST_SUMMARY();
}
