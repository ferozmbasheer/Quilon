/*
 * Quilon OS -- Text Viewer app (section 14.4)
 *
 * Reads a file path from win->title and displays its contents.
 * d/u scroll down/up one line.  Runs as a WM app thread.
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <gfx.h>
#include "../wm/wm.h"

#define MAX_LINE_LEN  128
#define MAX_LINES     512

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

static void tv_render(canvas_t *c, char lines[][MAX_LINE_LEN],
                      int nlines, int scroll, int rows)
{
    int r;
    gfx_fill(c, GFX_RGB(15, 15, 15));
    for (r = 0; r < rows && scroll + r < nlines; r++)
        gfx_draw_text(c, 0, r * GFX_CHAR_H, lines[scroll + r],
                      GFX_RGB(200, 200, 200), GFX_RGB(15, 15, 15));
}

void textview_app(window_t *win)
{
    /* The path to open is stored in win->title by the launcher. */
    char (*lines)[MAX_LINE_LEN] =
        (char (*)[MAX_LINE_LEN])malloc(MAX_LINES * MAX_LINE_LEN);
    if (!lines) { win->active = 0; return; }

    int nlines = tv_read_file(win->title, lines, MAX_LINES);
    int rows   = win->backbuf->h / GFX_CHAR_H;
    int scroll = 0;

    tv_render(win->backbuf, lines, nlines, scroll, rows);
    win->dirty = 1;

    wm_event_t ev;
    while (win->active) {
        if (!wm_evqueue_poll(&win->events, &ev)) continue;
        if (ev.type == WM_EV_CLOSE) break;
        if (ev.type == WM_EV_KEY) {
            if (ev.ascii == 'd' && scroll + rows < nlines) scroll++;
            if (ev.ascii == 'u' && scroll > 0)             scroll--;
            tv_render(win->backbuf, lines, nlines, scroll, rows);
            win->dirty = 1;
        }
    }

    free(lines);
}
