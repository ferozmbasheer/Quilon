/*
 * Quilon OS -- Text Viewer app (section 14.4, single-threaded WM)
 *
 * Displays the file the WM passes to textview_open().  Scroll with the
 * arrows, Page Up/Down, Home/End, d/u, the mouse wheel or the scrollbar
 * (widget kit, ROADMAP3 18.5).  Implemented as non-blocking WM callbacks
 * (on_event); per-window state (the loaded lines + the scroll view) lives in
 * win->app_state.
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <gfx.h>
#include "../wm/wm.h"
#include "../wm/widgets.h"

#define MAX_LINE_LEN  128
#define MAX_LINES     512

#define TV_BG  GFX_RGB(15, 15, 15)
#define TV_FG  GFX_RGB(200, 200, 200)

typedef struct {
    char (*lines)[MAX_LINE_LEN];   /* heap: MAX_LINES * MAX_LINE_LEN */
    wg_slist_t view;               /* the lines + scrollbar (18.5)    */
} tv_state_t;

static int tv_read_file(const char *path, char lines[][MAX_LINE_LEN], int max)
{
    int fd = open(path);
    if (fd < 0) return 0;

    int n = 0, pos = 0, prev_cr = 0;
    char buf[512];
    int bytes;
    while (n < max && (bytes = read(fd, buf, (int)sizeof(buf))) > 0) {
        int i;
        for (i = 0; i < bytes && n < max; i++) {
            char c = buf[i];
            /* CRLF ends ONE line: it used to show a blank line after
             * every line of the (CRLF) files on the disk. */
            int skip = (c == '\n' && prev_cr);
            prev_cr = (c == '\r');
            if (skip) continue;
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

/* Row text for the list widget. */
static const char *tv_row(void *ctx, int i, color_t *fg)
{
    tv_state_t *st = (tv_state_t *)ctx;
    *fg = TV_FG;
    return st->lines[i];
}

static void tv_render(window_t *win)
{
    tv_state_t *st = (tv_state_t *)win->app_state;
    wg_slist_draw(win->backbuf, &st->view, TV_BG, tv_row, st);
    win->dirty = 1;
}

static void tv_layout(tv_state_t *st, int w, int h)
{
    wg_slist_layout(&st->view, (rect_t){ 0, 0, w, h });
}

static void tv_event(window_t *win, const wm_event_t *ev)
{
    tv_state_t *st = (tv_state_t *)win->app_state;
    wg_list_t  *l  = &st->view.list;
    int old_top = l->top;

    switch (ev->type) {
    case WM_EV_CLOSE:
        win->want_close = 1;
        return;
    case WM_EV_RESIZE:                        /* ROADMAP3 18.3 */
        tv_layout(st, ev->width, ev->height);
        tv_render(win);
        return;
    case WM_EV_KEY:
        if (ev->keycode == 'd')      wg_list_key(l, WM_KEY_DOWN);
        else if (ev->keycode == 'u') wg_list_key(l, WM_KEY_UP);
        else if (!wg_list_key(l, ev->keycode)) return;
        if (l->top != old_top) tv_render(win);
        return;
    default:                                  /* wheel + scrollbar (18.5) */
        if (wg_slist_mouse(&st->view, ev, 0) != WG_SL_NONE) tv_render(win);
        return;
    }
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

void textview_open(window_t *win, const char *path)
{
    tv_state_t *st = (tv_state_t *)malloc(sizeof(tv_state_t));
    if (!st) { win->want_close = 1; return; }
    st->lines = (char (*)[MAX_LINE_LEN])malloc(MAX_LINES * MAX_LINE_LEN);
    if (!st->lines) { free(st); win->want_close = 1; return; }

    memset(&st->view, 0, sizeof st->view);
    st->view.list.n     = tv_read_file(path, st->lines, MAX_LINES);
    st->view.list.sel   = -1;                 /* a viewer: no selection */
    st->view.list.row_h = GFX_CHAR_H;
    tv_layout(st, win->backbuf->w, win->backbuf->h);

    win->app_state  = st;
    win->on_event   = tv_event;
    win->on_destroy = tv_destroy;

    tv_render(win);
}
