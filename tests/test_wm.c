/*
 * test_wm.c -- host-side unit tests for the Window Manager & Compositor
 *              (Quilon OS section 14.2)
 *
 * Compiled on the host with native gcc; no cross-compiler, no QEMU.
 *
 * What is tested
 * ──────────────
 *   Geometry helpers
 *     - wm_rect_contains          -- point-in-rect
 *     - wm_titlebar_rect          -- position relative to content area
 *     - wm_close_btn_rect         -- right-aligned, centred in title bar
 *     - wm_full_rect              -- title bar + content area
 *     - wm_in_titlebar / _close_btn / _content / _window
 *
 *   State management
 *     - wm_state_init             -- zeroed counters, screen size stored
 *     - wm_create_window          -- IDs increment, clamped min size
 *     - wm_find_window / idx      -- lookup by ID
 *     - wm_destroy_window         -- slot compacted; focus transferred
 *     - table full (WM_MAX_WINDOWS) -- 17th create returns -1
 *
 *   Z-order & focus
 *     - wm_raise                  -- window moves to front; hittest agrees
 *     - wm_focus                  -- only the focused window has focused=1
 *
 *   Mouse events
 *     - wm_handle_mouse_down      -- raise + focus; drag_win_id set in titlebar
 *     - wm_handle_mouse_move      -- dragged window position updated
 *     - wm_handle_mouse_up        -- drag ends
 *     - close-button click        -- window destroyed
 *     - click on desktop (no window) -- no crash, no state change
 *
 *   Compositor pixel output
 *     - wm_composite draws desktop background
 *     - title bar pixels match focused/unfocused colours
 *     - close button region is red
 *     - content area of a window with a known backbuf is blitted correctly
 *     - cursor pixels appear at the right location
 *     - wm_draw_cursor writes white pixels at the sprite positions
 */

#include <stddef.h>
#include <stdlib.h>   /* malloc / free from host libc */
#include <string.h>
#include "framework.h"

/* Include gfx.h via the include path supplied in the Makefile rule. */
#include <gfx.h>
#include "../user/wm/wm.h"
#include "../user/wm/wm_keys.h"

/* ── helpers ─────────────────────────────────────────────────────────────── */

static color_t pixel_at(const canvas_t *c, int x, int y)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return 0xDEADBEEFu;
    return c->pixels[y * c->pitch + x];
}

static int count_col(const canvas_t *c, color_t col)
{
    int n = 0;
    int x, y;
    for (y = 0; y < c->h; y++)
        for (x = 0; x < c->w; x++)
            if (c->pixels[y * c->pitch + x] == col) n++;
    return n;
}

static int any_col_in_rect(const canvas_t *c, rect_t r, color_t col)
{
    int x, y;
    for (y = r.y; y < r.y + r.h; y++)
        for (x = r.x; x < r.x + r.w; x++)
            if (pixel_at(c, x, y) == col) return 1;
    return 0;
}

/* (unused in current suite — kept for future tests) */
static int all_col_in_rect(const canvas_t *c, rect_t r, color_t col)
    __attribute__((unused));
static int all_col_in_rect(const canvas_t *c, rect_t r, color_t col)
{
    int x, y;
    for (y = r.y; y < r.y + r.h; y++)
        for (x = r.x; x < r.x + r.w; x++)
            if (pixel_at(c, x, y) != col) return 0;
    return 1;
}

/* Build a minimal wm_state_t with one window at (100, 100+TITLEBAR_H) 200×100. */
static wm_state_t make_single_window(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);
    wm_create_window(&s, "Test", 100, 100 + TITLEBAR_H, 200, 100);
    return s;
}

/* ======================================================================== */

static void test_geometry_rect_contains(void)
{
    rect_t r = {10, 20, 30, 40};  /* x=10,y=20,w=30,h=40 → x:[10,40), y:[20,60) */

    ASSERT_EQ(wm_rect_contains(r, 10, 20), 1,  "top-left corner is inside");
    ASSERT_EQ(wm_rect_contains(r, 39, 59), 1,  "bottom-right corner (exclusive) inside");
    ASSERT_EQ(wm_rect_contains(r, 40, 60), 0,  "one past bottom-right is outside");
    ASSERT_EQ(wm_rect_contains(r,  9, 20), 0,  "left of rect is outside");
    ASSERT_EQ(wm_rect_contains(r, 10, 19), 0,  "above rect is outside");
    ASSERT_EQ(wm_rect_contains(r, 25, 40), 1,  "centre is inside");
    ASSERT_EQ(wm_rect_contains(r,  0,  0), 0,  "origin outside");
}

static void test_geometry_titlebar(void)
{
    window_t w;
    w.bounds.x = 50; w.bounds.y = 150; w.bounds.w = 200; w.bounds.h = 100;

    rect_t tb = wm_titlebar_rect(&w);
    ASSERT_EQ(tb.x, 50,           "titlebar x = window x");
    ASSERT_EQ(tb.y, 150-TITLEBAR_H, "titlebar y = content_y - TITLEBAR_H");
    ASSERT_EQ(tb.w, 200,          "titlebar width = window width");
    ASSERT_EQ(tb.h, TITLEBAR_H,   "titlebar height = TITLEBAR_H");

    rect_t full = wm_full_rect(&w);
    ASSERT_EQ(full.x, 50,                      "full_rect x");
    ASSERT_EQ(full.y, 150 - TITLEBAR_H,        "full_rect y");
    ASSERT_EQ(full.w, 200,                     "full_rect width");
    ASSERT_EQ(full.h, TITLEBAR_H + 100,        "full_rect height = titlebar + content");
}

static void test_geometry_close_btn(void)
{
    window_t w;
    w.bounds.x = 50; w.bounds.y = 150; w.bounds.w = 200; w.bounds.h = 100;

    rect_t cb = wm_close_btn_rect(&w);
    rect_t tb = wm_titlebar_rect(&w);

    /* Close button must be within the title bar. */
    ASSERT(cb.x >= tb.x,            "close btn x >= titlebar x");
    ASSERT(cb.x + cb.w <= tb.x + tb.w, "close btn right edge <= titlebar right");
    ASSERT(cb.y >= tb.y,            "close btn y >= titlebar y");
    ASSERT(cb.y + cb.h <= tb.y + tb.h, "close btn bottom <= titlebar bottom");
    ASSERT_EQ(cb.w, WM_CLOSE_BTN_W, "close btn width");
    ASSERT_EQ(cb.h, WM_CLOSE_BTN_H, "close btn height");

    /* It should be right-aligned (right edge near the title bar's right). */
    ASSERT(cb.x + cb.w > tb.x + tb.w - WM_CLOSE_BTN_W - 4,
           "close btn is right-aligned");
}

