/*
 * Quilon OS -- Window Manager & Compositor (section 14.2, single-threaded)
 *
 * This header defines all WM types and pure-logic helpers.  The static inline
 * functions (geometry, hit-testing, z-order, state management) have no
 * hardware or syscall dependencies and are fully testable on the host.
 *
 * The compositor (wm_composite, wm_draw_cursor) is defined in wm/main.c
 * because it calls libgfx functions.  This header only declares them.
 *
 * Concurrency model (single-threaded event loop)
 * ──────────────────────────────────────────────
 * The WM is ONE thread running ONE event loop.  Apps are NOT threads -- they
 * are cooperative, non-blocking handlers invoked by the loop:
 *
 *   on_event(win, ev) -- called when a key / close event is delivered.
 *   on_tick(win)      -- called every loop iteration for periodic work
 *                        (clock updates, draining a terminal's shell pipe).
 *
 * Because only the loop ever touches wm_state_t and the framebuffer, there is
 * no shared-state race: no locks, no SPSC queue, no pthread_join reaping.  An
 * app must never block (no blocking read()); it returns quickly from each
 * callback and is ticked again next iteration.  Per-window app state lives in
 * win->app_state (heap-allocated by the launcher, freed on destroy).
 *
 * Z-order model
 * ─────────────
 * windows[] slots are permanently assigned for the window's lifetime; the
 * slot index never changes.  z_order[0..num_windows-1] holds the active slot
 * indices in back-to-front order (z_order[num_windows-1] = frontmost).
 * wm_raise() rotates only z_order[] — slot indices never move.
 */

#ifndef _USER_WM_WM_H
#define _USER_WM_WM_H

#include <stdint.h>

/* gfx.h is pulled in for canvas_t / rect_t / color_t.
 * When compiling on the host for tests, link gfx.c alongside; on the target,
 * the program is linked with -lgfx.                                         */
#include <gfx.h>

/* ── Constants ─────────────────────────────────────────────────────────── */

#define WM_MAX_WINDOWS   16
#define WM_TITLE_LEN     32
#define TITLEBAR_H       20   /* pixels */
#define WM_CLOSE_BTN_W   16
#define WM_CLOSE_BTN_H   16
#define WM_BORDER_W       1
#define WM_MIN_W         64
#define WM_MIN_H         32

/* Mouse cursor footprint on screen, including its 1-px drop shadow.  The
 * damage rect for a cursor at (x, y) is (x, y, WM_CURSOR_W, WM_CURSOR_H). */
#define WM_CURSOR_W      12
#define WM_CURSOR_H      18

/* Damage list capacity (ROADMAP3 18.1).  On overflow the list collapses to
 * one bounding rect rather than dropping damage. */
#define WM_DAMAGE_MAX    16

/* Desktop */
#define WM_DESKTOP_COL      GFX_RGB( 96, 138, 175)   /* steel blue (à la twlv) */

/* Title bar */
#define WM_TBAR_FOCUSED     GFX_RGB( 72, 102, 158)   /* muted navy */
#define WM_TBAR_UNFOCUSED   GFX_RGB(110, 110, 118)   /* gray-blue */
#define WM_TBAR_TEXT_COL    GFX_WHITE

/* Close button */
#define WM_CLOSE_COL        GFX_RGB(180,  50,  50)

/* 3D bevel colours (used for window chrome and taskbar buttons) */
#define WM_FRAME_BASE       GFX_RGB(207, 207, 207)   /* light gray fill */
#define WM_FRAME_HI         GFX_WHITE                /* top / left highlight */
#define WM_FRAME_MID        GFX_RGB(160, 160, 160)   /* inner shadow */
#define WM_FRAME_SHADOW     GFX_RGB( 64,  64,  64)   /* bottom / right shadow */

/* Taskbar */
#define WM_TASKBAR_COL      GFX_RGB(191, 191, 191)   /* light gray */
#define WM_TASKBTN_COL      WM_FRAME_BASE
#define WM_TASKBTN_TEXT     GFX_BLACK

/* ── Event types (WM → app) ────────────────────────────────────────────── */

