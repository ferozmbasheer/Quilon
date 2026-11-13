/*
 * test_widgets.c -- host tests for the WM widget kit and the 18.5 app logic
 *                   (Quilon OS ROADMAP3 18.5)
 *
 *   widgets.h   scrollbar geometry/hit/drag, list navigation, the scroll
 *               list's mouse protocol (wheel, arrows, paging, thumb drag,
 *               select, double-click activate), text-input editing
 *   widgets.c   drawing: thumb + selection pixels land where the logic says
 *   fb_path.h   Files: path join/parent, open kind, listing order
 *   bmp.h       wallpaper: BMP header parsing, pixels, centre vs tile
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "framework.h"

/* The host libc's; -I../libc/include shadows <stdlib.h>. */
void *malloc(size_t n);
void  free(void *p);

#include <gfx.h>
#include "../user/wm/wm.h"
#include "../user/wm/widgets.h"
#include "../user/wm/bmp.h"
#include "../user/filebr/fb_path.h"

static wm_event_t mouse_ev(wm_ev_type_t type, int x, int y, int clicks)
{
    wm_event_t ev = wm_event_make(type);
    ev.x = x; ev.y = y;
    ev.button = MOUSE_BTN_LEFT;
    ev.clicks = (uint8_t)clicks;
    return ev;
}

/* ── Scrollbar ──────────────────────────────────────────────────────────── */

static void test_vscroll_geometry(void)
{
    wg_vscroll_t sb = { { 100, 0, WG_SB_W, 214 }, 100, 10, 0 };
    rect_t t = wg_vscroll_track(&sb);
    ASSERT_EQ(t.y, WG_SB_W, "track starts under the up arrow");
    ASSERT_EQ(t.h, 214 - 2 * WG_SB_W, "track ends over the down arrow");

    rect_t th = wg_vscroll_thumb(&sb);
    ASSERT_EQ(th.y, t.y, "pos 0: thumb at the top");
    ASSERT_EQ(th.h, t.h * 10 / 100, "thumb height ~ visible/total");

    sb.pos = wg_vscroll_max(&sb);
    th = wg_vscroll_thumb(&sb);
    ASSERT_EQ(th.y + th.h, t.y + t.h, "pos max: thumb at the bottom");

    sb.total = 10000;                      /* tiny ratio: minimum thumb */
    sb.pos = 0;
    ASSERT_EQ(wg_vscroll_thumb(&sb).h, WG_SB_MIN, "thumb never under WG_SB_MIN");

    sb.total = 5;                          /* all visible */
    th = wg_vscroll_thumb(&sb);
    ASSERT(th.y == t.y && th.h == t.h, "everything visible: thumb = track");
    ASSERT_EQ(wg_vscroll_hit(&sb, 105, t.y + 5), WG_SB_NONE,
              "nothing to scroll: track does nothing");

    wg_vscroll_t tiny = { { 0, 0, WG_SB_W, 10 }, 50, 5, 0 };   /* very short */
    ASSERT_EQ(wg_vscroll_track(&tiny).h, 0, "arrows share a short bar");
}

static void test_vscroll_hit_and_drag(void)
{
    wg_vscroll_t sb = { { 0, 0, WG_SB_W, 214 }, 100, 10, 45 };
    rect_t th = wg_vscroll_thumb(&sb);
    ASSERT_EQ(wg_vscroll_hit(&sb, 3, 2), WG_SB_UP, "up arrow");
    ASSERT_EQ(wg_vscroll_hit(&sb, 3, 210), WG_SB_DOWN, "down arrow");
    ASSERT_EQ(wg_vscroll_hit(&sb, 3, th.y - 1), WG_SB_PGUP, "above thumb");
    ASSERT_EQ(wg_vscroll_hit(&sb, 3, th.y + th.h), WG_SB_PGDN, "below thumb");
    ASSERT_EQ(wg_vscroll_hit(&sb, 3, th.y + 1), WG_SB_THUMB, "thumb");
    ASSERT_EQ(wg_vscroll_hit(&sb, WG_SB_W, 100), WG_SB_NONE, "outside");

    /* pos_at inverts thumb placement for every pos. */
    int ok = 1;
    for (int p = 0; p <= wg_vscroll_max(&sb); p++) {
        sb.pos = p;
        if (wg_vscroll_pos_at(&sb, wg_vscroll_thumb(&sb).y) != p) ok = 0;
    }
    ASSERT(ok, "pos_at(thumb(pos).y) == pos for all pos");
    ASSERT_EQ(wg_vscroll_pos_at(&sb, -500), 0, "drag above clamps to 0");
    ASSERT_EQ(wg_vscroll_pos_at(&sb, 5000), 90, "drag below clamps to max");
}