static void test_geometry_hit_regions(void)
{
    window_t w;
    w.bounds.x = 100; w.bounds.y = 120; w.bounds.w = 200; w.bounds.h = 100;

    /* A point well inside the title bar. */
    int tb_mid_y = 120 - TITLEBAR_H + TITLEBAR_H / 2;
    ASSERT_EQ(wm_in_titlebar(&w, 150, tb_mid_y), 1, "midpoint of titlebar is in titlebar");
    ASSERT_EQ(wm_in_content(&w,  150, tb_mid_y), 0, "titlebar midpoint not in content");
    ASSERT_EQ(wm_in_window(&w,   150, tb_mid_y), 1, "titlebar midpoint in full window");

    /* A point in the content area. */
    ASSERT_EQ(wm_in_titlebar(&w, 150, 130), 0, "content area not in titlebar");
    ASSERT_EQ(wm_in_content(&w,  150, 130), 1, "content area in content");
    ASSERT_EQ(wm_in_window(&w,   150, 130), 1, "content area in full window");

    /* A point outside the window entirely. */
    ASSERT_EQ(wm_in_titlebar(&w,  0, 0), 0, "outside not in titlebar");
    ASSERT_EQ(wm_in_content(&w,   0, 0), 0, "outside not in content");
    ASSERT_EQ(wm_in_window(&w,    0, 0), 0, "outside not in full window");

    /* Close button. */
    rect_t cb = wm_close_btn_rect(&w);
    int cx = cb.x + cb.w / 2;
    int cy = cb.y + cb.h / 2;
    ASSERT_EQ(wm_in_close_btn(&w, cx, cy), 1, "center of close btn is in close btn");
    ASSERT_EQ(wm_in_titlebar(&w,  cx, cy), 1, "close btn center also in titlebar");
}

/* ── State management ────────────────────────────────────────────────────── */

static void test_state_init(void)
{
    wm_state_t s;
    wm_state_init(&s, 1024, 768);

    ASSERT_EQ(s.num_windows, 0,        "no windows after init");
    ASSERT_EQ(s.next_id,     1,        "first ID will be 1");
    ASSERT_EQ(s.screen_w,    1024,     "screen_w stored");
    ASSERT_EQ(s.screen_h,    768,      "screen_h stored");
    ASSERT_EQ(s.mouse_x,     512,      "mouse starts at screen centre x");
    ASSERT_EQ(s.mouse_y,     384,      "mouse starts at screen centre y");
    ASSERT_EQ((int)s.mouse_buttons, 0, "no buttons pressed");
    ASSERT_EQ(s.drag_win_id, -1,       "not dragging");
    ASSERT_NULL(wm_find_window(&s, 0), "ID 0 not found");
    ASSERT_NULL(wm_find_window(&s, 1), "ID 1 not found before any window");
}

static void test_create_window(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);

    int id1 = wm_create_window(&s, "Alpha", 10, 50, 200, 150);
    ASSERT(id1 >= 1,                "first window ID >= 1");
    ASSERT_EQ(s.num_windows, 1,     "one window after first create");

    window_t *w1 = wm_find_window(&s, id1);
    ASSERT_NOTNULL(w1,              "find first window");
    ASSERT_EQ(w1->id, id1,         "id matches");
    ASSERT_EQ(w1->active, 1,       "window is active");
    ASSERT_EQ(w1->bounds.x, 10,    "x stored");
    ASSERT_EQ(w1->bounds.y, 50,    "y stored");
    ASSERT_EQ(w1->bounds.w, 200,   "width stored");
    ASSERT_EQ(w1->bounds.h, 150,   "height stored");
    ASSERT_STR_EQ(w1->title, "Alpha", "title stored");

    int id2 = wm_create_window(&s, "Beta", 0, 0, 100, 80);
    ASSERT_EQ(s.num_windows, 2,    "two windows after second create");
    ASSERT(id2 > id1,              "IDs are monotonically increasing");

    /* Min-size clamping. */
    int id3 = wm_create_window(&s, "Tiny", 0, 0, 1, 1);
    window_t *w3 = wm_find_window(&s, id3);
    ASSERT_NOTNULL(w3,                "tiny window created");
    ASSERT(w3->bounds.w >= WM_MIN_W, "width clamped to WM_MIN_W");
    ASSERT(w3->bounds.h >= WM_MIN_H, "height clamped to WM_MIN_H");

    /* NULL title. */
    int id4 = wm_create_window(&s, (char*)0, 0, 0, 100, 80);
    window_t *w4 = wm_find_window(&s, id4);
    ASSERT_NOTNULL(w4,              "NULL-title window created");
    ASSERT_EQ((int)w4->title[0], 0, "NULL title results in empty string");
}

static void test_destroy_window(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);

    int id1 = wm_create_window(&s, "A", 0, 50, 100, 80);
    int id2 = wm_create_window(&s, "B", 0, 50, 100, 80);
    int id3 = wm_create_window(&s, "C", 0, 50, 100, 80);

    /* Destroy the middle window. */
    wm_destroy_window(&s, id2);
    ASSERT_EQ(s.num_windows, 2,        "two windows remain");
    ASSERT_NULL(wm_find_window(&s, id2), "destroyed window not findable");
    ASSERT_NOTNULL(wm_find_window(&s, id1), "window A still present");
    ASSERT_NOTNULL(wm_find_window(&s, id3), "window C still present");

    /* Destroy the first window. */
    wm_destroy_window(&s, id1);
    ASSERT_EQ(s.num_windows, 1,        "one window remains");
    ASSERT_NOTNULL(wm_find_window(&s, id3), "window C still present after A removed");

    /* Destroy with bogus id -- no crash. */
    wm_destroy_window(&s, 9999);
    ASSERT_EQ(s.num_windows, 1,        "no change for bogus destroy");

    /* Destroy last window. */
    wm_destroy_window(&s, id3);
    ASSERT_EQ(s.num_windows, 0,        "no windows remain");
}

static void test_max_windows(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);

    int i, ids[WM_MAX_WINDOWS];
    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        ids[i] = wm_create_window(&s, "W", 0, 50, 80, 60);
        ASSERT(ids[i] > 0, "window within limit created");
    }
    ASSERT_EQ(s.num_windows, WM_MAX_WINDOWS, "max windows reached");

    int extra = wm_create_window(&s, "Extra", 0, 50, 80, 60);
    ASSERT_EQ(extra, -1, "17th window creation fails");
    ASSERT_EQ(s.num_windows, WM_MAX_WINDOWS, "count unchanged after failed create");
}

/* ── Z-order & focus ─────────────────────────────────────────────────────── */

static void test_hittest_basic(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);

    /* No windows yet. */
    ASSERT_EQ(wm_hittest(&s, 400, 300), -1, "hittest on empty state = -1");

    /* One window at (100, 120) content, 200×100. */
    wm_create_window(&s, "A", 100, 120, 200, 100);
    /* Title bar is at y = 120 - TITLEBAR_H. */
    int tb_y = 120 - TITLEBAR_H + TITLEBAR_H / 2;
    ASSERT(wm_hittest(&s, 150, tb_y) >= 0, "hit in titlebar returns index");
    ASSERT(wm_hittest(&s, 150, 150)  >= 0, "hit in content area returns index");
    ASSERT_EQ(wm_hittest(&s,   0,   0), -1, "miss outside returns -1");
    ASSERT_EQ(wm_hittest(&s, 400, 300), -1, "miss in empty area returns -1");
}

