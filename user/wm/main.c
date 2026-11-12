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
 * Each iteration (ROADMAP3 18.1 -- the loop sleeps when nothing happens):
 *   0. wait_input(): sleep until the keyboard, the mouse, a watched fd (a
 *      terminal's shell pipe) has input, or the next timed app tick is due
 *   1. read the mouse                       -> raise/focus/drag/close
 *   2. drain the keyboard                   -> deliver key to focused window
 *   3. tick the windows that are due        -> clock updates, pipe draining
 *   4. destroy windows that requested close (want_close)
 *   5. recomposite + flush ONLY the damaged screen rects (cursor, moved or
 *      dirty windows, taskbar)
 *
 * Apps must never block.  Anything that would block (a terminal reading its
 * shell's pipe) is done with a non-blocking poll inside on_tick.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gfx.h>
#include <signal.h>
#include "wm.h"

/* App "open" entry points: each sets up the window's callbacks + app_state.
 * Defined in the respective app .c files. */
void gterm_open(window_t *win);
void clock_open(window_t *win);
void filebr_open(window_t *win);
void textview_open(window_t *win);
void gterm_reap_shells(void);   /* reap shells of closed terminals */
int  gterm_reap_pending(void);  /* 1 while a closed terminal's shell is unreaped */

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
static wm_damage_t g_damage;     /* screen rects to repaint this frame (18.1) */

/* Mark a window's whole on-screen footprint (chrome + content) damaged. */
static void damage_window(const window_t *w)
{
    if (w && w->id != 0) wm_damage_add(&g_damage, wm_full_rect(w));
}

/* The focused window, or NULL. */
static window_t *focused_window(void)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_wm.windows[i].id != 0 && g_wm.windows[i].focused)
            return &g_wm.windows[i];
    return (window_t *)0;
}

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

/* How often to retry reaping a closed terminal's shell while one is pending. */
#define REAP_POLL_MS  100

