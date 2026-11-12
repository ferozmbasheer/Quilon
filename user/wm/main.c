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
 *   1. read the mouse                       -> raise/focus/drag/close,
 *                                              taskbar apps + Power button;
 *                                              per-button press/release, drag
 *                                              motion and wheel to apps (18.2)
 *   2. drain + decode the keyboard          -> Alt-Tab / Alt-F4, else a key
 *                                              event to the focused window
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
void power_dialog_open(window_t *win);
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

static int         g_poweroff;   /* set by the power dialog: off after flush */
static int         g_power_win;  /* id of the open power dialog, 0 = none   */

void wm_request_poweroff(void) { g_poweroff = 1; }

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

/* ── Power button: open (or bring forward) the confirmation dialog ───────── */

static void wm_open_power_dialog(void)
{
    window_t *w = g_power_win ? wm_find_window(&g_wm, g_power_win) : (window_t *)0;
    if (w) {                                  /* already open: raise + focus */
        damage_window(focused_window());
        int slot = wm_find_idx(&g_wm, w->id);
        wm_raise(&g_wm, slot);
        wm_focus(&g_wm, slot);
        damage_window(w);
        return;
    }
    int x = (g_sw - WM_POWER_DLG_W) / 2;
    int y = (g_sh - WM_POWER_DLG_H) / 2;
    int id = wm_create_window(&g_wm, "Power", x, y, WM_POWER_DLG_W, WM_POWER_DLG_H);
    if (id < 0) return;
    w = wm_find_window(&g_wm, id);
    if (!w) return;
    w->backbuf = canvas_create(WM_POWER_DLG_W, WM_POWER_DLG_H);
    if (!w->backbuf) { wm_destroy_window(&g_wm, id); return; }
    damage_window(focused_window());          /* old focus loses its title */
    power_dialog_open(w);
    g_power_win = id;
}

/* Ask a window to close: notify the app (so it can flush/stop), then mark it
 * for destruction at the end of this loop iteration. */
static void request_close(window_t *w)
{
    if (w->on_event) {
        wm_event_t ev = wm_event_make(WM_EV_CLOSE);
        w->on_event(w, &ev);
    }
    w->want_close = 1;
}

/* ── Keyboard (ROADMAP3 18.2) ─────────────────────────────────────────────
 * The kernel hands us a byte stream (kbd_xlate); wm_key_decode turns it into
 * keys.  WM shortcuts are taken here; everything else goes to the focused
 * window as a WM_EV_KEY carrying keycode, mods, ascii and the raw bytes. */

static void handle_key(const wm_key_t *k)
{
    window_t *f = focused_window();

    if (k->keycode == WM_KEY_TAB && k->mods == WM_MOD_ALT) {   /* Alt-Tab */
        damage_window(f);
        int slot = wm_cycle_focus(&g_wm);
        if (slot >= 0) damage_window(&g_wm.windows[slot]);
        return;
    }
    if (k->keycode == WM_KEY_F1 + 3 && (k->mods & WM_MOD_ALT)) {  /* Alt-F4 */
        if (f) request_close(f);
        return;
    }

    if (!f || !f->on_event) return;
    wm_event_t ev = wm_event_make(WM_EV_KEY);
    ev.keycode = k->keycode;
    ev.mods    = k->mods;
    ev.ascii   = k->ascii;
    for (int i = 0; i < k->seq_len; i++) ev.seq[i] = k->seq[i];
    ev.seq_len = k->seq_len;
    f->on_event(f, &ev);
}

/* Drain everything the keyboard has and decode it.  A key's bytes are queued
 * by one keyboard IRQ, so they are never split between two drains -- except
 * at our buffer's edge, which is why a full buffer only decodes up to the
 * last WM_KEY_SEQ_MAX bytes and keeps the rest for the next read. */
static void drain_keyboard(void)
{
    static char kb[64];
    int kn = 0;
    for (;;) {
        int got = read_nonblock(STDIN_FILENO, kb + kn, (int)sizeof kb - kn);
        if (got > 0) kn += got;
        int more = (got > 0 && kn == (int)sizeof kb);
        int off  = 0;
        while (off < kn && (!more || kn - off >= WM_KEY_SEQ_MAX)) {
            wm_key_t k;
            off += wm_key_decode(kb + off, kn - off, &k);
            handle_key(&k);
        }
        for (int i = off; i < kn; i++) kb[i - off] = kb[i];
        kn -= off;
        if (!more) break;
    }
}

/* ── Mouse (ROADMAP3 18.2) ────────────────────────────────────────────────
 * A press inside a window's content is delivered to that window and
 * "captures" the mouse: motion while any button is held, and the release,
 * go to the same window even if the cursor has left it (drag-select). */

static int g_capture;            /* id of the window holding the mouse, 0 */