typedef enum {
    WM_EV_NONE       = 0,
    WM_EV_KEY        = 1,
    WM_EV_MOUSE_MOVE = 2,
    WM_EV_MOUSE_BTN  = 3,
    WM_EV_CLOSE      = 4,
} wm_ev_type_t;

typedef struct {
    wm_ev_type_t type;
    int          x;
    int          y;
    uint8_t      buttons;
    char         ascii;
} wm_event_t;

/* ── Window descriptor ─────────────────────────────────────────────────── */

struct wm_window_;

/* App callbacks (single-threaded model).  Both run on the WM loop thread and
 * MUST return promptly without blocking.
 *   on_event -- a key or close event was delivered to this window.
 *   on_tick  -- periodic work; called every loop iteration.
 *   on_destroy -- the window is being destroyed; free app_state here.
 * Any of them may be NULL. */
typedef void (*wm_on_event_fn)(struct wm_window_ *w, const wm_event_t *ev);
typedef void (*wm_on_tick_fn)(struct wm_window_ *w);
typedef void (*wm_on_destroy_fn)(struct wm_window_ *w);

typedef struct wm_window_ {
    int            id;               /* unique ID (≥ 1); 0 = slot unused   */
    rect_t         bounds;           /* content area: (x, y, w, h)         */
    char           title[WM_TITLE_LEN];
    canvas_t      *backbuf;          /* app draws here; NULL until assigned */
    int            focused;          /* 1 = has keyboard focus              */
    int            active;           /* 1 = slot occupied                   */
    int            dirty;            /* 1 = backbuf has new pixels          */
    int            want_close;       /* set by on_event to request destroy  */
    void          *app_state;        /* per-window app data (heap), or NULL  */
    wm_on_event_fn   on_event;       /* key/close handler, or NULL          */
    wm_on_tick_fn    on_tick;        /* periodic handler, or NULL           */
    wm_on_destroy_fn on_destroy;     /* cleanup handler, or NULL            */

    /* When to call on_tick (ROADMAP3 18.1) -- the loop sleeps otherwise:
     *   tick_ms > 0  -- every tick_ms milliseconds (the clock);
     *   wait_fd >= 0 -- whenever that fd has bytes to read (a terminal's
     *                   shell-output pipe).
     * Both may be set; with neither, on_tick is never called. */
    int            tick_ms;
    int            wait_fd;
    uint32_t       next_tick;        /* PIT tick the next timed tick is due */
} window_t;

/* ── WM state ──────────────────────────────────────────────────────────── */

typedef struct {
    window_t windows[WM_MAX_WINDOWS];
    int      z_order[WM_MAX_WINDOWS]; /* slot indices, back[0] to front[num_windows-1] */
    int      num_windows;   /* number of active windows                  */
    int      next_id;       /* monotonically increasing ID counter        */
    int      screen_w;
    int      screen_h;
    int      mouse_x;
    int      mouse_y;
    uint8_t  mouse_buttons;
    int      drag_win_id;   /* ID of window being dragged, -1 = none     */
    int      drag_ox;       /* cursor X offset within titlebar on drag   */
    int      drag_oy;       /* cursor Y offset within titlebar on drag   */
} wm_state_t;

/* ======================================================================== *
 * Pure-logic inline helpers — no hardware, no syscalls, host-testable.     *
 * ======================================================================== */

/* ── Geometry ──────────────────────────────────────────────────────────── */

static inline int wm_rect_contains(rect_t r, int px, int py)
{
    return px >= r.x && px < r.x + r.w &&
           py >= r.y && py < r.y + r.h;
}

static inline rect_t wm_titlebar_rect(const window_t *w)
{
    rect_t r;
    r.x = w->bounds.x;
    r.y = w->bounds.y - TITLEBAR_H;
    r.w = w->bounds.w;
    r.h = TITLEBAR_H;
    return r;
}

