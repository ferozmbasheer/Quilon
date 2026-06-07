/*
 * Quilon OS -- File Browser app (section 14.4, single-threaded WM)
 *
 * Scrollable directory listing.  j/k move the selection; Enter navigates into
 * directories or opens a file in a textview.  Implemented as non-blocking WM
 * callbacks (on_event for keys/close); per-window state in win->app_state.
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <gfx.h>
#include "../wm/wm.h"

#define LIST_ITEM_H  16
#define MAX_ENTRIES  64

typedef struct {
    char name[64];
    int  is_dir;
} fb_entry_t;

typedef struct {
    fb_entry_t entries[MAX_ENTRIES];
    int        n;
    int        scroll;
    int        selected;
    int        rows;
} fb_state_t;

static int fb_read_dir(fb_entry_t *entries, int max)
{
    dirent_t ent;
    unsigned int i = 0;
    int n = 0;
    while (n < max && readdir(i, &ent) == 0) {
        int len = strlen(ent.name);
        if (len >= 64) len = 63;
        int j;
        for (j = 0; j < len; j++) entries[n].name[j] = ent.name[j];
        entries[n].name[len] = '\0';
        entries[n].is_dir    = (ent.type == DIRENT_TYPE_DIR);
        n++;
        i++;
    }
    return n;
}

static void fb_render(canvas_t *c, fb_state_t *st)
{
    int i;
    gfx_fill(c, GFX_RGB(20, 20, 20));
    for (i = st->scroll; i < st->n; i++) {
        int y = (i - st->scroll) * LIST_ITEM_H;
        if (y + LIST_ITEM_H > c->h) break;
        color_t bg = (i == st->selected) ? GFX_RGB(50, 50, 150)
                                         : GFX_RGB(20, 20, 20);
        gfx_fill_rect(c, (rect_t){0, y, c->w, LIST_ITEM_H}, bg);
        color_t fg = st->entries[i].is_dir ? GFX_RGB(100, 180, 255)
                                           : GFX_RGB(220, 220, 220);
        gfx_draw_text(c, 4, y + 1, st->entries[i].name, fg, bg);
    }
}

static void fb_event(window_t *win, const wm_event_t *ev)
{
    fb_state_t *st = (fb_state_t *)win->app_state;

    if (ev->type == WM_EV_CLOSE) { win->want_close = 1; return; }
    if (ev->type != WM_EV_KEY) return;

    char c = ev->ascii;
    if (c == 'j' && st->selected < st->n - 1) st->selected++;
    if (c == 'k' && st->selected > 0)         st->selected--;
    if (st->selected < st->scroll)            st->scroll = st->selected;
    if (st->selected >= st->scroll + st->rows)
        st->scroll = st->selected - st->rows + 1;

    if (c == '\r' || c == '\n') {
        if (st->selected < st->n) {
            if (st->entries[st->selected].is_dir) {
                chdir(st->entries[st->selected].name);
                st->n = fb_read_dir(st->entries, MAX_ENTRIES);
                st->scroll = st->selected = 0;
            } else {
                /* Build absolute path: cwd + "/" + filename. */
                char path[320];
                char cwd_buf[256];
                int j = 0;
                if (getcwd(cwd_buf, sizeof(cwd_buf))) {
                    int cl = (int)strlen(cwd_buf);
                    int k;
                    for (k = 0; k < cl && j < (int)sizeof(path) - 1; k++)
                        path[j++] = cwd_buf[k];
                    if (j > 0 && path[j - 1] != '/' && j < (int)sizeof(path) - 1)
                        path[j++] = '/';
                }
                int nl = (int)strlen(st->entries[st->selected].name);
                int k;
                for (k = 0; k < nl && j < (int)sizeof(path) - 1; k++)
                    path[j++] = st->entries[st->selected].name[k];
                path[j] = '\0';
                wm_open_textview(path);
            }
        }
    }

    fb_render(win->backbuf, st);
    win->dirty = 1;
}

static void fb_destroy(window_t *win)
{
    if (win->app_state) { free(win->app_state); win->app_state = (void *)0; }
    if (win->backbuf)   { canvas_free(win->backbuf); win->backbuf = (canvas_t *)0; }
}

void filebr_open(window_t *win)
{
    fb_state_t *st = (fb_state_t *)malloc(sizeof(fb_state_t));
    if (!st) { win->want_close = 1; return; }
    st->n        = fb_read_dir(st->entries, MAX_ENTRIES);
    st->scroll   = 0;
    st->selected = 0;
    st->rows     = win->backbuf->h / LIST_ITEM_H;

    win->app_state  = st;
    win->on_event   = fb_event;
    win->on_destroy = fb_destroy;

    fb_render(win->backbuf, st);
    win->dirty = 1;
}