static void test_hittest_stacking(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);

    /* Two overlapping windows: B is created second (on top). */
    int id_a = wm_create_window(&s, "A", 50, 120, 200, 150);
    int id_b = wm_create_window(&s, "B", 80, 140, 200, 150);
    (void)id_b;

    /* In the overlapping region, B should win (higher index). */
    int idx = wm_hittest(&s, 120, 180);
    ASSERT(idx >= 0,                   "hit in overlap returns valid index");
    ASSERT_EQ(s.windows[idx].id, id_b, "B (on top) wins in overlap");

    /* Region only covered by A (left side). */
    idx = wm_hittest(&s, 55, 130);
    ASSERT(idx >= 0,                   "hit in A-only region returns index");
    ASSERT_EQ(s.windows[idx].id, id_a, "A wins where B doesn't cover");
}

static void test_raise(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);

    int id_a = wm_create_window(&s, "A", 50, 120, 200, 150);
    wm_create_window(&s, "B", 80, 140, 200, 150);

    /* B is on top; A is below. Raise A. */
    int idx_a = wm_find_idx(&s, id_a);
    wm_raise(&s, idx_a);

    /* Now A should be at the front of z_order. */
    int top_slot = s.z_order[s.num_windows - 1];
    ASSERT_EQ(s.windows[top_slot].id, id_a, "A is at top after raise");

    /* Hit test should now find A in the overlap. */
    int hit = wm_hittest(&s, 120, 180);
    ASSERT(hit >= 0,                    "overlap still hits something");
    ASSERT_EQ(s.windows[hit].id, id_a, "A wins after raise");

    /* Raising the already-front window is a no-op. */
    int before = wm_find_idx(&s, id_a);
    wm_raise(&s, before);
    top_slot = s.z_order[s.num_windows - 1];
    ASSERT_EQ(s.windows[top_slot].id, id_a, "raising front is no-op");
    ASSERT_EQ(s.num_windows, 2, "num_windows unchanged after raise");
}

static void test_focus(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);

    wm_create_window(&s, "A", 0, 50, 100, 80);
    wm_create_window(&s, "B", 0, 50, 100, 80);
    wm_create_window(&s, "C", 0, 50, 100, 80);

    wm_focus(&s, 1);  /* focus B (slot 1) */
    ASSERT_EQ(s.windows[0].focused, 0, "A not focused");
    ASSERT_EQ(s.windows[1].focused, 1, "B focused");
    ASSERT_EQ(s.windows[2].focused, 0, "C not focused");

    wm_focus(&s, 0);  /* focus A */
    ASSERT_EQ(s.windows[0].focused, 1, "A now focused");
    ASSERT_EQ(s.windows[1].focused, 0, "B lost focus");
    ASSERT_EQ(s.windows[2].focused, 0, "C still not focused");
}

/* ── Focus inheritance on destroy ───────────────────────────────────────── */

static void test_focus_transfer_on_destroy(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);

    wm_create_window(&s, "A", 0, 50, 100, 80);
    int id_b = wm_create_window(&s, "B", 0, 50, 100, 80);
    int id_c = wm_create_window(&s, "C", 0, 50, 100, 80);

    /* Give focus to C (top). */
    wm_focus(&s, 2);
    ASSERT_EQ(s.windows[2].focused, 1, "C has focus before destroy");

    /* Destroy C — focus should transfer to B (new top). */
    wm_destroy_window(&s, id_c);
    window_t *b = wm_find_window(&s, id_b);
    ASSERT_NOTNULL(b, "B still exists");
    ASSERT_EQ(b->focused, 1, "B gets focus after C destroyed");
}

/* ── Mouse events ────────────────────────────────────────────────────────── */

static void test_mouse_down_raise_focus(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);

    /* A is below B (created first). */
    int id_a = wm_create_window(&s, "A", 50, 120, 200, 150);
    int id_b = wm_create_window(&s, "B", 80, 140, 200, 150);
    (void)id_b;

    /* Click on A's title bar (outside B's area). */
    int tb_y = 120 - TITLEBAR_H + TITLEBAR_H / 2;
    wm_handle_mouse_down(&s, 55, tb_y, 1);

    /* A should now be at the front of z_order and focused. */
    int top_slot = s.z_order[s.num_windows - 1];
    ASSERT_EQ(s.windows[top_slot].id, id_a, "click raises A to front");
    ASSERT_EQ(s.windows[top_slot].focused, 1, "raised window is focused");
    ASSERT_EQ(s.drag_win_id, id_a, "dragging A (click in titlebar)");
}

static void test_mouse_down_content_no_drag(void)
{
    wm_state_t s = make_single_window();
    int id = s.windows[0].id;

    /* Click in content area (not title bar). */
    wm_handle_mouse_down(&s, 150, 130, 1);

    ASSERT_EQ(s.windows[s.num_windows-1].id, id, "window raised");
    ASSERT_EQ(s.drag_win_id, -1, "content click does not start drag");
}

static void test_mouse_down_desktop(void)
{
    wm_state_t s = make_single_window();

    /* Click on empty desktop. */
    wm_handle_mouse_down(&s, 5, 5, 1);
    ASSERT_EQ(s.drag_win_id, -1, "desktop click: no drag");
    ASSERT_EQ(s.num_windows, 1,  "window count unchanged");
}

static void test_mouse_move_drag(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);
    int id = wm_create_window(&s, "Drag", 100, 100 + TITLEBAR_H, 200, 100);

    /* Start drag from title bar. */
    int tb_y = 100 + TITLEBAR_H / 2;
    wm_handle_mouse_down(&s, 110, tb_y, 1);
    ASSERT_EQ(s.drag_win_id, id, "drag started");

    /* Move 30 px right, 20 px down. */
    wm_handle_mouse_move(&s, 140, tb_y + 20);

    window_t *w = wm_find_window(&s, id);
    ASSERT_NOTNULL(w, "dragged window still exists");
    /* New content x = (140 - drag_ox); content y derived from new titlebar y.
     * We just verify the window moved from its start position. */
    ASSERT(w->bounds.x != 100 || w->bounds.y != 100 + TITLEBAR_H,
           "window moved from original position");
}

static void test_mouse_up_ends_drag(void)
{
    wm_state_t s = make_single_window();

    /* Window content at (100, 100+TITLEBAR_H); titlebar y = 100. */
    int tb_y = 100 + TITLEBAR_H / 2;
    wm_handle_mouse_down(&s, 150, tb_y, 1);
    ASSERT(s.drag_win_id >= 0, "drag active");

    wm_handle_mouse_up(&s);
    ASSERT_EQ(s.drag_win_id, -1,       "drag ended after mouse_up");
    ASSERT_EQ((int)s.mouse_buttons, 0, "buttons cleared");
}

