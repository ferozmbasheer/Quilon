/*
 * Quilon OS -- Window Manager & Compositor (section 14.2)
 *
 * This header defines all WM types and pure-logic helpers.  The static inline
 * functions (geometry, hit-testing, z-order, state management) have no
 * hardware or syscall dependencies and are fully testable on the host.
 *
 * The compositor (wm_composite, wm_draw_cursor) is defined in wm/main.c
 * because it calls libgfx functions.  This header only declares them.
 *
 * Z-order model
 * ─────────────
 * The windows[] array is contiguous: slots 0 … num_windows-1 are active,
 * higher slots are inactive.  Slot 0 is the back; slot num_windows-1 is
 * the front.  wm_raise() rotates the raised window to slot num_windows-1.
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

/* Desktop and decoration colours */
#define WM_DESKTOP_COL      GFX_RGB(30,  30,  60 )
#define WM_TBAR_FOCUSED     GFX_RGB(80,  80,  200)
#define WM_TBAR_UNFOCUSED   GFX_RGB(60,  60,  60 )
#define WM_TBAR_TEXT_COL    GFX_WHITE
#define WM_CLOSE_COL        GFX_RED
#define WM_BORDER_COL       GFX_RGB(100, 100, 100)

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

/* ── Event queue (SPSC ring buffer — WM writes, app thread reads) ──────── */

#define WM_EVQUEUE_DEPTH  64

typedef struct {
    volatile int head;                   /* WM increments after writing  */
    volatile int tail;                   /* app increments after reading */
    wm_event_t   buf[WM_EVQUEUE_DEPTH];
} wm_evqueue_t;

static inline void wm_evqueue_push(wm_evqueue_t *q, wm_event_t ev)
{
    int next = (q->head + 1) % WM_EVQUEUE_DEPTH;
    if (next == q->tail) return;         /* full — drop the event         */
    q->buf[q->head] = ev;
    q->head = next;
}

static inline int wm_evqueue_poll(wm_evqueue_t *q, wm_event_t *out)
{
    if (q->tail == q->head) return 0;
    *out = q->buf[q->tail];
    q->tail = (q->tail + 1) % WM_EVQUEUE_DEPTH;
    return 1;
}

/* ── Window descriptor ─────────────────────────────────────────────────── */

typedef struct wm_window_ {
    int            id;               /* unique ID (≥ 1); 0 = slot unused   */
    rect_t         bounds;           /* content area: (x, y, w, h)         */
    char           title[WM_TITLE_LEN];
    canvas_t      *backbuf;         /* app draws here; NULL until assigned */
    int            focused;          /* 1 = has keyboard focus              */
    int            active;           /* 1 = slot occupied                   */
    int            dirty;            /* 1 = backbuf has new pixels          */
    wm_evqueue_t   events;           /* WM → app keyboard / mouse events    */
    void         (*app_fn)(struct wm_window_ *); /* launch fn, or NULL     */
} window_t;

/* ── WM state ──────────────────────────────────────────────────────────── */

typedef struct {
    window_t windows[WM_MAX_WINDOWS];
    int      num_windows;   /* active windows in slots 0…num_windows-1   */
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
        s->windows[i].backbuf     = (canvas_t *)0;
        s->windows[i].app_fn      = (void (*)(struct wm_window_ *))0;
        s->windows[i].events.head = 0;
        s->windows[i].events.tail = 0;
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
 * wm_create_window -- allocate the next contiguous slot (slot num_windows).
 * Returns the new window ID on success, -1 if the table is full.
 * The caller sets w->backbuf after this call.
 */
static inline int wm_create_window(wm_state_t *s, const char *title,
                                   int x, int y, int w, int h)
{
    int i;
    if (s->num_windows >= WM_MAX_WINDOWS) return -1;

    window_t *win = &s->windows[s->num_windows];
    win->id      = s->next_id++;
    win->bounds.x = x;
    win->bounds.y = y;
    win->bounds.w = w < WM_MIN_W ? WM_MIN_W : w;
    win->bounds.h = h < WM_MIN_H ? WM_MIN_H : h;
    win->focused      = 0;
    win->active       = 1;
    win->dirty        = 1;
    win->backbuf      = (canvas_t *)0;
    win->app_fn       = (void (*)(struct wm_window_ *))0;
    win->events.head  = 0;
    win->events.tail  = 0;

    /* Copy title — avoid string.h dependency. */
    for (i = 0; i < WM_TITLE_LEN - 1 && title && title[i]; i++)
        win->title[i] = title[i];
    win->title[i] = '\0';

    s->num_windows++;
    return win->id;
}

/* Return slot index of window with given id, or -1. */
static inline int wm_find_idx(const wm_state_t *s, int id)
{
    int i;
    for (i = 0; i < s->num_windows; i++) {
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
 * Shifts later windows down to keep the array contiguous.
 * If the destroyed window had focus, the new topmost window gets it.
 */
static inline void wm_destroy_window(wm_state_t *s, int id)
{
    int idx = wm_find_idx(s, id);
    if (idx < 0) return;

    int had_focus = s->windows[idx].focused;

    /* Compact the array. */
    int i;
    for (i = idx; i < s->num_windows - 1; i++)
        s->windows[i] = s->windows[i + 1];

    s->num_windows--;
    if (s->num_windows > 0)
        s->windows[s->num_windows].active = 0;

    /* Transfer focus to the new topmost window if needed. */
    if (had_focus && s->num_windows > 0) {
        for (i = 0; i < s->num_windows; i++)
            s->windows[i].focused = 0;
        s->windows[s->num_windows - 1].focused = 1;
    }

    if (s->drag_win_id == id)
        s->drag_win_id = -1;
}

/*
 * wm_hittest -- return slot index of the topmost window under (mx, my), or -1.
 * Iterates front-to-back (highest index first).
 */
static inline int wm_hittest(const wm_state_t *s, int mx, int my)
{
    int i;
    for (i = s->num_windows - 1; i >= 0; i--) {
        if (wm_in_window(&s->windows[i], mx, my))
            return i;
    }
    return -1;
}

/*
 * wm_raise -- move window at slot idx to the front (slot num_windows-1)
 * by rotating the array entries.
 */
static inline void wm_raise(wm_state_t *s, int idx)
{
    if (idx < 0 || idx >= s->num_windows) return;
    if (idx == s->num_windows - 1) return;  /* already at front */

    window_t tmp = s->windows[idx];
    int i;
    for (i = idx; i < s->num_windows - 1; i++)
        s->windows[i] = s->windows[i + 1];
    s->windows[s->num_windows - 1] = tmp;
}

/*
 * wm_focus -- give keyboard focus to the window at slot idx.
 * Clears focus on all other windows.
 */
static inline void wm_focus(wm_state_t *s, int idx)
{
    int i;
    for (i = 0; i < s->num_windows; i++)
        s->windows[i].focused = (i == idx) ? 1 : 0;
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
    /* After raise, the window is at num_windows-1. */
    idx = s->num_windows - 1;
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

/* ── Compositor declarations ────────────────────────────────────────────── */

void wm_composite(wm_state_t *s, canvas_t *screen);
void wm_draw_cursor(canvas_t *screen, int mx, int my);
void wm_draw_taskbar(canvas_t *screen, int sw, int sh, int taskbar_h,
                     const char **labels, int nlabels, int btn_w);

/* Launch a text viewer window for the given file path. */
void wm_open_textview(const char *path);

#endif /* _USER_WM_WM_H */
