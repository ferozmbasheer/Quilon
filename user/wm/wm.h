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
/* Slots app windows may not take, kept free for WM-owned dialogs (the Power
 * dialog), so the desktop can always be shut down even with every app slot
 * full.  App launches go through wm_create_app_window. */
#define WM_RESERVED_SLOTS 1
#define WM_TITLE_LEN     32
#define TITLEBAR_H       20   /* pixels */
#define WM_CLOSE_BTN_W   16
#define WM_CLOSE_BTN_H   16
#define WM_BORDER_W       1
#define WM_MIN_W         96   /* room for the title + 3 title buttons   */
#define WM_MIN_H         32

/* Title-bar buttons (ROADMAP3 18.3), right to left: close, maximize,
 * minimize.  All WM_CLOSE_BTN_W x WM_CLOSE_BTN_H, WM_TBTN_GAP apart. */
#define WM_TBTN_GAP       2
#define WM_TBTN_CLOSE     0
#define WM_TBTN_MAX       1
#define WM_TBTN_MIN       2

/* Resize grip: a square in the bottom-right corner of the content. */
#define WM_GRIP_SZ       12

/* Cascade placement of new windows (full-rect top-left of the first one;
 * each next one is offset by a title-bar height). */
#define WM_CASCADE_X0    40
#define WM_CASCADE_Y0    60
#define WM_CASCADE_STEP  TITLEBAR_H

/* At least this much of a dragged window's title bar stays on screen. */
#define WM_DRAG_KEEP     64

/* Double-click on a title bar: two presses this close in time and space. */
#define WM_DBLCLICK_MS   400
#define WM_DBLCLICK_PX     4

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
#define WM_TBAR_TEXT_UNFOC  GFX_RGB(196, 196, 200)   /* dimmed title text */

/* Title-bar buttons: bevelled gray with a black glyph; the close button
 * turns red (white glyph) while the cursor is over it. */
#define WM_CLOSE_COL        GFX_RGB(180,  50,  50)   /* close, hovered */
#define WM_TBTN_GLYPH       GFX_BLACK

/* Resize outline drawn while dragging the grip. */
#define WM_OUTLINE_DARK     GFX_BLACK
#define WM_OUTLINE_LIGHT    GFX_WHITE

/* 3D bevel colours (used for window chrome and taskbar buttons) */
#define WM_FRAME_BASE       GFX_RGB(207, 207, 207)   /* light gray fill */
#define WM_FRAME_HI         GFX_WHITE                /* top / left highlight */
#define WM_FRAME_MID        GFX_RGB(160, 160, 160)   /* inner shadow */
#define WM_FRAME_SHADOW     GFX_RGB( 64,  64,  64)   /* bottom / right shadow */

/* Taskbar */
#define WM_TASKBAR_COL      GFX_RGB(191, 191, 191)   /* light gray */
#define WM_TASKBTN_COL      WM_FRAME_BASE
#define WM_TASKBTN_TEXT     GFX_BLACK
#define WM_TASKBTN_DOWN     GFX_RGB(224, 224, 224)   /* pressed (focused) */
#define WM_TASKBTN_DIM      GFX_RGB(110, 110, 110)   /* minimized window */

/* ── Event types (WM → app) ────────────────────────────────────────────── */

#include "wm_keys.h"

/*
 * ROADMAP3 18.2 -- richer events.  Mouse x/y are relative to the window's
 * content origin (and may lie outside it during a drag).
 *
 *   WM_EV_KEY        keycode + mods + ascii, decoded by the WM from the
 *                    kernel's byte stream (wm_keys.h); seq/seq_len hold the
 *                    raw bytes (a terminal forwards those to its shell).
 *   WM_EV_MOUSE_BTN  a button went DOWN inside the content.  `button` is the
 *                    one that changed, `buttons` the state after it.
 *   WM_EV_MOUSE_UP   that button went up again -- delivered to the window
 *                    that got the press, wherever the cursor is now.
 *   WM_EV_MOUSE_MOVE motion while a button pressed in this window is held
 *                    (drag-select).  Not sent for plain hovering.
 *   WM_EV_SCROLL     mouse wheel over the window: `wheel` notches,
 *                    > 0 = towards the user (scroll down), < 0 = up.
 *   WM_EV_CLOSE      the window's close box (or Alt-F4).
 *   WM_EV_RESIZE     the content area changed size (resize grip, maximize,
 *                    restore).  `width` x `height` is the new size; the WM
 *                    has already given the window a new backbuf of that size
 *                    (holding the old pixels top-left) -- re-lay out and
 *                    redraw into it.  ROADMAP3 18.3.
 */