static void test_close_button_destroys(void)
{
    wm_state_t s = make_single_window();
    int id = s.windows[0].id;

    window_t *w = &s.windows[0];
    rect_t cb = wm_close_btn_rect(w);
    int cx = cb.x + cb.w / 2;
    int cy = cb.y + cb.h / 2;

    ASSERT_EQ(s.num_windows, 1, "one window before close");
    wm_handle_mouse_down(&s, cx, cy, 1);
    ASSERT_EQ(s.num_windows, 0,             "window destroyed by close button");
    ASSERT_NULL(wm_find_window(&s, id),     "window not findable after close");
    ASSERT_EQ(s.drag_win_id, -1,            "drag cleared after destroy");
}

/* ── Compositor pixel tests ──────────────────────────────────────────────── */

static void test_compositor_desktop_bg(void)
{
    canvas_t *screen = canvas_create(200, 150);
    ASSERT_NOTNULL(screen, "screen canvas allocated");

    wm_state_t s;
    wm_state_init(&s, 200, 150);

    /* No windows: composite should fill with desktop colour. */
    wm_composite(&s, screen);
    int bg_pixels = count_col(screen, WM_DESKTOP_COL);
    /* Most pixels (minus cursor + drop shadow, ~130 pixels max) should be desktop colour. */
    ASSERT(bg_pixels > 150 * 200 - 512, "almost all pixels are desktop colour");

    canvas_free(screen);
}

static void test_compositor_titlebar_colour(void)
{
    canvas_t *screen = canvas_create(400, 300);
    ASSERT_NOTNULL(screen, "screen canvas for titlebar test");

    wm_state_t s;
    wm_state_init(&s, 400, 300);

    /* Create one focused window, content at (50, 80), 200×100. */
    int id = wm_create_window(&s, "Win", 50, 80, 200, 100);
    wm_focus(&s, 0);
    window_t *w = wm_find_window(&s, id);
    /* Give it a solid-red backbuf so we can verify blit. */
    w->backbuf = canvas_create(200, 100);
    if (w->backbuf) gfx_fill(w->backbuf, GFX_RED);

    wm_composite(&s, screen);

    /* Title bar region should contain focused colour. */
    rect_t tb = wm_titlebar_rect(w);
    /* Check a point in the middle of the title bar (not near text). */
    ASSERT(any_col_in_rect(screen, tb, WM_TBAR_FOCUSED),
           "focused titlebar contains WM_TBAR_FOCUSED colour");

    /* Content area should be red (blitted from backbuf). */
    ASSERT(any_col_in_rect(screen, w->bounds, GFX_RED),
           "content area has red pixels from backbuf blit");

    /* Close button area should contain WM_CLOSE_COL (red — same value, test logic). */
    rect_t cb = wm_close_btn_rect(w);
    ASSERT(any_col_in_rect(screen, cb, WM_CLOSE_COL),
           "close button region contains close-button colour");

    canvas_free(w->backbuf);
    w->backbuf = (canvas_t *)0;
    canvas_free(screen);
}

static void test_compositor_unfocused_titlebar(void)
{
    canvas_t *screen = canvas_create(400, 300);
    ASSERT_NOTNULL(screen, "screen canvas for unfocused test");

    wm_state_t s;
    wm_state_init(&s, 400, 300);

    int id = wm_create_window(&s, "Back", 50, 80, 200, 100);
    /* Leave unfocused (default). */
    window_t *w = wm_find_window(&s, id);
    ASSERT_EQ(w->focused, 0, "window starts unfocused");

    wm_composite(&s, screen);

    rect_t tb = wm_titlebar_rect(w);
    ASSERT(any_col_in_rect(screen, tb, WM_TBAR_UNFOCUSED),
           "unfocused titlebar has WM_TBAR_UNFOCUSED colour");

    canvas_free(screen);
}

static void test_compositor_stacking_order(void)
{
    canvas_t *screen = canvas_create(400, 300);
    ASSERT_NOTNULL(screen, "screen canvas for stacking test");

    wm_state_t s;
    wm_state_init(&s, 400, 300);

    /* Window A (back): blue backbuf at (40, 80) 200×100. */
    int id_a = wm_create_window(&s, "A", 40, 80, 200, 100);
    window_t *wa = wm_find_window(&s, id_a);
    wa->backbuf = canvas_create(200, 100);
    if (wa->backbuf) gfx_fill(wa->backbuf, GFX_BLUE);

    /* Window B (front): green backbuf at (80, 100) 200×100 — overlaps A. */
    int id_b = wm_create_window(&s, "B", 80, 100, 200, 100);
    window_t *wb = wm_find_window(&s, id_b);
    wb->backbuf = canvas_create(200, 100);
    if (wb->backbuf) gfx_fill(wb->backbuf, GFX_GREEN);
    wm_focus(&s, 1);  /* B is focused */

    wm_composite(&s, screen);

    /* In the overlap region (x=80..240, y=100..180 clipped to A's content):
     * B (green) is on top so pixels there should be green.               */
    rect_t overlap = {100, 120, 80, 40};  /* safely inside both windows' content */
    ASSERT(any_col_in_rect(screen, overlap, GFX_GREEN),
           "overlap region shows B (green, front window)");

    /* Non-overlapping part of A should be blue. */
    rect_t a_only = {41, 82, 20, 20};
    ASSERT(any_col_in_rect(screen, a_only, GFX_BLUE),
           "non-overlapping part of A is blue");

    canvas_free(wa->backbuf); wa->backbuf = (canvas_t *)0;
    canvas_free(wb->backbuf); wb->backbuf = (canvas_t *)0;
    canvas_free(screen);
}

static void test_compositor_no_backbuf(void)
{
    canvas_t *screen = canvas_create(400, 300);
    ASSERT_NOTNULL(screen, "screen canvas for no-backbuf test");

    wm_state_t s;
    wm_state_init(&s, 400, 300);
    wm_create_window(&s, "NoBuf", 50, 80, 200, 100);
    /* Leave backbuf NULL. */

    /* Must not crash. */
    wm_composite(&s, screen);
    ASSERT_EQ(1, 1, "compositor handles NULL backbuf without crash");

    canvas_free(screen);
}

static void test_cursor_pixels(void)
{
    canvas_t *screen = canvas_create(200, 200);
    ASSERT_NOTNULL(screen, "screen for cursor test");

    gfx_fill(screen, GFX_BLACK);
    wm_draw_cursor(screen, 10, 10);

    /* The cursor's top-left pixel (bit 7 of cursor_shape[0]) must be white. */
    ASSERT_EQ((int)pixel_at(screen, 10, 10), (int)GFX_WHITE,
              "cursor top-left pixel is white");

    /* A point well outside the cursor must still be black. */
    ASSERT_EQ((int)pixel_at(screen, 100, 100), (int)GFX_BLACK,
              "non-cursor region unchanged");

    canvas_free(screen);
}

static void test_cursor_clip(void)
{
    canvas_t *screen = canvas_create(100, 100);
    ASSERT_NOTNULL(screen, "screen for cursor clip test");
    gfx_fill(screen, GFX_BLACK);

    /* Draw cursor at bottom-right corner — should not crash even if partially OOB. */
    wm_draw_cursor(screen, 96, 96);
    ASSERT_EQ(1, 1, "cursor at screen edge does not crash");

    canvas_free(screen);
}