/* ── List view ──────────────────────────────────────────────────────────── */

static void test_list_nav(void)
{
    wg_list_t l = { { 0, 20, 100, 160 }, 50, 0, 0, 16 };   /* 10 rows */
    ASSERT_EQ(wg_list_rows(&l), 10, "rows = h / row_h");
    ASSERT_EQ(wg_list_hit(&l, 5, 20), 0, "first row");
    ASSERT_EQ(wg_list_hit(&l, 5, 20 + 16 * 3 + 2), 3, "fourth row");
    ASSERT_EQ(wg_list_hit(&l, 5, 19), -1, "above the list");

    wg_list_key(&l, WM_KEY_DOWN);
    ASSERT_EQ(l.sel, 1, "Down moves the selection");
    wg_list_key(&l, WM_KEY_PGDN);
    ASSERT_EQ(l.sel, 10, "PgDn moves a page (rows - 1)");
    ASSERT_EQ(l.top, 1, "selection kept in view");
    wg_list_key(&l, WM_KEY_END);
    ASSERT(l.sel == 49 && l.top == 40, "End: last row, bottom page");
    wg_list_key(&l, WM_KEY_HOME);
    ASSERT(l.sel == 0 && l.top == 0, "Home: first row");
    ASSERT(!wg_list_key(&l, 'x'), "other keys unused");

    l.n = 3;                               /* short list: hit below = -1 */
    ASSERT_EQ(wg_list_hit(&l, 5, 20 + 16 * 5), -1, "below the last row");

    wg_list_t v = { { 0, 0, 100, 160 }, 50, 0, -1, 16 };   /* no selection */
    wg_list_key(&v, WM_KEY_DOWN);
    ASSERT(v.top == 1 && v.sel == -1, "no selection: keys scroll");
    wg_list_key(&v, WM_KEY_END);
    ASSERT_EQ(v.top, 40, "End scrolls to the last page");

    v.r.h = 1600;                          /* grew taller than the list */
    wg_list_clamp(&v);
    ASSERT_EQ(v.top, 0, "clamp after resize");
}