static inline rect_t wm_close_btn_rect(const window_t *w)
{
    rect_t tb = wm_titlebar_rect(w);
    rect_t r;
    r.x = tb.x + tb.w - WM_CLOSE_BTN_W - 2;
    r.y = tb.y + (TITLEBAR_H - WM_CLOSE_BTN_H) / 2;
    r.w = WM_CLOSE_BTN_W;
    r.h = WM_CLOSE_BTN_H;
    return r;
}

/* Full window rect: title bar + content area. */
static inline rect_t wm_full_rect(const window_t *w)
{
    rect_t r;
    r.x = w->bounds.x;
    r.y = w->bounds.y - TITLEBAR_H;
    r.w = w->bounds.w;
    r.h = TITLEBAR_H + w->bounds.h;
    return r;
}

static inline int wm_in_titlebar(const window_t *w, int mx, int my)
{
    return wm_rect_contains(wm_titlebar_rect(w), mx, my);
}

static inline int wm_in_close_btn(const window_t *w, int mx, int my)
{
    return wm_rect_contains(wm_close_btn_rect(w), mx, my);
}

static inline int wm_in_content(const window_t *w, int mx, int my)
{
    return wm_rect_contains(w->bounds, mx, my);
}

static inline int wm_in_window(const window_t *w, int mx, int my)
{
    return wm_rect_contains(wm_full_rect(w), mx, my);
}

/* ── State management ──────────────────────────────────────────────────── */

static inline void wm_state_init(wm_state_t *s, int screen_w, int screen_h)
{
    int i;
    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        s->windows[i].id          = 0;
        s->windows[i].active      = 0;
        s->windows[i].dirty       = 0;
        s->windows[i].focused     = 0;
        s->windows[i].want_close  = 0;
        s->windows[i].backbuf     = (canvas_t *)0;
        s->windows[i].app_state   = (void *)0;
        s->windows[i].on_event    = (wm_on_event_fn)0;
        s->windows[i].on_tick     = (wm_on_tick_fn)0;
        s->windows[i].on_destroy  = (wm_on_destroy_fn)0;
        s->windows[i].tick_ms     = 0;
        s->windows[i].wait_fd     = -1;
        s->windows[i].next_tick   = 0;
        s->z_order[i]             = i;
    }
    s->num_windows   = 0;
    s->next_id       = 1;
    s->screen_w      = screen_w;
    s->screen_h      = screen_h;
    s->mouse_x       = screen_w / 2;
    s->mouse_y       = screen_h / 2;
    s->mouse_buttons = 0;
    s->drag_win_id   = -1;
    s->drag_ox       = 0;
    s->drag_oy       = 0;
}

/*
 * wm_create_window -- find the first free slot (id==0), assign it, and
 * append it to z_order at the front (topmost).
 * Returns the new window ID on success, -1 if the table is full.
 * The caller sets w->backbuf after this call.
 */
static inline int wm_create_window(wm_state_t *s, const char *title,
                                   int x, int y, int w, int h)
{
    int i, slot;
    if (s->num_windows >= WM_MAX_WINDOWS) return -1;

    /* Find first free slot. */
    slot = -1;
    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        if (s->windows[i].id == 0) { slot = i; break; }
    }
    if (slot < 0) return -1;

    window_t *win = &s->windows[slot];
    win->id       = s->next_id++;
    win->bounds.x = x;
    win->bounds.y = y;
    win->bounds.w = w < WM_MIN_W ? WM_MIN_W : w;
    win->bounds.h = h < WM_MIN_H ? WM_MIN_H : h;
    win->focused      = 0;
    win->active       = 1;
    win->dirty        = 1;
    win->want_close   = 0;
    win->backbuf      = (canvas_t *)0;
    win->app_state    = (void *)0;
    win->on_event     = (wm_on_event_fn)0;
    win->on_tick      = (wm_on_tick_fn)0;
    win->on_destroy   = (wm_on_destroy_fn)0;
    win->tick_ms      = 0;
    win->wait_fd      = -1;
    win->next_tick    = 0;

    /* Copy title — avoid string.h dependency. */
    for (i = 0; i < WM_TITLE_LEN - 1 && title && title[i]; i++)
        win->title[i] = title[i];
    win->title[i] = '\0';

    /* New window goes to the front of z_order. */
    s->z_order[s->num_windows] = slot;
    s->num_windows++;
    return win->id;
}

