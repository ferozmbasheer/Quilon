/*
 * Quilon OS -- Window Manager & Desktop Launcher (section 14.2 revised)
 *
 * Architecture
 * ────────────
 * All graphical apps run as clone(CLONE_VM) threads of this process via
 * pthread_create().  Because they share the same virtual address space,
 * apps can write directly into their canvas_t* (allocated here in the WM
 * heap) with no IPC.  Keyboard events are forwarded via a per-window
 * wm_evqueue_t — a plain struct in shared memory.
 *
 * Threads
 * ───────
 *   main (WM)    — mouse handling, compositing, taskbar, gfx_flush()
 *   kbd thread   — blocks on read(0,...); routes keys to focused window
 *   app threads  — one per running app (clone(CLONE_VM) via pthread_create)
 *
 * Desktop
 * ───────
 * A taskbar at the bottom of the screen lists the built-in apps.  Clicking
 * a button launches the corresponding app in a new thread+window.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <gfx.h>
#include "wm.h"

/* Forward-declared app entry points (defined in their respective .c files). */
void gterm_app(window_t *win);
void clock_app(window_t *win);
void filebr_app(window_t *win);
void textview_app(window_t *win);

/* ── App table ────────────────────────────────────────────────────────────── */

typedef struct {
    const char      *label;
    void           (*fn)(window_t *);
    int              w, h;         /* default window content size */
    int              x, y;         /* default window position     */
} app_entry_t;

static const app_entry_t g_apps[] = {
    { "Terminal", gterm_app,   640, 384,  40, 60 },
    { "Clock",    clock_app,   160,  40, 440, 60 },
    { "Files",    filebr_app,  320, 320, 200, 80 },
};
#define G_NOAPPS  ((int)(sizeof g_apps / sizeof g_apps[0]))

#define TASKBAR_H    28
#define TASKBAR_BTN_W 110

/* ── Global WM state (shared with clone'd app threads via CLONE_VM) ──────── */

static wm_state_t  g_wm;
static canvas_t   *g_screen;
static int         g_sw, g_sh;   /* screen dimensions */

/* ── App trampoline ──────────────────────────────────────────────────────── */

static void *app_trampoline(void *arg)
{
    window_t *win = (window_t *)arg;
    win->app_fn(win);
    win->active = 0;
    return NULL;
}

/* ── Launch an app in a new window + thread ──────────────────────────────── */

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

    win->app_fn = a->fn;

    pthread_t tid;
    pthread_create(&tid, NULL, app_trampoline, win);

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

    /* Copy the path into win->title so textview_app can open it.
     * title is short enough; path may be longer — store it in the same field
     * (WM_TITLE_LEN is 32; for longer paths we truncate to the filename). */
    int i;
    for (i = 0; i < WM_TITLE_LEN - 1 && path[i]; i++)
        win->title[i] = path[i];
    win->title[i] = '\0';

    win->backbuf = canvas_create(win->bounds.w, win->bounds.h);
    if (!win->backbuf) { wm_destroy_window(&g_wm, id); return; }
    gfx_fill(win->backbuf, GFX_BLACK);

    win->app_fn = textview_app;

    pthread_t tid;
    pthread_create(&tid, NULL, app_trampoline, win);
}

/* ── Keyboard thread ─────────────────────────────────────────────────────── */

static void *kbd_thread(void *arg)
{
    (void)arg;
    char c;
    while (read(0, &c, 1) == 1) {
        int i;
        for (i = g_wm.num_windows - 1; i >= 0; i--) {
            window_t *w = &g_wm.windows[i];
            if (w->focused && w->active) {
                wm_event_t ev;
                ev.type    = WM_EV_KEY;
                ev.ascii   = c;
                ev.x       = 0;
                ev.y       = 0;
                ev.buttons = 0;
                wm_evqueue_push(&w->events, ev);
                break;
            }
        }
    }
    return NULL;
}

/* ── Close-button handling ───────────────────────────────────────────────── */

static void wm_send_close(window_t *w)
{
    wm_event_t ev;
    ev.type    = WM_EV_CLOSE;
    ev.ascii   = 0;
    ev.x       = ev.y = ev.buttons = 0;
    wm_evqueue_push(&w->events, ev);
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

    /* 3. Start keyboard thread. */
    pthread_t kbd_tid;
    pthread_create(&kbd_tid, NULL, kbd_thread, NULL);

    /* 4. Build the static label array for wm_draw_taskbar. */
    const char *labels[G_NOAPPS];
    int i;
    for (i = 0; i < G_NOAPPS; i++) labels[i] = g_apps[i].label;

    /* 5. Initial frame: desktop + taskbar. */
    wm_composite(&g_wm, g_screen);
    wm_draw_taskbar(g_screen, g_sw, g_sh, TASKBAR_H,
                    labels, G_NOAPPS, TASKBAR_BTN_W);
    gfx_flush();

    /* 6. Event loop. */
    mouse_event_t prev = {0};
    while (1) {
        /* -- Mouse --------------------------------------------------------- */
        mouse_event_t me;
        mouse_read(&me);

        int moved   = (me.x != prev.x || me.y != prev.y);
        int pressed = (me.buttons && !prev.buttons);
        int release = (!me.buttons && prev.buttons);

        if (pressed) {
            /* Check taskbar clicks first (below the desktop area). */
            if (me.y >= g_sh - TASKBAR_H) {
                int idx = me.x / TASKBAR_BTN_W;
                if (idx >= 0 && idx < G_NOAPPS)
                    wm_launch(idx);
            } else {
                /* Deliver to WM (raise/focus/drag). */
                int hit = wm_hittest(&g_wm, me.x, me.y);
                if (hit >= 0) {
                    window_t *w = &g_wm.windows[hit];
                    if (wm_in_close_btn(w, me.x, me.y)) {
                        wm_send_close(w);
                    } else {
                        wm_handle_mouse_down(&g_wm, me.x, me.y, me.buttons);
                    }
                }
            }
        } else if (release) {
            wm_handle_mouse_up(&g_wm);
        } else if (moved) {
            wm_handle_mouse_move(&g_wm, me.x, me.y);
        }

        prev = me;

        /* -- Composite if anything changed --------------------------------- */
        int any_dirty = moved || pressed || release;
        for (i = 0; i < g_wm.num_windows; i++) {
            if (g_wm.windows[i].dirty) { any_dirty = 1; break; }
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