static void test_slist_mouse(void)
{
    wg_slist_t s;
    memset(&s, 0, sizeof s);
    s.list.n = 40;
    s.list.row_h = 16;
    s.list.sel = 0;
    wg_slist_layout(&s, (rect_t){ 0, 0, 200, 160 });      /* 10 rows */
    ASSERT_EQ(s.list.r.w, 200 - WG_SB_W, "list leaves room for the bar");
    ASSERT_EQ(s.sb.r.x, 200 - WG_SB_W, "bar on the right");

    wm_event_t ev = wm_event_make(WM_EV_SCROLL);
    ev.wheel = 2;
    ASSERT_EQ(wg_slist_mouse(&s, &ev, 1), WG_SL_REDRAW, "wheel scrolls");
    ASSERT_EQ(s.list.top, 2 * WM_WHEEL_ROWS, "by WM_WHEEL_ROWS a notch");
    ev.wheel = -100;
    wg_slist_mouse(&s, &ev, 1);
    ASSERT_EQ(s.list.top, 0, "wheel clamps at the top");
    ASSERT_EQ(wg_slist_mouse(&s, &ev, 1), WG_SL_NONE, "no change: no redraw");

    ev = mouse_ev(WM_EV_MOUSE_BTN, 200 - 5, 155, 1);    /* down arrow */
    ASSERT_EQ(wg_slist_mouse(&s, &ev, 1), WG_SL_REDRAW, "arrow scrolls");
    ASSERT_EQ(s.list.top, 1, "a row");
    ev = mouse_ev(WM_EV_MOUSE_BTN, 200 - 5, 140, 1);    /* track below thumb */
    wg_slist_mouse(&s, &ev, 1);
    ASSERT_EQ(s.list.top, 10, "track pages (rows - 1)");

    /* Thumb drag: press on the thumb, move down, release. */
    wg_slist_sync(&s);
    rect_t th = wg_vscroll_thumb(&s.sb);
    ev = mouse_ev(WM_EV_MOUSE_BTN, 200 - 5, th.y + 2, 1);
    wg_slist_mouse(&s, &ev, 1);
    ASSERT(s.drag, "thumb press starts a drag");
    ev = mouse_ev(WM_EV_MOUSE_MOVE, 50, 1000, 0);        /* way off the bar */
    wg_slist_mouse(&s, &ev, 1);
    ASSERT_EQ(s.list.top, 30, "dragging to the bottom = last page");
    ev = mouse_ev(WM_EV_MOUSE_UP, 50, 1000, 0);
    ASSERT_EQ(wg_slist_mouse(&s, &ev, 1), WG_SL_REDRAW, "release redraws");
    ASSERT(!s.drag, "drag over");
    ev = mouse_ev(WM_EV_MOUSE_MOVE, 50, 0, 0);
    ASSERT_EQ(wg_slist_mouse(&s, &ev, 1), WG_SL_NONE, "moves after: ignored");

    /* Rows: click selects, double-click on the selection activates. */
    ev = mouse_ev(WM_EV_MOUSE_BTN, 20, 16 * 2 + 3, 1);
    ASSERT_EQ(wg_slist_mouse(&s, &ev, 1), WG_SL_SELECT, "click selects");
    ASSERT_EQ(s.list.sel, 32, "row under the cursor (top 30 + 2)");
    ev.clicks = 2;
    ASSERT_EQ(wg_slist_mouse(&s, &ev, 1), WG_SL_ACTIVATE, "double-click opens");
    ev = mouse_ev(WM_EV_MOUSE_BTN, 20, 16 * 3 + 3, 2);   /* other row */
    ASSERT_EQ(wg_slist_mouse(&s, &ev, 1), WG_SL_SELECT,
              "a double-click landing on another row only selects it");
    ASSERT_EQ(wg_slist_mouse(&s, &ev, 0), WG_SL_NONE, "not selectable: no-op");
    ev.button = 0x02;                      /* right */
    ASSERT_EQ(wg_slist_mouse(&s, &ev, 1), WG_SL_NONE, "right button ignored");
}

/* ── Text input ─────────────────────────────────────────────────────────── */

static void test_input_edit(void)
{
    wg_input_t in;
    wg_input_set(&in, "/abc");
    ASSERT(in.len == 4 && in.cur == 4, "set: cursor at the end");
    ASSERT_EQ(wg_input_key(&in, 'd', 'd'), WG_IN_CHANGED, "typing");
    ASSERT_STR_EQ(in.buf, "/abcd", "appended");
    wg_input_key(&in, WM_KEY_HOME, 0);
    wg_input_key(&in, WM_KEY_RIGHT, 0);
    wg_input_key(&in, 'x', 'x');
    ASSERT_STR_EQ(in.buf, "/xabcd", "inserted at the cursor");
    wg_input_key(&in, WM_KEY_BACKSPACE, 0x7F);
    ASSERT_STR_EQ(in.buf, "/abcd", "backspace before the cursor");
    wg_input_key(&in, WM_KEY_DELETE, 0);
    ASSERT_STR_EQ(in.buf, "/bcd", "delete at the cursor");
    ASSERT_EQ(in.cur, 1, "cursor stays");
    wg_input_key(&in, WM_KEY_HOME, 0);
    ASSERT_EQ(wg_input_key(&in, WM_KEY_BACKSPACE, 0x7F), WG_IN_NONE,
              "backspace at 0: nothing");
    ASSERT_EQ(wg_input_key(&in, WM_KEY_ENTER, '\n'), WG_IN_ENTER, "Enter");
    ASSERT_EQ(wg_input_key(&in, WM_KEY_ESC, 0x1B), WG_IN_CANCEL, "Esc");
    ASSERT_EQ(wg_input_key(&in, 'c', 0x03), WG_IN_NONE, "control chars ignored");
    ASSERT_EQ(in.len, (int)strlen(in.buf), "len matches");

    char longs[WG_INPUT_MAX + 10];
    memset(longs, 'a', sizeof longs - 1);
    longs[sizeof longs - 1] = '\0';
    wg_input_set(&in, longs);
    ASSERT_EQ(in.len, WG_INPUT_MAX - 1, "set truncates");
    ASSERT_EQ(wg_input_key(&in, 'b', 'b'), WG_IN_NONE, "full: no insert");

    wg_input_set(&in, "0123456789");
    wg_input_fit(&in, 4);
    ASSERT_EQ(in.scroll, 7, "scrolled so the end cursor shows");
    in.cur = 2;
    wg_input_fit(&in, 4);
    ASSERT_EQ(in.scroll, 2, "scrolled back to the cursor");
}

