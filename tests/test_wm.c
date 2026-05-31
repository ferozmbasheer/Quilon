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
#include "framework.h"

/* Include gfx.h via the include path supplied in the Makefile rule. */
#include <gfx.h>
#include "../user/wm/wm.h"

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

/* ── Event queue ─────────────────────────────────────────────────────────── */

static void test_evqueue_empty(void)
{
    wm_evqueue_t q;
    q.head = q.tail = 0;

    wm_event_t ev;
    ASSERT_EQ(0, wm_evqueue_poll(&q, &ev), "poll on empty returns 0");
}

static void test_evqueue_push_poll(void)
{
    wm_evqueue_t q;
    q.head = q.tail = 0;

    wm_event_t in;
    in.type    = WM_EV_KEY;
    in.ascii   = 'A';
    in.x = in.y = in.buttons = 0;
    wm_evqueue_push(&q, in);

    wm_event_t out;
    ASSERT_EQ(1, wm_evqueue_poll(&q, &out), "poll returns 1 after push");
    ASSERT_EQ((int)WM_EV_KEY, (int)out.type, "type preserved");
    ASSERT_EQ((int)'A', (int)out.ascii,      "ascii preserved");
    ASSERT_EQ(0, wm_evqueue_poll(&q, &out),  "queue empty after poll");
}

static void test_evqueue_fifo_order(void)
{
    wm_evqueue_t q;
    q.head = q.tail = 0;

    wm_event_t ev;
    ev.x = ev.y = ev.buttons = 0;
    ev.type = WM_EV_KEY; ev.ascii = '1'; wm_evqueue_push(&q, ev);
    ev.type = WM_EV_KEY; ev.ascii = '2'; wm_evqueue_push(&q, ev);
    ev.type = WM_EV_KEY; ev.ascii = '3'; wm_evqueue_push(&q, ev);

    wm_evqueue_poll(&q, &ev); ASSERT_EQ((int)'1', (int)ev.ascii, "first out is '1'");
    wm_evqueue_poll(&q, &ev); ASSERT_EQ((int)'2', (int)ev.ascii, "second out is '2'");
    wm_evqueue_poll(&q, &ev); ASSERT_EQ((int)'3', (int)ev.ascii, "third out is '3'");
    ASSERT_EQ(0, wm_evqueue_poll(&q, &ev),                       "empty after 3 pops");
}

static void test_evqueue_overflow_drops(void)
{
    wm_evqueue_t q;
    q.head = q.tail = 0;

    wm_event_t ev;
    ev.type = WM_EV_KEY; ev.x = ev.y = ev.buttons = 0;

    /* Fill to WM_EVQUEUE_DEPTH - 1 (ring buffer can hold DEPTH-1 items). */
    int i;
    for (i = 0; i < WM_EVQUEUE_DEPTH - 1; i++) {
        ev.ascii = (char)(i & 0x7F);
        wm_evqueue_push(&q, ev);
    }
    /* One more push should be silently dropped. */
    ev.ascii = 0x7E;
    wm_evqueue_push(&q, ev);

    /* Count how many we can pop. */
    int count = 0;
    while (wm_evqueue_poll(&q, &ev)) count++;
    ASSERT_EQ(WM_EVQUEUE_DEPTH - 1, count, "overflow drops the extra event");
}

static void test_evqueue_window_has_clean_queue(void)
{
    wm_state_t s;
    wm_state_init(&s, 800, 600);
    int id = wm_create_window(&s, "T", 0, 50, 100, 80);
    window_t *w = wm_find_window(&s, id);
    ASSERT_NOTNULL(w, "window allocated");

    wm_event_t ev;
    ASSERT_EQ(0, wm_evqueue_poll(&w->events, &ev),
              "fresh window event queue is empty");
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

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

    RUN_SUITE(test_evqueue_empty);
    RUN_SUITE(test_evqueue_push_poll);
    RUN_SUITE(test_evqueue_fifo_order);
    RUN_SUITE(test_evqueue_overflow_drops);
    RUN_SUITE(test_evqueue_window_has_clean_queue);

    TEST_SUMMARY();
}