/* ── Title truncation ────────────────────────────────────────────────────── */

static void test_title_truncation(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);

    char long_title[64];
    int i;
    for (i = 0; i < 63; i++) long_title[i] = 'X';
    long_title[63] = '\0';

    int id = wm_create_window(&s, long_title, 0, 50, 100, 80);
    window_t *w = wm_find_window(&s, id);
    ASSERT_NOTNULL(w, "window with long title created");
    ASSERT_EQ((int)w->title[WM_TITLE_LEN - 1], 0,
              "title NUL-terminated within WM_TITLE_LEN");
}

/* ── App callbacks (single-threaded model) ───────────────────────────────── */

static void test_window_fresh_callbacks(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);
    int id = wm_create_window(&s, "T", 0, 50, 100, 80);
    window_t *w = wm_find_window(&s, id);
    ASSERT_NOTNULL(w, "window allocated");
    ASSERT_NULL(w->app_state,  "fresh window has no app_state");
    ASSERT_NULL((void*)w->on_event,   "fresh window has no on_event");
    ASSERT_NULL((void*)w->on_tick,    "fresh window has no on_tick");
    ASSERT_NULL((void*)w->on_destroy, "fresh window has no on_destroy");
    ASSERT_EQ(0, w->want_close, "fresh window does not want close");
}

/* on_destroy must run exactly once when the window is destroyed. */
static int g_destroy_count = 0;
static void count_destroy(window_t *w) { (void)w; g_destroy_count++; }

static void test_on_destroy_invoked(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);
    int id = wm_create_window(&s, "T", 0, 50, 100, 80);
    window_t *w = wm_find_window(&s, id);
    w->on_destroy = count_destroy;

    g_destroy_count = 0;
    wm_destroy_window(&s, id);
    ASSERT_EQ(1, g_destroy_count, "on_destroy called once on destroy");
    ASSERT_EQ(0, s.num_windows,   "window count back to 0");
}

/* ── Entry point ─────────────────────────────────────────────────────────── */


/* ═══════════════════════ ROADMAP3 18.1: damage + ticks ═══════════════════ */

static int rect_eq(rect_t a, int x, int y, int w, int h)
{
    return a.x == x && a.y == y && a.w == w && a.h == h;
}

static void test_rect_algebra(void)
{
    rect_t a = { 0, 0, 10, 10 }, b = { 5, 5, 10, 10 }, c = { 20, 0, 5, 5 };
    ASSERT(rect_eq(wm_rect_intersect(a, b), 5, 5, 5, 5), "intersect overlap");
    ASSERT(wm_rect_empty(wm_rect_intersect(a, c)), "intersect disjoint is empty");
    ASSERT(!wm_rect_intersects((rect_t){0,0,10,10}, (rect_t){10,0,5,5}),
           "touching edges do not intersect");
    ASSERT(rect_eq(wm_rect_union(a, b), 0, 0, 15, 15), "union bounding box");
    ASSERT(rect_eq(wm_rect_union((rect_t){0,0,0,0}, c), 20, 0, 5, 5),
           "union with empty = other");
}

static void test_damage_merge_and_clip(void)
{
    wm_damage_t d;
    wm_damage_init(&d, 100, 80);
    wm_damage_add(&d, (rect_t){ 10, 10, 0, 5 });
    ASSERT_EQ(d.n, 0, "empty rect ignored");
    wm_damage_add(&d, (rect_t){ 200, 10, 5, 5 });
    ASSERT_EQ(d.n, 0, "off-screen rect ignored");
    wm_damage_add(&d, (rect_t){ -5, -5, 10, 10 });
    ASSERT(d.n == 1 && rect_eq(d.r[0], 0, 0, 5, 5), "clipped to screen");

    wm_damage_add(&d, (rect_t){ 50, 50, 10, 10 });
    ASSERT_EQ(d.n, 2, "disjoint rects kept apart");
    wm_damage_add(&d, (rect_t){ 3, 3, 4, 4 });
    ASSERT_EQ(d.n, 2, "overlapping rect merged");

    /* A bridge overlapping both merges all three into one. */
    wm_damage_add(&d, (rect_t){ 4, 4, 50, 50 });
    ASSERT(d.n == 1 && rect_eq(d.r[0], 0, 0, 60, 60), "chain merge");

    wm_damage_add_all(&d);
    ASSERT(d.n == 1 && rect_eq(d.r[0], 0, 0, 100, 80), "add_all = whole screen");
}

static void test_damage_overflow_collapses(void)
{
    wm_damage_t d;
    wm_damage_init(&d, 1000, 1000);
    for (int i = 0; i < WM_DAMAGE_MAX; i++)
        wm_damage_add(&d, (rect_t){ i * 20, 0, 5, 5 });
    ASSERT_EQ(d.n, WM_DAMAGE_MAX, "list full of disjoint rects");
    wm_damage_add(&d, (rect_t){ 500, 500, 5, 5 });
    ASSERT(d.n == 1 && rect_eq(d.r[0], 0, 0, 505, 505),
           "overflow collapses to the bounding rect");
}

/* Random adds: every added pixel stays covered, and no two rects overlap. */
static void test_damage_invariants_random(void)
{
    enum { W = 64, H = 48 };
    static unsigned char want[H][W];
    wm_damage_t d;
    unsigned seed = 12345;
    int ok_cover = 1, ok_disjoint = 1;

    for (int round = 0; round < 50; round++) {
        wm_damage_init(&d, W, H);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) want[y][x] = 0;
        int adds = 1 + round % 12;
        for (int k = 0; k < adds; k++) {
            seed = seed * 1103515245u + 12345u; int x = (int)(seed >> 16) % (W + 10) - 5;
            seed = seed * 1103515245u + 12345u; int y = (int)(seed >> 16) % (H + 10) - 5;
            seed = seed * 1103515245u + 12345u; int w = (int)(seed >> 16) % 20;
            seed = seed * 1103515245u + 12345u; int h = (int)(seed >> 16) % 20;
            wm_damage_add(&d, (rect_t){ x, y, w, h });
            for (int yy = y; yy < y + h; yy++)
                for (int xx = x; xx < x + w; xx++)
                    if (xx >= 0 && yy >= 0 && xx < W && yy < H) want[yy][xx] = 1;
        }
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (want[y][x]) {
                    int hit = 0;
                    for (int i = 0; i < d.n; i++)
                        if (wm_rect_contains(d.r[i], x, y)) hit = 1;
                    if (!hit) ok_cover = 0;
                }
        for (int i = 0; i < d.n; i++)
            for (int j = i + 1; j < d.n; j++)
                if (wm_rect_intersects(d.r[i], d.r[j])) ok_disjoint = 0;
    }
    ASSERT(ok_cover, "damage list covers every damaged pixel");
    ASSERT(ok_disjoint, "damage rects never overlap");
}

/* Compositing a clip rect paints exactly that rect, identically to a full
 * composite, and leaves every other pixel alone. */
