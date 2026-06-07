/*
 * Quilon OS -- Window Manager & Desktop Launcher (single-threaded event loop)
 *
 * Architecture
 * ────────────
 * The WM is ONE process running ONE event loop.  Apps are NOT threads: each
 * window registers non-blocking callbacks (on_event / on_tick / on_destroy)
 * that the loop invokes.  Because only the loop ever touches wm_state_t and the
 * framebuffer, there are no shared-state races -- no locks, no SPSC queues, no
 * pthread_join reaping.  Window create/destroy happen synchronously in the loop.
 *
 * Each iteration:
 *   1. poll the mouse (non-blocking)        -> raise/focus/drag/close
 *   2. poll the keyboard (non-blocking)     -> deliver key to focused window
 *   3. tick every window (on_tick)          -> clock updates, pipe draining
 *   4. destroy windows that requested close (want_close)
 *   5. if anything is dirty: composite + taskbar + flush
 *
 * Apps must never block.  Anything that would block (a terminal reading its
 * shell's pipe) is done with a non-blocking poll inside on_tick.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gfx.h>
#include "wm.h"

/* App "open" entry points: each sets up the window's callbacks + app_state.
 * Defined in the respective app .c files. */
void gterm_open(window_t *win);
void clock_open(window_t *win);
void filebr_open(window_t *win);
void textview_open(window_t *win);

/* ── App table ────────────────────────────────────────────────────────────── */

typedef struct {
    const char      *label;
    void           (*open)(window_t *);
    int              w, h;         /* default window content size */
    int              x, y;         /* default window position     */
} app_entry_t;

static const app_entry_t g_apps[] = {
    { "Terminal", gterm_open,   640, 384,  40, 60 },
    { "Clock",    clock_open,   160,  40, 440, 60 },
    { "Files",    filebr_open,  320, 320, 200, 80 },
};
#define G_NOAPPS  ((int)(sizeof g_apps / sizeof g_apps[0]))

#define TASKBAR_H    28
#define TASKBAR_BTN_W 110

/* ── Global WM state (single-threaded; only the loop touches it) ─────────── */

static wm_state_t  g_wm;
static canvas_t   *g_screen;
static int         g_sw, g_sh;   /* screen dimensions */

/* ── Launch an app in a new window ───────────────────────────────────────── */

static void wm_launch(int app_idx)
{
    const app_entry_t *a = &g_apps[app_idx];

    int id = wm_create_window(&g_wm, a->label, a->x, a->y + TITLEBAR_H, a->w, a->h);
    if (id < 0) {
        printf("[wm] no free window slot for '%s'\r\n", a->label);
        return;
    }

    window_t *win = wm_find_window(&g_wm, id);
    if (!win) return;

    win->backbuf = canvas_create(a->w, a->h);
    if (!win->backbuf) {
        wm_destroy_window(&g_wm, id);
        printf("[wm] OOM for '%s' canvas\r\n", a->label);
        return;
    }
    gfx_fill(win->backbuf, GFX_BLACK);

    /* The app installs its callbacks + app_state and draws its first frame. */
    a->open(win);

    printf("[wm] launched '%s' (win id=%d)\r\n", a->label, id);
}

/* ── Open a file in a new textview window ────────────────────────────────── */

void wm_open_textview(const char *path)
{
    /* Use the last component of the path as the window title. */
    const char *title = path;
    const char *p;
    for (p = path; *p; p++)
        if (*p == '/')
            title = p + 1;
    if (*title == '\0')
        title = path;

    int id = wm_create_window(&g_wm, title, 60, 60 + TITLEBAR_H, 560, 360);
    if (id < 0) return;

    window_t *win = wm_find_window(&g_wm, id);
    if (!win) return;

    /* Store the full path in win->title so textview_open can read it. */
    int i;
    for (i = 0; i < WM_TITLE_LEN - 1 && path[i]; i++)
        win->title[i] = path[i];
    win->title[i] = '\0';

    win->backbuf = canvas_create(win->bounds.w, win->bounds.h);
    if (!win->backbuf) { wm_destroy_window(&g_wm, id); return; }
    gfx_fill(win->backbuf, GFX_BLACK);

    textview_open(win);
}

/* ── Deliver a key event to the focused window ───────────────────────────── */