typedef enum {
    WM_EV_NONE       = 0,
    WM_EV_KEY        = 1,
    WM_EV_MOUSE_MOVE = 2,
    WM_EV_MOUSE_BTN  = 3,
    WM_EV_CLOSE      = 4,
    WM_EV_MOUSE_UP   = 5,
    WM_EV_SCROLL     = 6,
    WM_EV_RESIZE     = 7,
} wm_ev_type_t;

typedef struct {
    wm_ev_type_t type;
    int          x;
    int          y;
    uint8_t      buttons;     /* MOUSE_BTN_* state (after this event)     */
    uint8_t      button;      /* MOUSE_BTN / MOUSE_UP: the button changed */
    uint8_t      clicks;      /* MOUSE_BTN: 2 = second press of a double-click
                                 (same window + button, WM_DBLCLICK_MS/PX),
                                 else 1.  ROADMAP3 18.5. */
    char         ascii;       /* KEY: the character, 0 for special keys  */
    uint8_t      mods;        /* KEY: WM_MOD_* (wm_keys.h)                */
    int          keycode;     /* KEY: WM_KEY_* or the (lower-case) char   */
    int          wheel;       /* SCROLL: notches                          */
    int          width;       /* RESIZE: new content size                 */
    int          height;
    char         seq[WM_KEY_SEQ_MAX];   /* KEY: raw bytes                 */
    int          seq_len;
} wm_event_t;

/* A zeroed event of the given type (every field defined). */
static inline wm_event_t wm_event_make(wm_ev_type_t type)
{
    wm_event_t ev;
    unsigned char *p = (unsigned char *)&ev;
    for (unsigned i = 0; i < sizeof ev; i++) p[i] = 0;
    ev.type = type;
    return ev;
}

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

    /* Window operations (ROADMAP3 18.3). */
    int            resizable;        /* 1 = grip + maximize (default)       */
    int            minimized;        /* 1 = hidden; still alive and ticking */
    int            maximized;        /* 1 = fills the screen                */
    rect_t         restore;          /* bounds to go back to from maximized */
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

    /* Grip resize in progress (ROADMAP3 18.3): only an outline follows the
     * cursor; the window takes resize_rect's size on release. */
    int      resize_win_id; /* -1 = none                                  */
    int      resize_ox;     /* content's bottom-right minus press point   */
    int      resize_oy;
    rect_t   resize_rect;   /* proposed content bounds                    */

    int      hover_close_id; /* window whose close button is hovered, 0  */
    int      place_x;       /* full-rect top-left of the last cascaded    */
    int      place_y;       /*   window; place_x < 0 = none yet           */
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

/*
 * wm_tbtn_rect -- a title-bar button (WM_TBTN_CLOSE / _MAX / _MIN), packed
 * right to left.  A window that can't be resized has no maximize button
 * (an empty rect) and its minimize button moves into that place.
 */
static inline rect_t wm_tbtn_rect(const window_t *w, int which)
{
    rect_t tb = wm_titlebar_rect(w);
    rect_t r = { 0, 0, 0, 0 };
    int pos = which;
    if (which == WM_TBTN_MAX && !w->resizable) return r;
    if (which == WM_TBTN_MIN && !w->resizable) pos = 1;
    r.x = tb.x + tb.w - (pos + 1) * (WM_CLOSE_BTN_W + WM_TBTN_GAP);
    r.y = tb.y + (TITLEBAR_H - WM_CLOSE_BTN_H) / 2;
    r.w = WM_CLOSE_BTN_W;
    r.h = WM_CLOSE_BTN_H;
    return r;
}

static inline rect_t wm_close_btn_rect(const window_t *w)
{
    return wm_tbtn_rect(w, WM_TBTN_CLOSE);
}

/* The resize grip (bottom-right of the content), or an empty rect for a
 * window that can't be resized right now (fixed-size, or maximized). */