/* Return slot index of window with given id, or -1.
 * id=0 always returns -1 because 0 is the "slot free" sentinel. */
static inline int wm_find_idx(const wm_state_t *s, int id)
{
    int i;
    if (id == 0) return -1;
    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        if (s->windows[i].id == id)
            return i;
    }
    return -1;
}

/* Return pointer to window with given id, or NULL. */
static inline window_t *wm_find_window(wm_state_t *s, int id)
{
    int idx = wm_find_idx(s, id);
    return (idx >= 0) ? &s->windows[idx] : (window_t *)0;
}

/*
 * wm_destroy_window -- remove window with given id.
 * Clears the slot and removes it from z_order (slot index stays stable).
 * If the destroyed window had focus, the new frontmost window gets it.
 */
static inline void wm_destroy_window(wm_state_t *s, int id)
{
    int slot = wm_find_idx(s, id);
    if (slot < 0) return;

    window_t *w = &s->windows[slot];
    int had_focus = w->focused;

    /* Let the app free its per-window state (app_state, and its own canvas if
     * it owns one).  Runs on the WM loop thread, synchronously. */
    if (w->on_destroy) w->on_destroy(w);

    /* Remove slot from z_order, shifting later entries left. */
    int i, z_pos = -1;
    for (i = 0; i < s->num_windows; i++) {
        if (s->z_order[i] == slot) { z_pos = i; break; }
    }
    if (z_pos >= 0) {
        for (i = z_pos; i < s->num_windows - 1; i++)
            s->z_order[i] = s->z_order[i + 1];
    }

    /* Clear the slot — id=0 marks it free for reuse. */
    w->id         = 0;
    w->active     = 0;
    w->focused    = 0;
    w->want_close = 0;
    w->app_state  = (void *)0;
    w->on_event   = (wm_on_event_fn)0;
    w->on_tick    = (wm_on_tick_fn)0;
    w->on_destroy = (wm_on_destroy_fn)0;
    w->tick_ms    = 0;
    w->wait_fd    = -1;

    s->num_windows--;

    /* Transfer focus to the new frontmost window if needed. */
    if (had_focus && s->num_windows > 0) {
        int top = s->z_order[s->num_windows - 1];
        s->windows[top].focused = 1;
    }

    if (s->drag_win_id == id)
        s->drag_win_id = -1;
}

/*
 * wm_hittest -- return stable slot index of the topmost window under (mx, my),
 * or -1 if nothing is hit.  Iterates z_order front-to-back.
 */
static inline int wm_hittest(const wm_state_t *s, int mx, int my)
{
    int i;
    for (i = s->num_windows - 1; i >= 0; i--) {
        int slot = s->z_order[i];
        if (wm_in_window(&s->windows[slot], mx, my))
            return slot;
    }
    return -1;
}

/*
 * wm_raise -- bring window at stable slot index to the front of z_order.
 * Only rotates z_order[]; the slot and the window_t* it points to never move.
 */
static inline void wm_raise(wm_state_t *s, int slot)
{
    int i, z_pos = -1;
    for (i = 0; i < s->num_windows; i++) {
        if (s->z_order[i] == slot) { z_pos = i; break; }
    }
    if (z_pos < 0) return;
    if (z_pos == s->num_windows - 1) return;  /* already at front */

    for (i = z_pos; i < s->num_windows - 1; i++)
        s->z_order[i] = s->z_order[i + 1];
    s->z_order[s->num_windows - 1] = slot;
}

/*
 * wm_focus -- give keyboard focus to the window at stable slot index.
 * Clears focus on all other windows.
 */
static inline void wm_focus(wm_state_t *s, int slot)
{
    int i;
    for (i = 0; i < WM_MAX_WINDOWS; i++)
        s->windows[i].focused = (i == slot && s->windows[i].id != 0) ? 1 : 0;
}

/* ── Mouse event handling ──────────────────────────────────────────────── */