static void deliver_key(char c)
{
    int i;
    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        window_t *w = &g_wm.windows[i];
        if (w->id != 0 && w->focused && w->on_event) {
            wm_event_t ev;
            ev.type    = WM_EV_KEY;
            ev.ascii   = c;
            ev.x = ev.y = 0;
            ev.buttons = 0;
            w->on_event(w, &ev);
            return;
        }
    }
}

/* Ask a window to close: notify the app (so it can flush/stop), then mark it
 * for destruction at the end of this loop iteration. */
static void request_close(window_t *w)
{
    if (w->on_event) {
        wm_event_t ev;
        ev.type = WM_EV_CLOSE;
        ev.ascii = 0; ev.x = ev.y = ev.buttons = 0;
        w->on_event(w, &ev);
    }
    w->want_close = 1;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("[wm] Quilon Desktop starting\r\n");

    /* 1. Map shadow framebuffer. */
    gfx_info_t info;
    g_screen = gfx_screen_init(&info);
    if (!g_screen) {
        printf("[wm] ERROR: gfx_screen_init failed\r\n");
        return 1;
    }
    g_sw = (int)info.width;
    g_sh = (int)info.height;
    printf("[wm] screen %dx%d\r\n", g_sw, g_sh);

    /* 2. Initialise WM state (screen height minus taskbar). */
    wm_state_init(&g_wm, g_sw, g_sh - TASKBAR_H);

    /* 3. Build the static label array for wm_draw_taskbar. */
    const char *labels[G_NOAPPS];
    int i;
    for (i = 0; i < G_NOAPPS; i++) labels[i] = g_apps[i].label;

    /* 4. Initial frame: desktop + taskbar. */
    wm_composite(&g_wm, g_screen);
    wm_draw_taskbar(g_screen, g_sw, g_sh, TASKBAR_H,
                    labels, G_NOAPPS, TASKBAR_BTN_W);
    gfx_flush();

    /* 5. Event loop. */
    mouse_event_t prev = {0};
    while (1) {
        int any_dirty = 0;

        /* -- Mouse --------------------------------------------------------- */
        mouse_event_t me;
        mouse_read(&me);

        int moved   = (me.x != prev.x || me.y != prev.y);
        int pressed = (me.buttons && !prev.buttons);
        int release = (!me.buttons && prev.buttons);

        if (pressed) {
            if (me.y >= g_sh - TASKBAR_H) {
                int idx = me.x / TASKBAR_BTN_W;
                if (idx >= 0 && idx < G_NOAPPS)
                    wm_launch(idx);
            } else {
                int hit = wm_hittest(&g_wm, me.x, me.y);
                if (hit >= 0) {
                    window_t *w = &g_wm.windows[hit];
                    if (wm_in_close_btn(w, me.x, me.y)) {
                        request_close(w);
                    } else {
                        wm_handle_mouse_down(&g_wm, me.x, me.y, me.buttons);
                    }
                }
            }
            any_dirty = 1;
        } else if (release) {
            wm_handle_mouse_up(&g_wm);
        } else if (moved) {
            wm_handle_mouse_move(&g_wm, me.x, me.y);
            any_dirty = 1;
        }
        prev = me;

        /* -- Keyboard (non-blocking) --------------------------------------- */
        char c;
        while (read_nonblock(STDIN_FILENO, &c, 1) == 1)
            deliver_key(c);

        /* -- Tick every window (periodic, non-blocking app work) ----------- */
        for (i = 0; i < WM_MAX_WINDOWS; i++) {
            window_t *w = &g_wm.windows[i];
            if (w->id != 0 && w->on_tick)
                w->on_tick(w);
        }

        /* -- Destroy windows that requested close -------------------------- */
        for (i = 0; i < WM_MAX_WINDOWS; i++) {
            window_t *w = &g_wm.windows[i];
            if (w->id != 0 && w->want_close) {
                wm_destroy_window(&g_wm, w->id);
                any_dirty = 1;
            }
        }

        /* -- Composite if anything changed --------------------------------- */
        for (i = 0; i < g_wm.num_windows; i++) {
            if (g_wm.windows[g_wm.z_order[i]].dirty) { any_dirty = 1; break; }
        }

        if (any_dirty) {
            wm_composite(&g_wm, g_screen);
            wm_draw_taskbar(g_screen, g_sw, g_sh, TASKBAR_H,
                            labels, G_NOAPPS, TASKBAR_BTN_W);
            gfx_flush();
        }
    }

    return 0;
}