static inline rect_t wm_grip_rect(const window_t *w)
{
    rect_t r = { 0, 0, 0, 0 };
    if (!w->resizable || w->maximized) return r;
    r.x = w->bounds.x + w->bounds.w - WM_GRIP_SZ;
    r.y = w->bounds.y + w->bounds.h - WM_GRIP_SZ;
    r.w = WM_GRIP_SZ;
    r.h = WM_GRIP_SZ;
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

static inline int wm_in_tbtn(const window_t *w, int which, int mx, int my)
{
    return wm_rect_contains(wm_tbtn_rect(w, which), mx, my);
}

static inline int wm_in_grip(const window_t *w, int mx, int my)
{
    return wm_rect_contains(wm_grip_rect(w), mx, my);
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
        s->windows[i].resizable   = 1;
        s->windows[i].minimized   = 0;
        s->windows[i].maximized   = 0;
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
    s->resize_win_id = -1;
    s->resize_ox     = 0;
    s->resize_oy     = 0;
    s->resize_rect.x = s->resize_rect.y = s->resize_rect.w = s->resize_rect.h = 0;
    s->hover_close_id = 0;
    s->place_x       = -1;
    s->place_y       = -1;
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
    win->resizable    = 1;
    win->minimized    = 0;
    win->maximized    = 0;
    win->restore      = win->bounds;

    /* Copy title — avoid string.h dependency. */
    for (i = 0; i < WM_TITLE_LEN - 1 && title && title[i]; i++)
        win->title[i] = title[i];
    win->title[i] = '\0';

    /* New window goes to the front of z_order. */
    s->z_order[s->num_windows] = slot;
    s->num_windows++;
    return win->id;
}

/*
 * wm_create_app_window -- wm_create_window for ordinary app windows: fails
 * (-1) once only the WM_RESERVED_SLOTS slots are left, so a WM dialog such as
 * Power can still open.
 */
static inline int wm_create_app_window(wm_state_t *s, const char *title,
                                       int x, int y, int w, int h)
{
    if (s->num_windows >= WM_MAX_WINDOWS - WM_RESERVED_SLOTS) return -1;
    return wm_create_window(s, title, x, y, w, h);
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
 * wm_focus_top -- give focus to the frontmost window that isn't minimized
 * (none if every window is).  Returns its slot, or -1.
 */
static inline int wm_focus_top(wm_state_t *s)
{
    int i, top = -1;
    for (i = s->num_windows - 1; i >= 0; i--) {
        if (!s->windows[s->z_order[i]].minimized) { top = s->z_order[i]; break; }
    }
    for (i = 0; i < WM_MAX_WINDOWS; i++)
        s->windows[i].focused = (i == top) ? 1 : 0;
    return top;
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
    w->minimized  = 0;
    w->maximized  = 0;
    w->resizable  = 1;

    s->num_windows--;

    /* Transfer focus to the new frontmost (visible) window if needed. */
    if (had_focus) wm_focus_top(s);

    if (s->drag_win_id == id)
        s->drag_win_id = -1;
    if (s->resize_win_id == id)
        s->resize_win_id = -1;
    if (s->hover_close_id == id)
        s->hover_close_id = 0;
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
        if (s->windows[slot].minimized) continue;
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

/* ── Minimize / maximize (ROADMAP3 18.3) ───────────────────────────────── */

/*
 * wm_minimize -- hide the window at `slot`.  It stays alive (ticking, its
 * shell running) but is not drawn, hit-tested or focused.  It moves to the
 * BACK of z_order, so the next Alt-Tab brings it straight back; focus goes
 * to the frontmost visible window.
 */
static inline void wm_minimize(wm_state_t *s, int slot)
{
    window_t *w = &s->windows[slot];
    if (w->id == 0 || w->minimized) return;
    int i, z_pos = -1;
    for (i = 0; i < s->num_windows; i++)
        if (s->z_order[i] == slot) { z_pos = i; break; }
    if (z_pos < 0) return;
    for (i = z_pos; i > 0; i--)
        s->z_order[i] = s->z_order[i - 1];
    s->z_order[0] = slot;

    w->minimized = 1;
    w->focused   = 0;
    if (s->drag_win_id == w->id)    s->drag_win_id = -1;
    if (s->resize_win_id == w->id)  s->resize_win_id = -1;
    if (s->hover_close_id == w->id) s->hover_close_id = 0;
    wm_focus_top(s);
}

/* wm_unminimize -- show a minimized window again, raised and focused. */
static inline void wm_unminimize(wm_state_t *s, int slot)
{
    window_t *w = &s->windows[slot];
    if (w->id == 0) return;
    w->minimized = 0;
    w->dirty     = 1;
    wm_raise(s, slot);
    wm_focus(s, slot);
}

/* The content bounds of a maximized window: the whole screen (the WM's
 * screen_h already excludes the taskbar) below one title bar. */
static inline rect_t wm_maximized_bounds(const wm_state_t *s)
{
    rect_t r = { 0, TITLEBAR_H, s->screen_w, s->screen_h - TITLEBAR_H };
    return r;
}

/*
 * wm_toggle_maximize -- maximize the window at `slot`, remembering its
 * bounds, or put a maximized one back where it was.  Only bounds change
 * here; the WM notices the new size and reallocates the backbuf + sends
 * WM_EV_RESIZE.  A fixed-size window is left alone.
 */
static inline void wm_toggle_maximize(wm_state_t *s, int slot)
{
    window_t *w = &s->windows[slot];
    if (w->id == 0 || !w->resizable) return;
    if (w->maximized) {
        w->bounds    = w->restore;
        w->maximized = 0;
    } else {
        w->restore   = w->bounds;
        w->bounds    = wm_maximized_bounds(s);
        w->maximized = 1;
    }
    w->dirty = 1;
    if (s->drag_win_id == w->id) s->drag_win_id = -1;
}

/*
 * wm_cycle_focus -- Alt-Tab (ROADMAP3 18.2): raise and focus the BACKMOST
 * window -- un-minimizing it if it was minimized (minimize puts a window at
 * the back, so Alt-Tab right after it restores it).  Repeating it visits
 * every window in turn (the old front window moves back one place each
 * time).  Returns the newly focused slot, or -1 with no windows.
 */
static inline int wm_cycle_focus(wm_state_t *s)
{
    if (s->num_windows <= 0) return -1;
    int slot = s->z_order[0];
    if (s->windows[slot].minimized) {
        wm_unminimize(s, slot);
        return slot;
    }
    wm_raise(s, slot);
    wm_focus(s, slot);
    return slot;
}

/* ── Placement (ROADMAP3 18.3) ────────────────────────────────────────────
 * wm_cascade_pos -- where to put a new w x h window: one title-bar height
 * down-right of the last cascaded window (WM_CASCADE_X0/Y0 for the first,
 * or when no windows are open), wrapping back there when it would run off
 * the screen.  Returns the CONTENT origin (as wm_create_window takes it). */
static inline void wm_cascade_pos(wm_state_t *s, int w, int h,
                                  int *out_x, int *out_y)
{
    int x = WM_CASCADE_X0, y = WM_CASCADE_Y0;
    if (s->num_windows > 0 && s->place_x >= 0) {
        x = s->place_x + WM_CASCADE_STEP;
        y = s->place_y + WM_CASCADE_STEP;
        if (x + w > s->screen_w || y + TITLEBAR_H + h > s->screen_h) {
            x = WM_CASCADE_X0;
            y = WM_CASCADE_Y0;
        }
    }
    s->place_x = x;
    s->place_y = y;
    *out_x = x;
    *out_y = y + TITLEBAR_H;
}

/* ── Resize (ROADMAP3 18.3) ────────────────────────────────────────────── */

/*
 * wm_resize_bounds -- the content bounds window `w` would get with its grip
 * dragged to (mx, my): the top-left stays put; the size follows the cursor
 * (keeping the press offset), clamped to WM_MIN_W x WM_MIN_H and so the
 * window doesn't grow past the screen's right/bottom edge.
 */
static inline rect_t wm_resize_bounds(const wm_state_t *s, const window_t *w,
                                      int mx, int my)
{
    rect_t r = w->bounds;
    r.w = mx + s->resize_ox - r.x;
    r.h = my + s->resize_oy - r.y;
    if (r.w > s->screen_w - r.x) r.w = s->screen_w - r.x;
    if (r.h > s->screen_h - r.y) r.h = s->screen_h - r.y;
    if (r.w < WM_MIN_W) r.w = WM_MIN_W;
    if (r.h < WM_MIN_H) r.h = WM_MIN_H;
    return r;
}

/* The on-screen outline for a resize in progress (title bar + content). */
static inline rect_t wm_resize_outline(const wm_state_t *s)
{
    rect_t r = s->resize_rect;
    r.y -= TITLEBAR_H;
    r.h += TITLEBAR_H;
    return r;
}

/* ── Drag clamping (ROADMAP3 18.3) ────────────────────────────────────────
 * Content origin for a window dragged so its title bar's top-left would be
 * at (tx, ty): at least WM_DRAG_KEEP pixels of the title bar stay on screen
 * horizontally (so it can always be grabbed again), and the whole title bar
 * stays inside the screen vertically. */
static inline void wm_clamp_drag(const wm_state_t *s, const window_t *w,
                                 int tx, int ty, int *out_x, int *out_y)
{
    int keep = WM_DRAG_KEEP < w->bounds.w ? WM_DRAG_KEEP : w->bounds.w;
    if (tx < keep - w->bounds.w)   tx = keep - w->bounds.w;
    if (tx > s->screen_w - keep)   tx = s->screen_w - keep;
    if (ty > s->screen_h - TITLEBAR_H) ty = s->screen_h - TITLEBAR_H;
    if (ty < 0)                    ty = 0;
    *out_x = tx;
    *out_y = ty + TITLEBAR_H;
}

/* Two presses `dt` PIT ticks apart and (dx, dy) pixels apart: double-click? */
static inline int wm_is_double_click(uint32_t dt, uint32_t hz, int dx, int dy)
{
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return (uint64_t)dt * 1000u <= (uint64_t)WM_DBLCLICK_MS * hz &&
           dx <= WM_DBLCLICK_PX && dy <= WM_DBLCLICK_PX;
}

/* Window id whose close button is under (mx, my) -- only if that window is
 * the one actually hit there -- or 0.  Drives the close-button hover. */
static inline int wm_hover_close(const wm_state_t *s, int mx, int my)
{
    int hit = wm_hittest(s, mx, my);
    if (hit < 0) return 0;
    return wm_in_close_btn(&s->windows[hit], mx, my) ? s->windows[hit].id : 0;
}

/*
 * wm_button_edges -- the button transitions since the last mouse sample, in
 * the order they happened.  `prev`/`now` are the button states at the last
 * and this sample; `pressed`/`released` are the edges the kernel latched in
 * between (so a click shorter than one WM iteration is not lost).  Writes up
 * to 3 transitions per button to out[] (buttons in MOUSE_BTN order) and
 * returns how many.
 */
typedef struct { uint8_t button; uint8_t down; } wm_btn_edge_t;
#define WM_BTN_EDGES_MAX 9

static inline int wm_button_edges(uint8_t prev, uint8_t now, uint8_t pressed,
                                  uint8_t released, wm_btn_edge_t *out)
{
    static const uint8_t bits[3] = { 0x01, 0x02, 0x04 };  /* L, R, M */
    int n = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t b = bits[i];
        int state = (prev & b) != 0, want = (now & b) != 0;
        int p = (pressed & b) != 0, r = (released & b) != 0;
        /* Replay the latched edges from the previous state. */
        if (!state && p)  { out[n++] = (wm_btn_edge_t){ b, 1 }; state = 1; p = 0; }
        if (state && r)   { out[n++] = (wm_btn_edge_t){ b, 0 }; state = 0; r = 0; }
        if (!state && (p || (pressed & b)) && want) {
            out[n++] = (wm_btn_edge_t){ b, 1 }; state = 1;  /* press again */
        }
        /* Whatever was missed, end in the sampled state. */
        if (state != want) out[n++] = (wm_btn_edge_t){ b, (uint8_t)want };
    }
    return n;
}

/* ── Mouse event handling ──────────────────────────────────────────────── */

/*
 * wm_handle_mouse_down -- process a (left) button press.
 *
 * Raises and focuses the hit window, then by where the click landed:
 * close button -> destroy; maximize -> toggle maximize; minimize ->
 * minimize; resize grip -> start a resize; elsewhere in the title bar ->
 * start a drag (not while maximized).
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
    if (wm_in_tbtn(w, WM_TBTN_MAX, mx, my)) {
        wm_toggle_maximize(s, idx);
        return;
    }
    if (wm_in_tbtn(w, WM_TBTN_MIN, mx, my)) {
        wm_minimize(s, idx);
        return;
    }
    if (wm_in_grip(w, mx, my)) {
        s->resize_win_id = win_id;
        s->resize_ox     = w->bounds.x + w->bounds.w - mx;
        s->resize_oy     = w->bounds.y + w->bounds.h - my;
        s->resize_rect   = w->bounds;
        return;
    }

    if (wm_in_titlebar(w, mx, my) && !w->maximized) {
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

    if (s->resize_win_id >= 0) {
        window_t *rw = wm_find_window(s, s->resize_win_id);
        if (!rw) { s->resize_win_id = -1; return; }
        s->resize_rect = wm_resize_bounds(s, rw, mx, my);
        return;
    }

    if (s->drag_win_id < 0) return;

    int idx = wm_find_idx(s, s->drag_win_id);
    if (idx < 0) { s->drag_win_id = -1; return; }

    window_t *w = &s->windows[idx];

    /* Keep (enough of) the title bar on screen. */
    int new_x, new_y;
    wm_clamp_drag(s, w, mx - s->drag_ox, my - s->drag_oy, &new_x, &new_y);

    w->bounds.x = new_x;
    w->bounds.y = new_y;
    w->dirty    = 1;
}

/*
 * wm_handle_mouse_up -- end the current drag, or finish a resize: the
 * window takes the outline's size (the WM then reallocates its backbuf
 * and sends WM_EV_RESIZE).
 */
static inline void wm_handle_mouse_up(wm_state_t *s)
{
    s->mouse_buttons = 0;
    s->drag_win_id   = -1;
    if (s->resize_win_id >= 0) {
        window_t *w = wm_find_window(s, s->resize_win_id);
        if (w) {
            w->bounds.w = s->resize_rect.w;
            w->bounds.h = s->resize_rect.h;
            w->dirty    = 1;
        }
        s->resize_win_id = -1;
    }
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

/* Damage the 2-px frame of rect r (the resize outline) as four strips that
 * don't overlap each other, rather than the whole rect. */
#define WM_OUTLINE_W 2
static inline void wm_damage_frame(wm_damage_t *d, rect_t r)
{
    int t = WM_OUTLINE_W;
    rect_t top = { r.x, r.y, r.w, t };
    rect_t bot = { r.x, r.y + r.h - t, r.w, t };
    rect_t lft = { r.x, r.y + t, t, r.h - 2 * t };
    rect_t rgt = { r.x + r.w - t, r.y + t, t, r.h - 2 * t };
    wm_damage_add(d, top);
    wm_damage_add(d, bot);
    wm_damage_add(d, lft);
    wm_damage_add(d, rgt);
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

/* ── Scrolling a list view (textview, Files) -- ROADMAP3 18.2 ─────────────
 * New first-visible row after moving `delta` rows, for a list of `total`
 * rows shown `rows` at a time: never past the end, never below 0. */
#define WM_WHEEL_ROWS 3          /* rows per wheel notch */

static inline int wm_scroll_clamp(int scroll, int delta, int total, int rows)
{
    int max = total - rows;
    if (max < 0) max = 0;
    scroll += delta;
    if (scroll > max) scroll = max;
    if (scroll < 0)   scroll = 0;
    return scroll;
}

/* ── Taskbar Power button + power-off dialog layout ───────────────────────
 * The Power button sits at the right end of the taskbar; clicking it opens a
 * small confirmation dialog with two buttons.  Pure geometry, host-tested. */

#define WM_POWER_BTN_W     72
#define WM_POWER_LABEL     "Power"
#define WM_POWER_DLG_W    264      /* dialog content size */
#define WM_POWER_DLG_H     88
#define WM_DLG_BTN_W       96
#define WM_DLG_BTN_H       24
#define WM_DLG_BTN_GAP     16

/* The Power button's screen rect (inset like the other taskbar buttons). */
static inline rect_t wm_taskbar_power_rect(int sw, int sh, int taskbar_h)
{
    rect_t r = { sw - WM_POWER_BTN_W + 2, sh - taskbar_h + 3,
                 WM_POWER_BTN_W - 4, taskbar_h - 6 };
    return r;
}

/* ── Taskbar: Apps menu, window list, tray (ROADMAP3 18.4) ─────────────
 * Left to right: the Apps (start) button, one button per open window, the
 * tray clock, the Power button.  The Apps button opens a menu of the app
 * table just above it.  Pure geometry + logic, host-tested. */

#define WM_START_BTN_W     64
#define WM_START_LABEL     "Apps"
#define WM_CLOCK_W         60      /* tray clock: "HH:MM" in a sunken box */
#define WM_WINBTN_MAX_W   160      /* a window button never grows past this */
#define WM_WINBTN_MIN_W    24      /* narrower than this: not shown */
#define WM_MENU_W         160
#define WM_MENU_ITEM_H     22

static inline rect_t wm_taskbar_start_rect(int sh, int taskbar_h)
{
    rect_t r = { 2, sh - taskbar_h + 3, WM_START_BTN_W - 4, taskbar_h - 6 };
    return r;
}

/* The tray clock, just left of the Power button. */
static inline rect_t wm_taskbar_clock_rect(int sw, int sh, int taskbar_h)
{
    rect_t r = { sw - WM_POWER_BTN_W - WM_CLOCK_W + 2, sh - taskbar_h + 3,
                 WM_CLOCK_W - 4, taskbar_h - 6 };
    return r;
}

/* Button i of n in the window list: the strip between the Apps button and
 * the tray, shared equally (at most WM_WINBTN_MAX_W each).  An empty rect
 * when the strip is too narrow to show it. */
static inline rect_t wm_winlist_btn_rect(int i, int n, int sw, int sh,
                                         int taskbar_h)
{
    rect_t none = { 0, 0, 0, 0 };
    int x0 = WM_START_BTN_W;
    int x1 = sw - WM_POWER_BTN_W - WM_CLOCK_W;
    if (n <= 0 || i < 0 || i >= n || x1 <= x0) return none;
    int bw = (x1 - x0) / n;
    if (bw > WM_WINBTN_MAX_W) bw = WM_WINBTN_MAX_W;
    if (bw < WM_WINBTN_MIN_W) return none;
    rect_t r = { x0 + i * bw + 2, sh - taskbar_h + 3, bw - 4, taskbar_h - 6 };
    return r;
}

/* Index of the window-list button under (mx, my), or -1. */
static inline int wm_winlist_hit(int mx, int my, int n, int sw, int sh,
                                 int taskbar_h)
{
    for (int i = 0; i < n; i++)
        if (wm_rect_contains(wm_winlist_btn_rect(i, n, sw, sh, taskbar_h),
                             mx, my))
            return i;
    return -1;
}

/* Slots of the open windows in creation (id) order -- a stable taskbar
 * order that raising or focusing doesn't shuffle.  Returns the count. */
static inline int wm_winlist(const wm_state_t *s, int out[WM_MAX_WINDOWS])
{
    int n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (s->windows[i].id == 0) continue;
        int j = n++;
        while (j > 0 && s->windows[out[j - 1]].id > s->windows[i].id) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = i;
    }
    return n;
}

/* A click on a window's taskbar button: restore it if minimized, minimize
 * it if it already has the focus, otherwise raise + focus it. */
#define WM_WL_NONE       0
#define WM_WL_RESTORED   1
#define WM_WL_MINIMIZED  2
#define WM_WL_FOCUSED    3
static inline int wm_winlist_click(wm_state_t *s, int slot)
{
    if (slot < 0 || slot >= WM_MAX_WINDOWS) return WM_WL_NONE;
    window_t *w = &s->windows[slot];
    if (w->id == 0) return WM_WL_NONE;
    if (w->minimized) { wm_unminimize(s, slot); return WM_WL_RESTORED; }
    if (w->focused)   { wm_minimize(s, slot);   return WM_WL_MINIMIZED; }
    wm_raise(s, slot);
    wm_focus(s, slot);
    return WM_WL_FOCUSED;
}

/* The text on a window's taskbar button: the title's last path component
 * (a text viewer keeps the file's full path as its title). */
static inline const char *wm_title_label(const char *title)
{
    const char *l = title;
    for (const char *p = title; *p; p++)
        if (*p == '/' && p[1]) l = p + 1;
    return l;
}

/* "HH:MM" (UTC) for Unix time t, or "--:--" when the clock is unknown. */
static inline void wm_format_hhmm(uint32_t t, char out[6])
{
    if (t == 0) {
        out[0] = out[1] = out[3] = out[4] = '-';
    } else {
        uint32_t h = (t / 3600) % 24, m = (t / 60) % 60;
        out[0] = (char)('0' + h / 10); out[1] = (char)('0' + h % 10);
        out[3] = (char)('0' + m / 10); out[4] = (char)('0' + m % 10);
    }
    out[2] = ':';
    out[5] = '\0';
}

/* The Apps menu: n items stacked just above the Apps button. */
static inline rect_t wm_menu_rect(int n, int sh, int taskbar_h)
{
    int h = n * WM_MENU_ITEM_H + 4;
    rect_t r = { 0, sh - taskbar_h - h, WM_MENU_W, h };
    return r;
}

static inline rect_t wm_menu_item_rect(int i, int n, int sh, int taskbar_h)
{
    rect_t m = wm_menu_rect(n, sh, taskbar_h);
    rect_t r = { m.x + 2, m.y + 2 + i * WM_MENU_ITEM_H, m.w - 4, WM_MENU_ITEM_H };
    return r;
}

/* Index of the menu item under (mx, my), or -1. */
static inline int wm_menu_hit(int mx, int my, int n, int sh, int taskbar_h)
{
    for (int i = 0; i < n; i++)
        if (wm_rect_contains(wm_menu_item_rect(i, n, sh, taskbar_h), mx, my))
            return i;
    return -1;
}

/* Everything the taskbar shows, so the loop can tell (by hash) when it has
 * to be repainted and the compositor can draw it. */
typedef struct {
    int         n;                          /* window buttons */
    int         slot[WM_MAX_WINDOWS];
    const char *label[WM_MAX_WINDOWS];
    uint8_t     pressed[WM_MAX_WINDOWS];    /* focused window */
    uint8_t     minimized[WM_MAX_WINDOWS];
    int         start_down;                 /* Apps menu open */
    char        clock[6];                   /* "HH:MM" */
} wm_taskbar_view_t;

static inline void wm_taskbar_view(const wm_state_t *s, int menu_open,
                                   uint32_t now, wm_taskbar_view_t *v)
{
    v->n = wm_winlist(s, v->slot);
    for (int i = 0; i < v->n; i++) {
        const window_t *w = &s->windows[v->slot[i]];
        v->label[i]     = wm_title_label(w->title);
        v->pressed[i]   = (uint8_t)(w->focused && !w->minimized);
        v->minimized[i] = (uint8_t)w->minimized;
    }
    v->start_down = menu_open ? 1 : 0;
    wm_format_hhmm(now, v->clock);
}

/* FNV-1a over what the taskbar would draw: equal hashes = nothing to redo. */
static inline uint32_t wm_taskbar_view_hash(const wm_taskbar_view_t *v)
{
    uint32_t h = 2166136261u;
#define WM_FNV(b) (h = (h ^ (uint8_t)(b)) * 16777619u)
    WM_FNV(v->n); WM_FNV(v->start_down);
    for (int i = 0; i < 5; i++) WM_FNV(v->clock[i]);
    for (int i = 0; i < v->n; i++) {
        WM_FNV(v->slot[i]); WM_FNV(v->pressed[i]); WM_FNV(v->minimized[i]);
        for (const char *p = v->label[i]; *p; p++) WM_FNV(*p);
        WM_FNV(0);
    }
#undef WM_FNV
    return h;
}

/* Dialog buttons, in content coordinates: 0 = "Power off", 1 = "Cancel",
 * side by side and centred along the bottom of a cw x ch content area. */
#define WM_DLG_BTN_OK      0
#define WM_DLG_BTN_CANCEL  1
static inline rect_t wm_dialog_btn_rect(int which, int cw, int ch)
{
    int total = 2 * WM_DLG_BTN_W + WM_DLG_BTN_GAP;
    int x0 = (cw - total) / 2;
    rect_t r = { x0 + which * (WM_DLG_BTN_W + WM_DLG_BTN_GAP),
                 ch - WM_DLG_BTN_H - 12, WM_DLG_BTN_W, WM_DLG_BTN_H };
    return r;
}

/* Which dialog button content point (x, y) is on, or -1. */
static inline int wm_dialog_btn_hit(int x, int y, int cw, int ch)
{
    for (int b = 0; b < 2; b++)
        if (wm_rect_contains(wm_dialog_btn_rect(b, cw, ch), x, y)) return b;
    return -1;
}

/* ── Compositor declarations ────────────────────────────────────────────── */

void wm_composite(wm_state_t *s, canvas_t *screen);
/* Recomposite only `clip` (desktop + windows, back to front), leaving the
 * screen's clip rect cleared afterwards.  Does not touch w->dirty.
 * ROADMAP3 18.1. */
void wm_composite_rect(wm_state_t *s, canvas_t *screen, rect_t clip);
void wm_draw_cursor(canvas_t *screen, int mx, int my);
/* The 2-px resize outline (WM_OUTLINE_W) around full-window rect r. */
void wm_draw_outline(canvas_t *screen, rect_t r);
/* The taskbar (ROADMAP3 18.4): Apps button, window list, tray clock, Power. */
void wm_draw_taskbar(canvas_t *screen, int sw, int sh, int taskbar_h,
                     const wm_taskbar_view_t *v);
/* The Apps menu (n labels) above the Apps button; `hover` item highlighted
 * (-1 = none). */
void wm_draw_menu(canvas_t *screen, int sh, int taskbar_h,
                  const char *const *labels, int n, int hover);
/* A raised, labelled button (taskbar and dialog buttons share the look). */
void wm_draw_button(canvas_t *c, rect_t r, const char *label);
/* The chrome's 3D bevels, for the widget kit (18.5): raised = light top/left,
 * dark bottom/right; sunken = the reverse.  Only the 1-2 px edges are drawn. */
void wm_bevel_raised(canvas_t *c, rect_t r);
void wm_bevel_sunken(canvas_t *c, rect_t r);
/* Desktop wallpaper (18.5): a screen-sized canvas the compositor copies the
 * desktop from instead of filling it with WM_DESKTOP_COL.  NULL = none. */
void wm_set_wallpaper(canvas_t *bg);

/* Ask the WM loop to power the machine off after the current frame is on
 * screen (the power dialog's "Power off").  Defined in main.c. */
void wm_request_poweroff(void);

/* Launch a text viewer window for the given file path. */
void wm_open_textview(const char *path);
/* Run the program at `path` in a new Terminal window (ROADMAP3 18.5). */
void wm_open_program(const char *path);

#endif /* _USER_WM_WM_H */