/*
 * wm_handle_mouse_down -- process a button press.
 *
 * Raises and focuses the hit window.  If the click lands in the title bar,
 * starts a drag.  If in the close button, destroys the window.
 */
static inline void wm_handle_mouse_down(wm_state_t *s,
                                        int mx, int my, uint8_t buttons)
{
    s->mouse_x       = mx;
    s->mouse_y       = my;
    s->mouse_buttons = buttons;
    s->drag_win_id   = -1;

    int idx = wm_hittest(s, mx, my);
    if (idx < 0) return;

    int win_id = s->windows[idx].id;

    wm_raise(s, idx);
    /* idx is a stable slot index — still valid after raise. */
    wm_focus(s, idx);

    window_t *w = &s->windows[idx];

    if (wm_in_close_btn(w, mx, my)) {
        wm_destroy_window(s, win_id);
        return;
    }

    if (wm_in_titlebar(w, mx, my)) {
        s->drag_win_id = win_id;
        rect_t tb      = wm_titlebar_rect(w);
        s->drag_ox     = mx - tb.x;
        s->drag_oy     = my - tb.y;
    }
}

/*
 * wm_handle_mouse_move -- update cursor; move the dragged window.
 */
static inline void wm_handle_mouse_move(wm_state_t *s, int mx, int my)
{
    s->mouse_x = mx;
    s->mouse_y = my;

    if (s->drag_win_id < 0) return;

    int idx = wm_find_idx(s, s->drag_win_id);
    if (idx < 0) { s->drag_win_id = -1; return; }

    window_t *w = &s->windows[idx];

    int new_x = mx - s->drag_ox;
    int new_y = my - s->drag_oy + TITLEBAR_H;

    /* Keep title bar on screen. */
    if (new_x < -(w->bounds.w - WM_CLOSE_BTN_W * 2))
        new_x = -(w->bounds.w - WM_CLOSE_BTN_W * 2);
    if (new_x > s->screen_w - WM_CLOSE_BTN_W * 2)
        new_x = s->screen_w - WM_CLOSE_BTN_W * 2;
    if (new_y < TITLEBAR_H)
        new_y = TITLEBAR_H;
    if (new_y > s->screen_h)
        new_y = s->screen_h;

    w->bounds.x = new_x;
    w->bounds.y = new_y;
    w->dirty    = 1;
}

/*
 * wm_handle_mouse_up -- end the current drag.
 */
static inline void wm_handle_mouse_up(wm_state_t *s)
{
    s->mouse_buttons = 0;
    s->drag_win_id   = -1;
}

/* ── Rect algebra (ROADMAP3 18.1) ──────────────────────────────────────── */

static inline int wm_rect_empty(rect_t r) { return r.w <= 0 || r.h <= 0; }

static inline rect_t wm_rect_intersect(rect_t a, rect_t b)
{
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    rect_t r;
    r.x = x0; r.y = y0;
    r.w = x1 > x0 ? x1 - x0 : 0;
    r.h = y1 > y0 ? y1 - y0 : 0;
    return r;
}

static inline int wm_rect_intersects(rect_t a, rect_t b)
{
    return !wm_rect_empty(wm_rect_intersect(a, b));
}