static void send_mouse(window_t *w, wm_ev_type_t type, int mx, int my,
                       uint8_t button, uint8_t buttons)
{
    if (!w || !w->on_event || w->want_close) return;
    wm_event_t ev = wm_event_make(type);
    ev.x       = mx - w->bounds.x;
    ev.y       = my - w->bounds.y;
    ev.button  = button;
    ev.buttons = buttons;
    w->on_event(w, &ev);
}

static void mouse_press(int mx, int my, uint8_t button, uint8_t buttons,
                        const char **labels)
{
    (void)labels;
    if (my >= g_sh - TASKBAR_H) {
        if (button != MOUSE_BTN_LEFT) return;
        int idx = wm_taskbar_app_hit(mx, my, g_sw, g_sh, TASKBAR_H,
                                     G_NOAPPS, TASKBAR_BTN_W);
        if (idx >= 0)
            wm_launch(idx);             /* new window is dirty -> damaged */
        else if (wm_rect_contains(wm_taskbar_power_rect(g_sw, g_sh, TASKBAR_H),
                                  mx, my))
            wm_open_power_dialog();
        return;
    }

    int hit = wm_hittest(&g_wm, mx, my);
    if (hit < 0) return;
    window_t *w = &g_wm.windows[hit];
    if (button == MOUSE_BTN_LEFT && wm_in_close_btn(w, mx, my)) {
        request_close(w);
        return;
    }

    /* Raise + focus: the old focus loses its active titlebar and the hit
     * window may come to the front. */
    damage_window(focused_window());
    damage_window(w);
    if (button == MOUSE_BTN_LEFT) {
        wm_handle_mouse_down(&g_wm, mx, my, buttons);   /* + titlebar drag */
    } else {
        wm_raise(&g_wm, hit);
        wm_focus(&g_wm, hit);
    }

    /* Content clicks go to the app (titlebar clicks are the WM's). */
    if (wm_in_content(w, mx, my)) {
        if (!g_capture) g_capture = w->id;
        if (g_capture == w->id)
            send_mouse(w, WM_EV_MOUSE_BTN, mx, my, button, buttons);
    }
}

static void mouse_release(int mx, int my, uint8_t button, uint8_t buttons)
{
    if (button == MOUSE_BTN_LEFT) wm_handle_mouse_up(&g_wm);
    if (g_capture) {
        send_mouse(wm_find_window(&g_wm, g_capture), WM_EV_MOUSE_UP,
                   mx, my, button, buttons);
        if (buttons == 0) g_capture = 0;
    }
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

        int moved = (me.x != prev.x || me.y != prev.y);
        if (moved) {
            /* The cursor leaves its old spot and appears at the new one. */
            wm_damage_add(&g_damage, wm_cursor_rect(prev.x, prev.y));
            wm_damage_add(&g_damage, wm_cursor_rect(me.x, me.y));
        }

        /* Every button transition since the last read, in order -- the
         * kernel latches edges, so a click quicker than one loop iteration
         * still arrives as a press and a release. */
        wm_btn_edge_t edges[WM_BTN_EDGES_MAX];
        int ne = wm_button_edges(prev.buttons, me.buttons,
                                 me.pressed, me.released, edges);
        uint8_t bstate = prev.buttons;
        for (int e = 0; e < ne; e++) {
            if (edges[e].down) {
                bstate |= edges[e].button;
                mouse_press(me.x, me.y, edges[e].button, bstate, labels);
            } else {
                bstate &= (uint8_t)~edges[e].button;
                mouse_release(me.x, me.y, edges[e].button, bstate);
            }
        }

        if (moved) {
            /* A drag repaints where the window was and where it now is. */
            window_t *dw = wm_find_window(&g_wm, g_wm.drag_win_id);
            damage_window(dw);
            wm_handle_mouse_move(&g_wm, me.x, me.y);
            damage_window(dw);
            /* Motion with a button held goes to the window that got the
             * press (drag-select); plain hovering is not reported. */
            if (g_capture && bstate)
                send_mouse(wm_find_window(&g_wm, g_capture), WM_EV_MOUSE_MOVE,
                           me.x, me.y, 0, bstate);
        }

        /* Wheel: to the window under the cursor. */
        if (me.wheel) {
            int hit = wm_hittest(&g_wm, me.x, me.y);
            if (hit >= 0) {
                window_t *w = &g_wm.windows[hit];
                if (w->on_event) {
                    wm_event_t ev = wm_event_make(WM_EV_SCROLL);
                    ev.x       = me.x - w->bounds.x;
                    ev.y       = me.y - w->bounds.y;
                    ev.buttons = me.buttons;
                    ev.wheel   = me.wheel;
                    w->on_event(w, &ev);
                }
            }
        }
        prev = me;

        /* -- Keyboard (non-blocking) --------------------------------------- */
        drain_keyboard();

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
                if (w->id == g_power_win) g_power_win = 0;
                if (w->id == g_capture)   g_capture = 0;
                printf("[wm] closed '%s' (win id=%d)\r\n", w->title, w->id);
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

        /* The power dialog's "Powering off..." frame is now on screen. */
        if (g_poweroff) poweroff();
    }

    return 0;
}