static void test_composite_rect_matches_full(void)
{
    enum { W = 300, H = 200 };
    canvas_t *full = canvas_create(W, H), *part = canvas_create(W, H);
    ASSERT(full && part, "canvases allocated");

    wm_state_t s;
    wm_state_init(&s, W, H);
    int a = wm_create_window(&s, "A", 20, 40, 120, 80);
    int b = wm_create_window(&s, "B", 90, 70, 150, 90);
    window_t *wa = wm_find_window(&s, a), *wb = wm_find_window(&s, b);
    wa->backbuf = canvas_create(120, 80); gfx_fill(wa->backbuf, GFX_RGB(200, 0, 0));
    wb->backbuf = canvas_create(150, 90); gfx_fill(wb->backbuf, GFX_RGB(0, 0, 200));
    wm_focus(&s, wm_find_idx(&s, b));

    wm_composite(&s, full);
    const color_t SENT = GFX_RGB(1, 2, 3);
    gfx_fill(part, SENT);
    rect_t clip = { 70, 50, 100, 60 };
    wm_composite_rect(&s, part, clip);

    int inside_ok = 1, outside_ok = 1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t pv = part->pixels[y * part->pitch + x];
            if (wm_rect_contains(clip, x, y)) {
                if (pv != full->pixels[y * full->pitch + x]) inside_ok = 0;
            } else if (pv != SENT) {
                outside_ok = 0;
            }
        }
    ASSERT(inside_ok, "clipped composite == full composite inside the clip");
    ASSERT(outside_ok, "clipped composite touches nothing outside the clip");
    ASSERT(!part->has_clip, "clip cleared afterwards");

    canvas_free(wa->backbuf); canvas_free(wb->backbuf);
    canvas_free(full); canvas_free(part);
}

static void test_tick_scheduling(void)
{
    ASSERT_EQ(wm_ms_to_ticks(250, 100), 25u, "250 ms @100 Hz = 25 ticks");
    ASSERT_EQ(wm_ms_to_ticks(1, 100), 1u, "1 ms rounds up to 1 tick");
    ASSERT_EQ(wm_ms_to_ticks(0, 100), 0u, "0 ms = 0 ticks");

    wm_state_t s;
    wm_state_init(&s, 400, 300);
    ASSERT_EQ(wm_next_timeout_ms(&s, 0, 100), -1, "no windows: sleep forever");

    int id = wm_create_window(&s, "T", 10, 30, 100, 50);
    window_t *w = wm_find_window(&s, id);
    ASSERT(w->tick_ms == 0 && w->wait_fd == -1, "new window has no tick, no fd");
    w->on_tick = (wm_on_tick_fn)1;      /* never called here */
    ASSERT_EQ(wm_next_timeout_ms(&s, 0, 100), -1, "untimed window: forever");

    w->tick_ms = 250;
    w->next_tick = 0;
    ASSERT(wm_tick_due(w, 0), "first tick due immediately");
    wm_tick_rearm(w, 1000, 100);
    ASSERT_EQ(w->next_tick, 1025u, "rearm = now + 25 ticks");
    ASSERT(!wm_tick_due(w, 1024), "not due one tick early");
    ASSERT(wm_tick_due(w, 1025), "due on time");
    ASSERT_EQ(wm_next_timeout_ms(&s, 1000, 100), 250, "250 ms to go");
    ASSERT_EQ(wm_next_timeout_ms(&s, 1030, 100), 0, "overdue: 0 ms");

    /* Tick counter wrap. */
    wm_tick_rearm(w, 0xFFFFFFF0u, 100);
    ASSERT(!wm_tick_due(w, 0xFFFFFFFFu), "not due before wrap");
    ASSERT(wm_tick_due(w, 0x00000009u), "due across the wrap");
    ASSERT_EQ(wm_next_timeout_ms(&s, 0xFFFFFFF0u, 100), 250, "timeout across wrap");

    wm_destroy_window(&s, id);
    ASSERT_EQ(wm_next_timeout_ms(&s, 0, 100), -1, "destroyed window not scheduled");
}

/* -- Taskbar Power button + power dialog geometry -------------------------- */

static void test_power_button_layout(void)
{
    const int sw = 800, sh = 600, th = 28, bw = 110;
    rect_t p = wm_taskbar_power_rect(sw, sh, th);
    ASSERT(p.x + p.w <= sw && p.x >= sw - WM_POWER_BTN_W,
           "Power button sits at the right end of the taskbar");
    ASSERT(p.y >= sh - th && p.y + p.h <= sh, "Power button inside the taskbar");

    ASSERT_EQ(wm_taskbar_app_hit(5, sh - 10, sw, sh, th, 3, bw), 0, "first app");
    ASSERT_EQ(wm_taskbar_app_hit(2 * bw + 1, sh - 10, sw, sh, th, 3, bw), 2,
              "third app");
    ASSERT_EQ(wm_taskbar_app_hit(3 * bw + 1, sh - 10, sw, sh, th, 3, bw), -1,
              "past the last app: nothing");
    ASSERT_EQ(wm_taskbar_app_hit(5, sh - th - 1, sw, sh, th, 3, bw), -1,
              "above the taskbar: nothing");
    ASSERT_EQ(wm_taskbar_app_hit(p.x + 1, p.y + 1, sw, sh, th, 3, bw), -1,
              "the Power button is not an app button");
    ASSERT(wm_rect_contains(p, p.x + 1, p.y + 1), "Power button hit");

    /* A screen so narrow the Power button overlaps app buttons: the covered
     * ones are not clickable (and not drawn), so a click never does both. */
    int narrow = 3 * bw;
    rect_t pn = wm_taskbar_power_rect(narrow, sh, th);
    ASSERT_EQ(wm_taskbar_app_hit(pn.x + 1, sh - 10, narrow, sh, th, 3, bw), -1,
              "app button under the Power button doesn't fire");
}

static void test_power_dialog_buttons(void)
{
    const int cw = WM_POWER_DLG_W, ch = WM_POWER_DLG_H;
    rect_t ok = wm_dialog_btn_rect(WM_DLG_BTN_OK, cw, ch);
    rect_t ca = wm_dialog_btn_rect(WM_DLG_BTN_CANCEL, cw, ch);
    ASSERT(ok.x >= 0 && ca.x + ca.w <= cw, "both buttons fit horizontally");
    ASSERT(ok.y >= 0 && ok.y + ok.h <= ch, "buttons fit vertically");
    ASSERT(!wm_rect_intersects(ok, ca), "buttons don't overlap");
    ASSERT(ok.x < ca.x, "Power off is left of Cancel");
    ASSERT_EQ(wm_dialog_btn_hit(ok.x + 1, ok.y + 1, cw, ch), WM_DLG_BTN_OK,
              "hit Power off");
    ASSERT_EQ(wm_dialog_btn_hit(ca.x + ca.w - 1, ca.y + ca.h - 1, cw, ch),
              WM_DLG_BTN_CANCEL, "hit Cancel (bottom-right pixel)");
    ASSERT_EQ(wm_dialog_btn_hit(ok.x + ok.w + 1, ok.y + 1, cw, ch), -1,
              "the gap between the buttons hits nothing");
    ASSERT_EQ(wm_dialog_btn_hit(2, 2, cw, ch), -1, "message area hits nothing");
}