/* ── Drawing ────────────────────────────────────────────────────────────── */

static const char *row_text(void *ctx, int i, color_t *fg)
{
    (void)ctx; (void)i;
    *fg = WG_TEXT;
    return "row";
}

static void test_widget_drawing(void)
{
    canvas_t *c = canvas_create(200, 160);
    wg_slist_t s;
    memset(&s, 0, sizeof s);
    s.list.n = 40; s.list.row_h = 16; s.list.sel = 3; s.list.top = 0;
    wg_slist_layout(&s, (rect_t){ 0, 0, 200, 160 });
    wg_slist_draw(c, &s, GFX_BLACK, row_text, 0);

    rect_t th = wg_vscroll_thumb(&s.sb);
    rect_t tr = wg_vscroll_track(&s.sb);
    ASSERT_EQ(c->pixels[(th.y + th.h / 2) * c->pitch + th.x + th.w / 2],
              WG_FACE, "thumb face");
    ASSERT_EQ(c->pixels[(tr.y + tr.h - 2) * c->pitch + tr.x + tr.w / 2],
              WG_TRACK, "track below the thumb");
    ASSERT_EQ(c->pixels[(3 * 16 + 1) * c->pitch + 150], WG_SEL_BG,
              "selected row highlighted");
    ASSERT_EQ(c->pixels[(2 * 16 + 1) * c->pitch + 150], GFX_BLACK,
              "other rows plain");
    ASSERT(!c->has_clip, "list drawing leaves no clip behind");

    wg_input_t in;
    wg_input_set(&in, "ab");
    gfx_fill(c, GFX_BLACK);
    wg_input_draw(c, (rect_t){ 10, 10, 100, 20 }, &in, 1);
    ASSERT_EQ(c->pixels[20 * c->pitch + 50], WG_FIELD_BG, "field background");
    int cx = 10 + 4 + 2 * GFX_CHAR_W;
    ASSERT_EQ(c->pixels[18 * c->pitch + cx], WG_TEXT, "cursor after 'ab'");
    canvas_free(c);
}

/* ── Files path logic ───────────────────────────────────────────────────── */

