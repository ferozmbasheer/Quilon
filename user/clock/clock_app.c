/*
 * Quilon OS -- Clock app (section 14.4)
 *
 * Displays HH:MM:SS since boot.  Runs as a WM app thread.
 */

#include <unistd.h>
#include <stdio.h>
#include <gfx.h>
#include "../wm/wm.h"

static void int_to_2dig(char *buf, unsigned int v)
{
    buf[0] = '0' + (v / 10) % 10;
    buf[1] = '0' + v % 10;
}

void clock_app(window_t *win)
{
    unsigned int last_sec = (unsigned int)-1;

    while (win->active) {
        /* Check for WM close event. */
        wm_event_t ev;
        if (wm_evqueue_poll(&win->events, &ev) && ev.type == WM_EV_CLOSE)
            break;

        unsigned int hz  = gethz();
        unsigned int now = hz ? getticks() / hz : 0;

        if (now == last_sec) continue;
        last_sec = now;

        unsigned int h = (now / 3600) % 24;
        unsigned int m = (now / 60)   % 60;
        unsigned int s = now           % 60;

        char str[9];   /* HH:MM:SS\0 */
        int_to_2dig(str + 0, h); str[2] = ':';
        int_to_2dig(str + 3, m); str[5] = ':';
        int_to_2dig(str + 6, s); str[8] = '\0';

        canvas_t *c = win->backbuf;
        gfx_fill(c, GFX_RGB(10, 10, 10));
        gfx_draw_text(c, 40, 12, str,
                      GFX_RGB(0, 255, 128), GFX_RGB(10, 10, 10));
        win->dirty = 1;
    }
}