/* -- Key decoding (ROADMAP3 18.2, wm_keys.h) ------------------------------- */

/* Decode the whole string s; returns the number of keys, filling ks[]. */
static int decode_all(const char *s, int n, wm_key_t *ks, int max)
{
    int nk = 0;
    while (n > 0 && nk < max) {
        int used = wm_key_decode(s, n, &ks[nk++]);
        if (used <= 0) break;
        s += used; n -= used;
    }
    return nk;
}

static void test_key_plain_and_control(void)
{
    wm_key_t k;
    ASSERT_EQ(wm_key_decode("a", 1, &k), 1, "plain: 1 byte");
    ASSERT(k.keycode == 'a' && k.mods == 0 && k.ascii == 'a', "plain a");
    wm_key_decode("Q", 1, &k);
    ASSERT(k.keycode == 'q' && k.mods == WM_MOD_SHIFT && k.ascii == 'Q',
           "upper case -> letter + Shift, ascii kept");
    wm_key_decode("\x03", 1, &k);
    ASSERT(k.keycode == 'c' && k.mods == WM_MOD_CTRL && k.ascii == 0x03,
           "Ctrl-C -> 'c' + Ctrl, ascii 0x03");
    wm_key_decode("\n", 1, &k);
    ASSERT_EQ(k.keycode, WM_KEY_ENTER, "newline is Enter");
    wm_key_decode("\r", 1, &k);
    ASSERT_EQ(k.keycode, WM_KEY_ENTER, "CR is Enter");
    wm_key_decode("\t", 1, &k);
    ASSERT(k.keycode == WM_KEY_TAB && k.mods == 0, "Tab is not Ctrl-I");
    wm_key_decode("\b", 1, &k);
    ASSERT_EQ(k.keycode, WM_KEY_BACKSPACE, "backspace");
    wm_key_decode("\x7f", 1, &k);
    ASSERT_EQ(k.keycode, WM_KEY_BACKSPACE, "DEL is backspace too");
    wm_key_decode("\x1c", 1, &k);
    ASSERT(k.keycode == '\\' && k.mods == WM_MOD_CTRL, "Ctrl-backslash");
    ASSERT_EQ(wm_key_decode("", 0, &k), 0, "empty buffer consumes nothing");
}

static void test_key_escape_sequences(void)
{
    static const struct { const char *s; int code; } cases[] = {
        { "\x1b[A", WM_KEY_UP },     { "\x1b[B", WM_KEY_DOWN },
        { "\x1b[C", WM_KEY_RIGHT },  { "\x1b[D", WM_KEY_LEFT },
        { "\x1b[H", WM_KEY_HOME },   { "\x1b[F", WM_KEY_END },
        { "\x1b[2~", WM_KEY_INSERT },{ "\x1b[3~", WM_KEY_DELETE },
        { "\x1b[5~", WM_KEY_PGUP },  { "\x1b[6~", WM_KEY_PGDN },
        { "\x1bOP", WM_KEY_F1 },     { "\x1bOS", WM_KEY_F1 + 3 },
        { "\x1b[15~", WM_KEY_F1 + 4 }, { "\x1b[21~", WM_KEY_F1 + 9 },
        { "\x1b[23~", WM_KEY_F1 + 10 },{ "\x1b[24~", WM_KEY_F12 },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        wm_key_t k;
        int len = (int)strlen(cases[i].s);
        int used = wm_key_decode(cases[i].s, len, &k);
        ASSERT_EQ(used, len, "whole sequence consumed");
        ASSERT_EQ(k.keycode, cases[i].code, "sequence -> keycode");
        ASSERT(k.mods == 0 && k.ascii == 0, "no mods, no ascii");
        ASSERT(k.seq_len == len && memcmp(k.seq, cases[i].s, len) == 0,
               "raw bytes kept for the terminal");
    }
}

static void test_key_modifiers(void)
{
    wm_key_t k;
    wm_key_decode("\x1b[1;2A", 6, &k);
    ASSERT(k.keycode == WM_KEY_UP && k.mods == WM_MOD_SHIFT, "Shift-Up");
    wm_key_decode("\x1b[1;5D", 6, &k);
    ASSERT(k.keycode == WM_KEY_LEFT && k.mods == WM_MOD_CTRL, "Ctrl-Left");
    wm_key_decode("\x1b[1;3S", 6, &k);
    ASSERT(k.keycode == WM_KEY_F1 + 3 && k.mods == WM_MOD_ALT, "Alt-F4");
    wm_key_decode("\x1b[5;8~", 6, &k);
    ASSERT(k.keycode == WM_KEY_PGUP &&
           k.mods == (WM_MOD_SHIFT | WM_MOD_ALT | WM_MOD_CTRL),
           "Ctrl-Alt-Shift-PgUp");
    ASSERT_EQ(k.seq_len, 6, "modified sequence kept whole");

    wm_key_decode("\x1b\t", 2, &k);
    ASSERT(k.keycode == WM_KEY_TAB && k.mods == WM_MOD_ALT, "Alt-Tab");
    wm_key_decode("\x1bx", 2, &k);
    ASSERT(k.keycode == 'x' && k.mods == WM_MOD_ALT && k.ascii == 'x', "Alt-x");
    wm_key_decode("\x1b\x1bOS", 4, &k);
    ASSERT(k.keycode == WM_KEY_F1 + 3 && k.mods == WM_MOD_ALT && k.seq_len == 4,
           "ESC-prefixed sequence = Alt + that key");
    wm_key_decode("\x1b", 1, &k);
    ASSERT(k.keycode == WM_KEY_ESC && k.mods == 0, "lone ESC is the Esc key");
    wm_key_decode("\x1b\x1b", 2, &k);
    ASSERT(k.keycode == WM_KEY_ESC && k.mods == WM_MOD_ALT, "Alt-Esc");

    ASSERT_EQ(wm_key_mods_from_param(1), 0, "m=1: no modifiers");
    ASSERT_EQ(wm_key_mods_from_param(9), 0, "out of range: none");
}