static void test_fb_paths(void)
{
    char out[FB_PATH_MAX];
    fb_path_join("/", "README.TXT", out);
    ASSERT_STR_EQ(out, "/README.TXT", "root + name");
    fb_path_join("/a/b", "c", out);
    ASSERT_STR_EQ(out, "/a/b/c", "dir + name");
    fb_path_join("/a/b", "..", out);
    ASSERT_STR_EQ(out, "/a", "..");
    fb_path_join("/a", "..", out);
    ASSERT_STR_EQ(out, "/", ".. to the root");
    fb_path_join("/", "..", out);
    ASSERT_STR_EQ(out, "/", ".. at the root stays");
    fb_path_join("/a/b", "/x//y/", out);
    ASSERT_STR_EQ(out, "/x/y", "absolute, slashes squeezed");
    fb_path_join("/a", "./b/../c", out);
    ASSERT_STR_EQ(out, "/a/c", ". and .. inside");
    fb_path_join("/a", "", out);
    ASSERT_STR_EQ(out, "/a", "empty name: the dir");

    char longname[FB_PATH_MAX + 4];
    memset(longname, 'x', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    memcpy(out, "untouched", 10);
    ASSERT_EQ(fb_path_join("/", longname, out), -1, "too long fails");
    ASSERT_STR_EQ(out, "untouched", "out unchanged on failure");

    char p[FB_PATH_MAX] = "/a/b/";
    fb_parent(p);
    ASSERT_STR_EQ(p, "/a", "parent ignores a trailing slash");

    ASSERT_EQ(fb_open_kind("SUB", 1), FB_OPEN_DIR, "dir");
    ASSERT_EQ(fb_open_kind("HELLO.ELF", 0), FB_OPEN_PROG, ".ELF runs");
    ASSERT_EQ(fb_open_kind("wget.elf", 0), FB_OPEN_PROG, ".elf runs");
    ASSERT_EQ(fb_open_kind("README.TXT", 0), FB_OPEN_TEXT, "text");
    ASSERT_EQ(fb_open_kind(".elf", 0), FB_OPEN_TEXT, "bare .elf is a name");
}

static void test_fb_sort(void)
{
    fb_entry_t e[5] = {
        { "zeta.txt", 0 }, { "Beta", 1 }, { "..", 1 }, { "alpha.elf", 0 },
        { "ALPHA", 1 },
    };
    fb_sort(e, 5);
    ASSERT_STR_EQ(e[0].name, "..", ".. first");
    ASSERT_STR_EQ(e[1].name, "ALPHA", "then dirs A-Z (case-insensitive)");
    ASSERT_STR_EQ(e[2].name, "Beta", "dirs");
    ASSERT_STR_EQ(e[3].name, "alpha.elf", "then files");
    ASSERT_STR_EQ(e[4].name, "zeta.txt", "files A-Z");
}

/* ── BMP wallpaper ──────────────────────────────────────────────────────── */

/* A w x h BMP, `bpp` 24/32, pixel (x, y) = colour f(x, y). */
static uint8_t *make_bmp(int w, int h, int bpp, int top_down, uint32_t *len)
{
    uint32_t stride = ((uint32_t)w * (bpp / 8) + 3u) & ~3u;
    uint32_t off = 54, size = off + stride * (uint32_t)h;
    uint8_t *d = malloc(size);
    memset(d, 0, size);
    d[0] = 'B'; d[1] = 'M';
    #define PUT32(o, v) do { uint32_t v_ = (v); d[o] = v_; d[o+1] = v_ >> 8; \
                             d[o+2] = v_ >> 16; d[o+3] = v_ >> 24; } while (0)
    PUT32(2, size); PUT32(10, off); PUT32(14, 40);
    PUT32(18, (uint32_t)w); PUT32(22, (uint32_t)(top_down ? -h : h));
    d[26] = 1; d[28] = (uint8_t)bpp;
    for (int y = 0; y < h; y++) {
        int row = top_down ? y : h - 1 - y;
        for (int x = 0; x < w; x++) {
            uint8_t *p = d + off + (uint32_t)row * stride + (uint32_t)x * (bpp / 8);
            p[0] = (uint8_t)(x * 10);        /* B */
            p[1] = (uint8_t)(y * 10);        /* G */
            p[2] = 0x80;                     /* R */
        }
    }
    *len = size;
    return d;
}

static uint32_t want_px(int x, int y)
{
    return 0x800000u | ((uint32_t)(uint8_t)(y * 10) << 8) | (uint8_t)(x * 10);
}