int main(void)
{
    printf("[wm] Quilon Desktop starting\r\n");

    /* Ctrl-C must reach us as an ordinary 0x03 key (forwarded to the focused
     * terminal, which interrupts its own shell's job) rather than kill the
     * desktop: we are the console shell's foreground job.  ROADMAP3 17.0.1. */
    signal(SIGINT, SIG_IGN);

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
    wm_damage_init(&g_damage, g_sw, g_sh);
    const rect_t taskbar_rect = { 0, g_sh - TASKBAR_H, g_sw, TASKBAR_H };
    const uint32_t hz = gethz();

    /* 3. Build the static label array for wm_draw_taskbar. */
    const char *labels[G_NOAPPS];
    int i;
    for (i = 0; i < G_NOAPPS; i++) labels[i] = g_apps[i].label;

    /* 4. Initial frame: desktop + taskbar + cursor (cursor last = on top).
     *    Reading the mouse first also syncs the kernel's "mouse seen" mark,
     *    so the first wait_input() doesn't return at once for stale motion. */
    mouse_event_t prev = {0};
    mouse_read(&prev);
    g_wm.mouse_x = prev.x;
    g_wm.mouse_y = prev.y;
    wm_composite(&g_wm, g_screen);
    wm_draw_taskbar(g_screen, g_sw, g_sh, TASKBAR_H,
                    labels, G_NOAPPS, TASKBAR_BTN_W);
    wm_draw_cursor(g_screen, g_wm.mouse_x, g_wm.mouse_y);
    gfx_flush();

    /* 5. Event loop. */
    while (1) {
        /* -- Sleep until something can happen ------------------------------
         * Watch each window's wait_fd (terminal shell pipes); wake for the
         * next timed tick (the clock), or to retry reaping a closed
         * terminal's shell.  With none of those we sleep until input. */
        int fds[WAIT_INPUT_MAX_FDS], fd_slot[WAIT_INPUT_MAX_FDS], nfds = 0;
        for (i = 0; i < WM_MAX_WINDOWS && nfds < WAIT_INPUT_MAX_FDS; i++) {
            window_t *w = &g_wm.windows[i];
            if (w->id != 0 && w->on_tick && w->wait_fd >= 0) {
                fds[nfds] = w->wait_fd;
                fd_slot[nfds++] = i;
            }
        }
        int timeout = wm_next_timeout_ms(&g_wm, getticks(), hz);
        if (gterm_reap_pending() && (timeout < 0 || timeout > REAP_POLL_MS))
            timeout = REAP_POLL_MS;
        int ready = wait_input(timeout < 0 ? WAIT_INPUT_FOREVER : timeout,
                               fds, nfds);
        if (ready < 0) ready = 0;
        uint32_t now = getticks();

        /* -- Mouse --------------------------------------------------------- */
        mouse_event_t me;
        mouse_read(&me);

        int moved   = (me.x != prev.x || me.y != prev.y);
        int pressed = (me.buttons && !prev.buttons);
        int release = (!me.buttons && prev.buttons);

        if (moved) {
            /* The cursor leaves its old spot and appears at the new one. */
            wm_damage_add(&g_damage, wm_cursor_rect(prev.x, prev.y));
            wm_damage_add(&g_damage, wm_cursor_rect(me.x, me.y));
        }

        if (pressed) {
            if (me.y >= g_sh - TASKBAR_H) {
                int idx = me.x / TASKBAR_BTN_W;
                if (idx >= 0 && idx < G_NOAPPS)
                    wm_launch(idx);     /* new window is dirty -> damaged */
            } else {
                int hit = wm_hittest(&g_wm, me.x, me.y);
                if (hit >= 0) {
                    window_t *w = &g_wm.windows[hit];
                    if (wm_in_close_btn(w, me.x, me.y)) {
                        request_close(w);
                    } else {
                        /* Raise + focus: the old focus loses its active
                         * titlebar and the hit window may come to the front. */
                        damage_window(focused_window());
                        damage_window(w);
                        wm_handle_mouse_down(&g_wm, me.x, me.y, me.buttons);
                        /* Deliver in-content clicks to the app, with the
                         * cursor expressed relative to the content origin.
                         * (Titlebar clicks start a drag and aren't forwarded.) */
                        if (wm_in_content(w, me.x, me.y) && w->on_event) {
                            wm_event_t ev;
                            ev.type    = WM_EV_MOUSE_BTN;
                            ev.x       = me.x - w->bounds.x;
                            ev.y       = me.y - w->bounds.y;
                            ev.buttons = me.buttons;
                            ev.ascii   = 0;
                            w->on_event(w, &ev);
                        }
                    }
                }
            }
        } else if (release) {
            wm_handle_mouse_up(&g_wm);
        }
        if (moved) {
            /* A drag repaints where the window was and where it now is. */
            window_t *dw = wm_find_window(&g_wm, g_wm.drag_win_id);
            damage_window(dw);
            wm_handle_mouse_move(&g_wm, me.x, me.y);
            damage_window(dw);
        }
        prev = me;

        /* -- Keyboard (non-blocking) --------------------------------------- */
        char c;
        while (read_nonblock(STDIN_FILENO, &c, 1) == 1)
            deliver_key(c);

        /* -- Tick the windows that are due --------------------------------- */
        for (i = 0; i < WM_MAX_WINDOWS; i++) {
            window_t *w = &g_wm.windows[i];
            if (w->id == 0 || !w->on_tick) continue;
            int due = wm_tick_due(w, now);
            for (int k = 0; k < nfds && !due; k++)
                if (fd_slot[k] == i && fds[k] == w->wait_fd &&
                    (ready & (int)WAIT_INPUT_FD(k)))
                    due = 1;
            if (!due) continue;
            if (w->tick_ms > 0 && wm_tick_due(w, now))
                wm_tick_rearm(w, now, hz);
            w->on_tick(w);
        }

        /* -- Reap the shells of terminals closed earlier (never blocks) ----- */
        gterm_reap_shells();

        /* -- Destroy windows that requested close -------------------------- */
        for (i = 0; i < WM_MAX_WINDOWS; i++) {
            window_t *w = &g_wm.windows[i];
            if (w->id != 0 && w->want_close) {
                damage_window(w);
                wm_destroy_window(&g_wm, w->id);
                /* Focus may have passed to the new frontmost window. */
                damage_window(focused_window());
            }
        }

        /* -- Dirty windows (new pixels, moved, or just created) ------------ */
        for (i = 0; i < g_wm.num_windows; i++) {
            window_t *w = &g_wm.windows[g_wm.z_order[i]];
            if (w->dirty) { damage_window(w); w->dirty = 0; }
        }

        if (g_damage.n == 0) continue;

        /* -- Repaint + flush only the damage ------------------------------- */
        for (i = 0; i < g_damage.n; i++) {
            rect_t r = g_damage.r[i];
            wm_composite_rect(&g_wm, g_screen, r);
            if (wm_rect_intersects(r, taskbar_rect)) {
                gfx_set_clip(g_screen, r);
                wm_draw_taskbar(g_screen, g_sw, g_sh, TASKBAR_H,
                                labels, G_NOAPPS, TASKBAR_BTN_W);
                gfx_clear_clip(g_screen);
            }
        }
        /* Cursor last so it stays on top of windows AND the taskbar.  Where
         * nothing under it was repainted this rewrites identical pixels. */
        wm_draw_cursor(g_screen, g_wm.mouse_x, g_wm.mouse_y);
        for (i = 0; i < g_damage.n; i++)
            gfx_flush_rect(g_damage.r[i]);
        g_damage.n = 0;
    }

    return 0;
}