/* Smallest rect containing both (an empty rect contributes nothing). */
static inline rect_t wm_rect_union(rect_t a, rect_t b)
{
    if (wm_rect_empty(a)) return b;
    if (wm_rect_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    rect_t r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

/* ── Damage list (ROADMAP3 18.1) ─────────────────────────────────────────
 *
 * The screen regions that must be recomposited and flushed this frame.
 * Invariant: rects are non-empty, inside the screen, and pairwise
 * non-overlapping (overlapping adds are merged into their union), so no
 * pixel is composited twice.  If the list fills up it collapses to a single
 * bounding rect -- more pixels than needed, but never fewer.
 */
typedef struct {
    rect_t r[WM_DAMAGE_MAX];
    int    n;
    int    sw, sh;      /* screen size: everything is clipped to it */
} wm_damage_t;

static inline void wm_damage_init(wm_damage_t *d, int sw, int sh)
{
    d->n = 0; d->sw = sw; d->sh = sh;
}

static inline void wm_damage_add(wm_damage_t *d, rect_t r)
{
    rect_t scr = { 0, 0, d->sw, d->sh };
    r = wm_rect_intersect(r, scr);
    if (wm_rect_empty(r)) return;

    /* Absorb every existing rect that overlaps r; the grown r may now
     * overlap rects it missed before, so rescan until nothing merges. */
    int merged = 1;
    while (merged) {
        merged = 0;
        for (int i = 0; i < d->n; i++) {
            if (wm_rect_intersects(d->r[i], r)) {
                r = wm_rect_union(r, d->r[i]);
                d->r[i] = d->r[--d->n];
                merged = 1;
                break;
            }
        }
    }

    if (d->n == WM_DAMAGE_MAX) {
        /* Full: collapse everything into one bounding rect. */
        rect_t all = r;
        for (int i = 0; i < d->n; i++) all = wm_rect_union(all, d->r[i]);
        d->r[0] = all;
        d->n = 1;
        return;
    }
    d->r[d->n++] = r;
}

static inline void wm_damage_add_all(wm_damage_t *d)
{
    rect_t scr = { 0, 0, d->sw, d->sh };
    d->n = 0;
    wm_damage_add(d, scr);
}

/* Damage rect covering the cursor drawn at (mx, my). */
static inline rect_t wm_cursor_rect(int mx, int my)
{
    rect_t r = { mx, my, WM_CURSOR_W, WM_CURSOR_H };
    return r;
}

/* ── Tick scheduling (ROADMAP3 18.1) ─────────────────────────────────────
 * Times are PIT ticks (getticks()); hz = gethz().  Comparisons are
 * wrap-safe. */

static inline uint32_t wm_ms_to_ticks(int ms, uint32_t hz)
{
    if (ms <= 0 || hz == 0) return 0;
    uint32_t t = (uint32_t)(((uint64_t)(uint32_t)ms * hz + 999u) / 1000u);
    return t ? t : 1;
}

static inline int wm_tick_due(const window_t *w, uint32_t now)
{
    return w->id != 0 && w->on_tick && w->tick_ms > 0 &&
           (int32_t)(now - w->next_tick) >= 0;
}

/* Arm the next timed tick tick_ms after `now`. */
static inline void wm_tick_rearm(window_t *w, uint32_t now, uint32_t hz)
{
    w->next_tick = now + wm_ms_to_ticks(w->tick_ms, hz);
}

/*
 * wm_next_timeout_ms -- how long the loop may sleep: milliseconds until the
 * earliest timed tick is due (0 if one is already due), or -1 if no window
 * has a timed tick (sleep until input).
 */
static inline int wm_next_timeout_ms(const wm_state_t *s, uint32_t now,
                                     uint32_t hz)
{
    int best = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        const window_t *w = &s->windows[i];
        if (w->id == 0 || !w->on_tick || w->tick_ms <= 0) continue;
        int32_t dt = (int32_t)(w->next_tick - now);
        int ms;
        if (dt <= 0 || hz == 0) ms = 0;
        else ms = (int)(((uint64_t)(uint32_t)dt * 1000u + hz - 1) / hz);
        if (best < 0 || ms < best) best = ms;
    }
    return best;
}

/* ── Compositor declarations ────────────────────────────────────────────── */

void wm_composite(wm_state_t *s, canvas_t *screen);
/* Recomposite only `clip` (desktop + windows, back to front), leaving the
 * screen's clip rect cleared afterwards.  Does not touch w->dirty.
 * ROADMAP3 18.1. */
void wm_composite_rect(wm_state_t *s, canvas_t *screen, rect_t clip);
void wm_draw_cursor(canvas_t *screen, int mx, int my);
void wm_draw_taskbar(canvas_t *screen, int sw, int sh, int taskbar_h,
                     const char **labels, int nlabels, int btn_w);

/* Launch a text viewer window for the given file path. */
void wm_open_textview(const char *path);

#endif /* _USER_WM_WM_H */