static void test_key_stream(void)
{
    wm_key_t ks[8];
    const char s[] = "a\x1b[Ab\x1b[3~\x03";
    int nk = decode_all(s, (int)sizeof s - 1, ks, 8);
    ASSERT_EQ(nk, 5, "five keys in the stream");
    ASSERT(ks[0].keycode == 'a' && ks[1].keycode == WM_KEY_UP &&
           ks[2].keycode == 'b' && ks[3].keycode == WM_KEY_DELETE &&
           ks[4].keycode == 'c' && ks[4].mods == WM_MOD_CTRL,
           "stream decodes in order");

    /* Malformed / truncated sequences never stall or overrun. */
    wm_key_t k;
    ASSERT_EQ(wm_key_decode("\x1b[", 2, &k), 1, "truncated CSI: Esc alone");
    ASSERT_EQ(k.keycode, WM_KEY_ESC, "... reported as Esc");
    ASSERT_EQ(wm_key_decode("\x1b[1;2", 5, &k), 1, "no final byte: Esc");
    ASSERT_EQ(wm_key_decode("\x1b[\x01", 3, &k), 1, "junk in CSI: Esc");
    ASSERT_EQ(wm_key_decode("\x1b[99~", 5, &k), 5, "unknown ~ number consumed");
    ASSERT_EQ(k.keycode, WM_KEY_UNKNOWN, "... as an unknown key");
    ASSERT_EQ(wm_key_decode("\x1b[123456789A", 12, &k), 1,
              "over-long CSI is not read past WM_KEY_SEQ_MAX");

    /* Every byte value alone decodes to exactly one byte. */
    int ok = 1;
    for (int b = 1; b < 256; b++) {
        char c = (char)b;
        if (wm_key_decode(&c, 1, &k) != 1 || k.seq_len != 1) ok = 0;
    }
    ASSERT(ok, "any single byte decodes as one key");
}

/* -- Mouse button edges + Alt-Tab (ROADMAP3 18.2) --------------------------- */

static void test_button_edges(void)
{
    wm_btn_edge_t e[WM_BTN_EDGES_MAX];
    ASSERT_EQ(wm_button_edges(0, 0, 0, 0, e), 0, "nothing happened");
    ASSERT_EQ(wm_button_edges(1, 1, 0, 0, e), 0, "held: no edge");

    int n = wm_button_edges(0, 1, 1, 0, e);
    ASSERT(n == 1 && e[0].button == 1 && e[0].down == 1, "left press");
    n = wm_button_edges(1, 0, 0, 1, e);
    ASSERT(n == 1 && e[0].button == 1 && e[0].down == 0, "left release");

    /* A click shorter than one WM iteration: both edges latched, the level
     * sample sees "up" both times.  The old WM lost these clicks. */
    n = wm_button_edges(0, 0, 1, 1, e);
    ASSERT(n == 2 && e[0].down == 1 && e[1].down == 0, "fast click: down, up");

    /* Released and pressed again while sampled down both times. */
    n = wm_button_edges(1, 1, 1, 1, e);
    ASSERT(n == 2 && e[0].down == 0 && e[1].down == 1, "re-press: up, down");

    /* Double click inside one sample: down, up, down. */
    n = wm_button_edges(0, 1, 1, 1, e);
    ASSERT(n == 3 && e[0].down == 1 && e[1].down == 0 && e[2].down == 1,
           "down, up, down");

    /* Per-button: right pressed while left released. */
    n = wm_button_edges(1, 2, 2, 1, e);
    ASSERT(n == 2, "two buttons changed");
    ASSERT(e[0].button == 1 && e[0].down == 0, "left up");
    ASSERT(e[1].button == 2 && e[1].down == 1, "right down");

    /* No latches (old kernel) still yields the level change. */
    n = wm_button_edges(0, 4, 0, 0, e);
    ASSERT(n == 1 && e[0].button == 4 && e[0].down == 1, "middle from level");

    /* Bound. */
    ASSERT(wm_button_edges(0, 7, 7, 7, e) <= WM_BTN_EDGES_MAX, "bounded");
}

static void test_cycle_focus(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);
    ASSERT_EQ(wm_cycle_focus(&s), -1, "no windows: nothing to focus");

    int a = wm_create_window(&s, "A", 10, 30, 100, 80);
    int b = wm_create_window(&s, "B", 20, 40, 100, 80);
    int c = wm_create_window(&s, "C", 30, 50, 100, 80);
    int sa = wm_find_idx(&s, a), sb = wm_find_idx(&s, b), sc = wm_find_idx(&s, c);
    wm_focus(&s, sc);                       /* C front + focused: A B C */

    int seen_a = 0, seen_b = 0, seen_c = 0;
    for (int i = 0; i < 3; i++) {
        int slot = wm_cycle_focus(&s);
        ASSERT_EQ(s.z_order[s.num_windows - 1], slot, "cycled window is in front");
        ASSERT(s.windows[slot].focused, "and focused");
        int nfocused = 0;
        for (int k = 0; k < WM_MAX_WINDOWS; k++) nfocused += s.windows[k].focused;
        ASSERT_EQ(nfocused, 1, "exactly one focused window");
        if (slot == sa) seen_a = 1;
        if (slot == sb) seen_b = 1;
        if (slot == sc) seen_c = 1;
    }
    ASSERT(seen_a && seen_b && seen_c, "three Alt-Tabs visit all three windows");
    ASSERT_EQ(s.windows[sc].focused, 1, "and come back to where we started");
}

int main(void)
{
    RUN_SUITE(test_geometry_rect_contains);
    RUN_SUITE(test_geometry_titlebar);
    RUN_SUITE(test_geometry_close_btn);
    RUN_SUITE(test_geometry_hit_regions);

    RUN_SUITE(test_state_init);
    RUN_SUITE(test_create_window);
    RUN_SUITE(test_destroy_window);
    RUN_SUITE(test_max_windows);

    RUN_SUITE(test_hittest_basic);
    RUN_SUITE(test_hittest_stacking);
    RUN_SUITE(test_raise);
    RUN_SUITE(test_focus);
    RUN_SUITE(test_focus_transfer_on_destroy);

    RUN_SUITE(test_mouse_down_raise_focus);
    RUN_SUITE(test_mouse_down_content_no_drag);
    RUN_SUITE(test_mouse_down_desktop);
    RUN_SUITE(test_mouse_move_drag);
    RUN_SUITE(test_mouse_up_ends_drag);
    RUN_SUITE(test_close_button_destroys);

    RUN_SUITE(test_compositor_desktop_bg);
    RUN_SUITE(test_compositor_titlebar_colour);
    RUN_SUITE(test_compositor_unfocused_titlebar);
    RUN_SUITE(test_compositor_stacking_order);
    RUN_SUITE(test_compositor_no_backbuf);

    RUN_SUITE(test_cursor_pixels);
    RUN_SUITE(test_cursor_clip);

    RUN_SUITE(test_title_truncation);

    RUN_SUITE(test_window_fresh_callbacks);
    RUN_SUITE(test_on_destroy_invoked);

    RUN_SUITE(test_rect_algebra);
    RUN_SUITE(test_damage_merge_and_clip);
    RUN_SUITE(test_damage_overflow_collapses);
    RUN_SUITE(test_damage_invariants_random);
    RUN_SUITE(test_composite_rect_matches_full);
    RUN_SUITE(test_tick_scheduling);
    RUN_SUITE(test_power_button_layout);
    RUN_SUITE(test_power_dialog_buttons);
    RUN_SUITE(test_key_plain_and_control);
    RUN_SUITE(test_key_escape_sequences);
    RUN_SUITE(test_key_modifiers);
    RUN_SUITE(test_key_stream);
    RUN_SUITE(test_button_edges);
    RUN_SUITE(test_cycle_focus);

    TEST_SUMMARY();
}