static void test_bmp_parse(void)
{
    uint32_t len;
    bmp_info_t b;
    uint8_t *d = make_bmp(5, 3, 24, 0, &len);
    ASSERT_EQ(bmp_parse(d, len, &b), 0, "24-bit bottom-up parses");
    ASSERT(b.w == 5 && b.h == 3 && b.stride == 16, "size + padded stride");
    ASSERT_EQ(bmp_pixel(d, &b, 0, 0), want_px(0, 0), "top-left");
    ASSERT_EQ(bmp_pixel(d, &b, 4, 2), want_px(4, 2), "bottom-right");
    ASSERT_EQ(bmp_parse(d, len - 1, &b), -1, "truncated pixels rejected");
    d[30] = 1;                                       /* RLE8 */
    ASSERT_EQ(bmp_parse(d, len, &b), -1, "compressed rejected");
    free(d);

    d = make_bmp(4, 4, 32, 1, &len);
    ASSERT_EQ(bmp_parse(d, len, &b), 0, "32-bit top-down parses");
    ASSERT_EQ(bmp_pixel(d, &b, 3, 1), want_px(3, 1), "top-down pixel");
    d[0] = 'P';
    ASSERT_EQ(bmp_parse(d, len, &b), -1, "bad magic rejected");
    free(d);

    d = make_bmp(4, 4, 8, 0, &len);
    ASSERT_EQ(bmp_parse(d, len, &b), -1, "8-bit (palette) rejected");
    free(d);
    ASSERT_EQ(bmp_parse((const uint8_t *)"BM", 2, &b), -1, "tiny rejected");
}

static void test_wallpaper_render(void)
{
    uint32_t len;
    bmp_info_t b;

    /* Small (<= BMP_TILE_MAX): tiled. */
    canvas_t *bg = canvas_create(20, 10);
    uint8_t *d = make_bmp(3, 2, 24, 0, &len);
    bmp_parse(d, len, &b);
    wm_wallpaper_render(bg, d, &b, 0x123456);
    ASSERT_EQ(bg->pixels[0], want_px(0, 0), "tile origin");
    ASSERT_EQ(bg->pixels[5 * bg->pitch + 7], want_px(1, 1), "tile repeats");
    free(d);
    canvas_free(bg);

    /* Large: scaled to cover.  200x130 -> 400x260 is exactly 2x. */
    d = make_bmp(200, 130, 24, 0, &len);
    bmp_parse(d, len, &b);
    bg = canvas_create(400, 260);
    wm_wallpaper_render(bg, d, &b, 0x123456);
    ASSERT_EQ(bg->pixels[0], want_px(0, 0), "top-left = image top-left");
    ASSERT_EQ(bg->pixels[259 * bg->pitch + 399], want_px(199, 129),
              "bottom-right = image bottom-right");
    int red_ok = 1;
    for (int i = 0; i < 400 * 260; i += 97)
        if ((bg->pixels[i] >> 16) != 0x80) red_ok = 0;
    ASSERT(red_ok, "a constant channel stays constant through the blend");
    /* Between source pixels (0,0) and (1,0): blue 0 and 10 -> in between. */
    uint32_t mid = bg->pixels[2] & 0xFF;
    ASSERT(mid > 0 && mid < 10, "bilinear: a blend between neighbours");
    canvas_free(bg);

    /* Wider than the image's aspect: cover crops top and bottom. 200x130 ->
     * 400x100: scale 2 (by width), 260 tall, centred at y -80, so screen
     * row 0 samples image row 39.75 -- between rows 39 and 40. */
    bg = canvas_create(400, 100);
    wm_wallpaper_render(bg, d, &b, 0x123456);
    uint32_t g = (bg->pixels[0] >> 8) & 0xFF;
    uint32_t g39 = (want_px(0, 39) >> 8) & 0xFF, g40 = (want_px(0, 40) >> 8) & 0xFF;
    ASSERT(g > g39 && g < g40, "cropped: top row comes from image rows 39-40");
    int filled = 1;
    for (int i = 0; i < 400 * 100; i++) if (bg->pixels[i] == 0x123456u) filled = 0;
    ASSERT(filled, "cover leaves no desktop colour");
    canvas_free(bg);
    free(d);
}

int main(void)
{
    RUN_SUITE(test_vscroll_geometry);
    RUN_SUITE(test_vscroll_hit_and_drag);
    RUN_SUITE(test_list_nav);
    RUN_SUITE(test_slist_mouse);
    RUN_SUITE(test_input_edit);
    RUN_SUITE(test_widget_drawing);
    RUN_SUITE(test_fb_paths);
    RUN_SUITE(test_fb_sort);
    RUN_SUITE(test_bmp_parse);
    RUN_SUITE(test_wallpaper_render);
    TEST_SUMMARY();
}
