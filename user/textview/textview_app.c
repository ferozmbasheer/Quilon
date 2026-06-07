/*
 * Quilon OS -- Text Viewer app (section 14.4, single-threaded WM)
 *
 * Reads a file path from win->title and displays its contents.  d/u scroll.
 * Implemented as non-blocking WM callbacks (on_event); per-window state
 * (the loaded lines + scroll position) lives in win->app_state.
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <gfx.h>
#include "../wm/wm.h"

#define MAX_LINE_LEN  128
#define MAX_LINES     512

typedef struct {
    char (*lines)[MAX_LINE_LEN];   /* heap: MAX_LINES * MAX_LINE_LEN */
    int    nlines;
    int    scroll;
    int    rows;
} tv_state_t;

static int tv_read_file(const char *path, char lines[][MAX_LINE_LEN], int max)
{
    int fd = open(path);
    if (fd < 0) return 0;

    int n = 0, pos = 0;
    char buf[512];
    int bytes;
    while (n < max && (bytes = read(fd, buf, (int)sizeof(buf))) > 0) {
        int i;
        for (i = 0; i < bytes && n < max; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                lines[n][pos] = '\0';
                n++;
                pos = 0;
            } else if (pos < MAX_LINE_LEN - 1) {
                lines[n][pos++] = c;
            }
        }
    }
    if (pos > 0 && n < max) { lines[n][pos] = '\0'; n++; }
    close(fd);
    return n;
}

static void tv_render(window_t *win)
{
    tv_state_t *st = (tv_state_t *)win->app_state;
    canvas_t   *c  = win->backbuf;
    int r;
    gfx_fill(c, GFX_RGB(15, 15, 15));
    for (r = 0; r < st->rows && st->scroll + r < st->nlines; r++)
        gfx_draw_text(c, 0, r * GFX_CHAR_H, st->lines[st->scroll + r],
                      GFX_RGB(200, 200, 200), GFX_RGB(15, 15, 15));
    win->dirty = 1;
}

static void tv_event(window_t *win, const wm_event_t *ev)
{
    tv_state_t *st = (tv_state_t *)win->app_state;
    if (ev->type == WM_EV_CLOSE) { win->want_close = 1; return; }
    if (ev->type != WM_EV_KEY) return;

    if (ev->ascii == 'd' && st->scroll + st->rows < st->nlines) st->scroll++;
    if (ev->ascii == 'u' && st->scroll > 0)                     st->scroll--;
    tv_render(win);
}

static void tv_destroy(window_t *win)
{
    tv_state_t *st = (tv_state_t *)win->app_state;
    if (st) {
        if (st->lines) free(st->lines);
        free(st);
        win->app_state = (void *)0;
    }
    if (win->backbuf) { canvas_free(win->backbuf); win->backbuf = (canvas_t *)0; }
}

void textview_open(window_t *win)
{
    tv_state_t *st = (tv_state_t *)malloc(sizeof(tv_state_t));
    if (!st) { win->want_close = 1; return; }
    st->lines = (char (*)[MAX_LINE_LEN])malloc(MAX_LINES * MAX_LINE_LEN);
    if (!st->lines) { free(st); win->want_close = 1; return; }

    st->nlines = tv_read_file(win->title, st->lines, MAX_LINES);
    st->rows   = win->backbuf->h / GFX_CHAR_H;
    st->scroll = 0;

    win->app_state  = st;
    win->on_event   = tv_event;
    win->on_destroy = tv_destroy;

    tv_render(win);
}
