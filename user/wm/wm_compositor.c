/*
 * Quilon OS -- WM compositor implementation (section 14.2)
 *
 * Compiled separately so tests can link it without pulling in main().
 * Depends only on libgfx (canvas_t, gfx_* functions) and wm.h types.
 */

#include <gfx.h>
#include "wm.h"

/* ── Mouse cursor sprite (8×8 arrow, 1-bit per pixel, MSB = leftmost) ─── */

static const uint8_t cursor_shape[8] = {
    0xFE,   /* █ █ █ █ █ █ █ · */
    0xFC,   /* █ █ █ █ █ █ · · */
    0xF8,   /* █ █ █ █ █ · · · */
    0xF0,   /* █ █ █ █ · · · · */
    0xE0,   /* █ █ █ · · · · · */
    0xC0,   /* █ █ · · · · · · */
    0x80,   /* █ · · · · · · · */
    0x00,   /* · · · · · · · · */
};
#define CURSOR_W 8
#define CURSOR_H 8

void wm_draw_cursor(canvas_t *screen, int mx, int my)
{
    int row, col;
    for (row = 0; row < CURSOR_H; row++) {
        for (col = 0; col < CURSOR_W; col++) {
            int px = mx + col;
            int py = my + row;
            if (px < 0 || py < 0 || px >= screen->w || py >= screen->h)
                continue;
            if (cursor_shape[row] & (0x80u >> col))
                screen->pixels[py * screen->pitch + px] = GFX_WHITE;
        }
    }
}

/* ── Compositor (painter's algorithm) ─────────────────────────────────── */

void wm_composite(wm_state_t *s, canvas_t *screen)
{
    int i;

    /* 1. Desktop background. */
    gfx_fill(screen, WM_DESKTOP_COL);

    /* 2. Back-to-front (index 0 = back, num_windows-1 = front). */
    for (i = 0; i < s->num_windows; i++) {
        window_t *w = &s->windows[i];

        rect_t tb    = wm_titlebar_rect(w);
        rect_t full  = wm_full_rect(w);
        rect_t close = wm_close_btn_rect(w);

        color_t tbar_col = w->focused ? WM_TBAR_FOCUSED : WM_TBAR_UNFOCUSED;

        /* Title bar background. */
        gfx_fill_rect(screen, tb, tbar_col);

        /* Title text. */
        gfx_draw_text(screen, tb.x + 4, tb.y + 2, w->title,
                      WM_TBAR_TEXT_COL, tbar_col);

        /* Close button. */
        gfx_fill_rect(screen, close, WM_CLOSE_COL);
        gfx_draw_text(screen, close.x + 4, close.y + 2, "x",
                      GFX_WHITE, WM_CLOSE_COL);

        /* App back-buffer (if present). */
        if (w->backbuf) {
            gfx_set_clip(screen, w->bounds);
            gfx_blit(screen, w->bounds.x, w->bounds.y,
                     w->backbuf,
                     (rect_t){0, 0, w->backbuf->w, w->backbuf->h});
            gfx_clear_clip(screen);
        } else {
            gfx_fill_rect(screen, w->bounds, GFX_RGB(20, 20, 20));
        }

        /* Window border. */
        gfx_draw_rect(screen, full, WM_BORDER_COL);

        w->dirty = 0;
    }

    /* 3. Cursor on top of everything. */
    wm_draw_cursor(screen, s->mouse_x, s->mouse_y);
}

/* ── Taskbar ─────────────────────────────────────────────────────────────── */

void wm_draw_taskbar(canvas_t *screen, int sw, int sh, int taskbar_h,
                     const char **labels, int nlabels, int btn_w)
{
    int i;
    int ty = sh - taskbar_h;

    /* Bar background. */
    gfx_fill_rect(screen, (rect_t){0, ty, sw, taskbar_h}, GFX_RGB(40,40,40));

    /* One button per app. */
    for (i = 0; i < nlabels; i++) {
        int bx = i * btn_w;
        gfx_fill_rect(screen, (rect_t){bx + 1, ty + 2, btn_w - 2, taskbar_h - 4},
                      GFX_RGB(70, 70, 100));
        gfx_draw_text(screen, bx + 6, ty + 6, labels[i],
                      GFX_WHITE, GFX_RGB(70, 70, 100));
    }
}
