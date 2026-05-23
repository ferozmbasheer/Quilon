/*
 * Quilon OS -- File Browser app (section 14.4)
 *
 * Scrollable directory listing.  j/k or arrow keys move the selection;
 * Enter navigates into directories.  Runs as a WM app thread.
 */

#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <gfx.h>
#include "../wm/wm.h"

#define LIST_ITEM_H  16
#define MAX_ENTRIES  64

typedef struct {
    char name[64];
    int  is_dir;
} fb_entry_t;

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

static void fb_render(canvas_t *c, fb_entry_t *entries, int n,
                      int scroll, int selected)
{
    int i;
    gfx_fill(c, GFX_RGB(20, 20, 20));
    for (i = scroll; i < n; i++) {
        int y = (i - scroll) * LIST_ITEM_H;
        if (y + LIST_ITEM_H > c->h) break;
        color_t bg = (i == selected) ? GFX_RGB(50, 50, 150)
                                     : GFX_RGB(20, 20, 20);
        gfx_fill_rect(c, (rect_t){0, y, c->w, LIST_ITEM_H}, bg);
        color_t fg = entries[i].is_dir ? GFX_RGB(100, 180, 255)
                                       : GFX_RGB(220, 220, 220);
        gfx_draw_text(c, 4, y + 1, entries[i].name, fg, bg);
    }
}

void filebr_app(window_t *win)
{
    fb_entry_t entries[MAX_ENTRIES];
    int n = fb_read_dir(entries, MAX_ENTRIES);
    int scroll = 0, selected = 0;
    int rows = win->backbuf->h / LIST_ITEM_H;

    fb_render(win->backbuf, entries, n, scroll, selected);
    win->dirty = 1;

    wm_event_t ev;
    while (win->active) {
        if (!wm_evqueue_poll(&win->events, &ev)) continue;

        if (ev.type == WM_EV_CLOSE) break;

        if (ev.type == WM_EV_KEY) {
            char c = ev.ascii;
            if (c == 'j' && selected < n - 1) selected++;
            if (c == 'k' && selected > 0)     selected--;
            if (selected < scroll)             scroll = selected;
            if (selected >= scroll + rows)     scroll = selected - rows + 1;

            if (c == '\r' || c == '\n') {
                if (selected < n) {
                    if (entries[selected].is_dir) {
                        chdir(entries[selected].name);
                        n = fb_read_dir(entries, MAX_ENTRIES);
                        scroll = selected = 0;
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
                            if (j > 0 && path[j - 1] != '/' &&
                                j < (int)sizeof(path) - 1)
                                path[j++] = '/';
                        }
                        int nl = (int)strlen(entries[selected].name);
                        int k;
                        for (k = 0; k < nl && j < (int)sizeof(path) - 1; k++)
                            path[j++] = entries[selected].name[k];
                        path[j] = '\0';
                        wm_open_textview(path);
                    }
                }
            }
            fb_render(win->backbuf, entries, n, scroll, selected);
            win->dirty = 1;
        }
    }
}
