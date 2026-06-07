/*
 * Quilon OS -- Clock app (section 14.4, single-threaded WM)
 *
 * Displays HH:MM:SS since boot.  Implemented as non-blocking WM callbacks:
 *   on_tick  -- redraw once per second
 *   on_event -- close button -> request destroy
 * Per-window state (last rendered second) lives in win->app_state.
 */

#include <unistd.h>
#include <stdlib.h>
#include <gfx.h>
#include "../wm/wm.h"

typedef struct {
    unsigned int last_sec;
} clock_state_t;

static void int_to_2dig(char *buf, unsigned int v)
{
    buf[0] = '0' + (v / 10) % 10;
    buf[1] = '0' + v % 10;
}

static void clock_tick(window_t *win)
{
    clock_state_t *st = (clock_state_t *)win->app_state;

    unsigned int hz  = gethz();
    unsigned int now = hz ? getticks() / hz : 0;
    if (now == st->last_sec) return;       /* nothing changed this tick */
    st->last_sec = now;

    unsigned int h = (now / 3600) % 24;
    unsigned int m = (now / 60)   % 60;
    unsigned int s = now           % 60;

    char str[9];   /* HH:MM:SS\0 */
    int_to_2dig(str + 0, h); str[2] = ':';
    int_to_2dig(str + 3, m); str[5] = ':';
    int_to_2dig(str + 6, s); str[8] = '\0';

    canvas_t *c = win->backbuf;
    gfx_fill(c, GFX_RGB(10, 10, 10));
    gfx_draw_text(c, 40, 12, str, GFX_RGB(0, 255, 128), GFX_RGB(10, 10, 10));
    win->dirty = 1;
}

static void clock_event(window_t *win, const wm_event_t *ev)
{
    if (ev->type == WM_EV_CLOSE)
        win->want_close = 1;
}

static void clock_destroy(window_t *win)
{
    if (win->app_state) { free(win->app_state); win->app_state = (void *)0; }
    if (win->backbuf)   { canvas_free(win->backbuf); win->backbuf = (canvas_t *)0; }
}

/* Launcher entry point: install callbacks + state, draw the first frame. */
void clock_open(window_t *win)
{
    clock_state_t *st = (clock_state_t *)malloc(sizeof(clock_state_t));
    if (!st) { win->want_close = 1; return; }
    st->last_sec = (unsigned int)-1;

    win->app_state  = st;
    win->on_tick    = clock_tick;
    win->on_event   = clock_event;
    win->on_destroy = clock_destroy;

    clock_tick(win);   /* render the initial time immediately */
}
