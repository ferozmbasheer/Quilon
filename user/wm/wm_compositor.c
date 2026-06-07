/*
 * Quilon OS -- WM compositor implementation (section 14.2)
 *
 * Visual design inspired by classic 1990s windowing systems (Windows 3.1 /
 * early macOS) and the twlv hobby OS (Nim).  Key technique: 3D bevel via
 * layered 1-pixel rectangles — top/left = white highlight, bottom/right =
 * dark shadow, 1-px inset inner shadow — gives window chrome depth without
 * any alpha blending.
 *
 * Compiled separately so tests can link it without pulling in main().
 * Depends only on libgfx (canvas_t, gfx_* functions) and wm.h types.
 */

#include <gfx.h>
#include "wm.h"

/* ── Mouse cursor ──────────────────────────────────────────────────────────
 *
 * Classic 11-wide × 17-tall arrow cursor (Windows 3.1 style).
 * Encoded as uint16_t per row: bit 15 = leftmost pixel.
 *
 * Hot-spot: (0, 0) — the tip of the arrow.
 *
 * Rendering uses two passes: black drop-shadow at (+1,+1), white fill on
 * top.  This makes the cursor visible against any background colour.
 */

#define CURSOR_W  11
#define CURSOR_H  17

static const uint16_t cursor_shape[CURSOR_H] = {
    0x8000,   /* █ · · · · · · · · · · */
    0xC000,   /* █ █ · · · · · · · · · */
    0xE000,   /* █ █ █ · · · · · · · · */
    0xF000,   /* █ █ █ █ · · · · · · · */
    0xF800,   /* █ █ █ █ █ · · · · · · */
    0xFC00,   /* █ █ █ █ █ █ · · · · · */
    0xFE00,   /* █ █ █ █ █ █ █ · · · · */
    0xFF00,   /* █ █ █ █ █ █ █ █ · · · */
    0xFF80,   /* █ █ █ █ █ █ █ █ █ · · */
    0xF800,   /* █ █ █ █ █ · · · · · · */
    0xD800,   /* █ █ · █ █ · · · · · · */
    0x8C00,   /* █ · · · █ █ · · · · · */
    0x0C00,   /* · · · · █ █ · · · · · */
    0x0600,   /* · · · · · █ █ · · · · */
    0x0600,   /* · · · · · █ █ · · · · */
    0x0300,   /* · · · · · · █ █ · · · */
    0x0000,
};

void wm_draw_cursor(canvas_t *screen, int mx, int my)
{
    int row, bit;

    /* Pass 1 — black drop shadow at (+1, +1). */
    for (row = 0; row < CURSOR_H; row++) {
        for (bit = 0; bit < CURSOR_W; bit++) {
            if (!(cursor_shape[row] & (0x8000u >> bit))) continue;
            int px = mx + bit + 1, py = my + row + 1;
            if (px >= 0 && py >= 0 && px < screen->w && py < screen->h)
                screen->pixels[py * screen->pitch + px] = GFX_BLACK;
        }
    }

    /* Pass 2 — white fill at actual position. */
    for (row = 0; row < CURSOR_H; row++) {
        for (bit = 0; bit < CURSOR_W; bit++) {
            if (!(cursor_shape[row] & (0x8000u >> bit))) continue;
            int px = mx + bit, py = my + row;
            if (px >= 0 && py >= 0 && px < screen->w && py < screen->h)
                screen->pixels[py * screen->pitch + px] = GFX_WHITE;
        }
    }
}

/* ── 3D bevel helpers ───────────────────────────────────────────────────────
 *
 * raised_bevel  — outer highlight (top/left white) + shadow (bottom/right
 *                 dark) + 1-px inner shadow.  Makes an element look raised.
 * sunken_bevel  — inverted: dark on top/left, light on bottom/right.
 *                 Makes an inset well (used around content areas).
 */

static void raised_bevel(canvas_t *c, int x, int y, int w, int h)
{
    /* Top edge: white highlight. */
    gfx_fill_rect(c, (rect_t){x,       y,       w,   1}, WM_FRAME_HI);
    /* Left edge: white highlight. */
    gfx_fill_rect(c, (rect_t){x,       y,       1,   h}, WM_FRAME_HI);
    /* Bottom edge: dark shadow. */
    gfx_fill_rect(c, (rect_t){x,       y+h-1,   w,   1}, WM_FRAME_SHADOW);
    /* Right edge: dark shadow. */
    gfx_fill_rect(c, (rect_t){x+w-1,   y,       1,   h}, WM_FRAME_SHADOW);
    /* Inner bottom (1-px inset): mid shadow. */
    gfx_fill_rect(c, (rect_t){x+1,     y+h-2,   w-2, 1}, WM_FRAME_MID);
    /* Inner right (1-px inset): mid shadow. */
    gfx_fill_rect(c, (rect_t){x+w-2,   y+1,     1, h-2}, WM_FRAME_MID);
}

static void sunken_bevel(canvas_t *c, int x, int y, int w, int h)
{
    /* Top/left: dark. */
    gfx_fill_rect(c, (rect_t){x,     y,     w,   1}, WM_FRAME_SHADOW);
    gfx_fill_rect(c, (rect_t){x,     y,     1,   h}, WM_FRAME_SHADOW);
    /* Inner top/left: mid shadow. */
    gfx_fill_rect(c, (rect_t){x+1,   y+1,   w-2, 1}, WM_FRAME_MID);
    gfx_fill_rect(c, (rect_t){x+1,   y+1,   1, h-2}, WM_FRAME_MID);
    /* Bottom/right: white highlight. */
    gfx_fill_rect(c, (rect_t){x,     y+h-1, w,   1}, WM_FRAME_HI);
    gfx_fill_rect(c, (rect_t){x+w-1, y,     1,   h}, WM_FRAME_HI);
}

/* ── Compositor (painter's algorithm) ─────────────────────────────────────── */

void wm_composite(wm_state_t *s, canvas_t *screen)
{
    int i;

    /* 1. Desktop background. */
    gfx_fill(screen, WM_DESKTOP_COL);

    /* 2. Back-to-front via z_order (z_order[0]=back, z_order[num_windows-1]=front). */
    for (i = 0; i < s->num_windows; i++) {
        window_t *w = &s->windows[s->z_order[i]];

        rect_t tb    = wm_titlebar_rect(w);
        rect_t full  = wm_full_rect(w);
        rect_t close = wm_close_btn_rect(w);
        color_t tbar_col = w->focused ? WM_TBAR_FOCUSED : WM_TBAR_UNFOCUSED;

        int fx = full.x, fy = full.y, fw = full.w, fh = full.h;

        /* ── Title bar ─────────────────────────────────────────────────── */
        gfx_fill_rect(screen, tb, tbar_col);
        /* Title text — vertically centred, leave room on right for close btn. */
        gfx_draw_text(screen,
                      tb.x + 6,
                      tb.y + (TITLEBAR_H - GFX_CHAR_H) / 2,
                      w->title, WM_TBAR_TEXT_COL, tbar_col);

        /* ── Close button ──────────────────────────────────────────────── */
        gfx_fill_rect(screen, close, WM_CLOSE_COL);
        /* Draw × with two diagonal lines. */
        {
            int pad = 3;
            gfx_draw_line(screen,
                          close.x + pad,            close.y + pad,
                          close.x + close.w-1-pad,  close.y + close.h-1-pad,
                          GFX_WHITE);
            gfx_draw_line(screen,
                          close.x + close.w-1-pad,  close.y + pad,
                          close.x + pad,            close.y + close.h-1-pad,
                          GFX_WHITE);
        }

        /* ── App canvas ────────────────────────────────────────────────── */
        if (w->backbuf) {
            gfx_set_clip(screen, w->bounds);
            gfx_blit(screen, w->bounds.x, w->bounds.y,
                     w->backbuf,
                     (rect_t){0, 0, w->backbuf->w, w->backbuf->h});
            gfx_clear_clip(screen);
        } else {
            gfx_fill_rect(screen, w->bounds, GFX_RGB(20, 20, 20));
        }

        /* Sunken border around content area — drawn over blit edges. */
        sunken_bevel(screen,
                     w->bounds.x, w->bounds.y,
                     w->bounds.w, w->bounds.h);

        /* ── 3D raised frame (outermost layer, drawn last) ─────────────── */
        raised_bevel(screen, fx, fy, fw, fh);

        w->dirty = 0;
    }

    /* The cursor is drawn by the caller AFTER the taskbar, so it stays on top
     * of everything (including the taskbar). */
}

/* ── Taskbar ─────────────────────────────────────────────────────────────── */

void wm_draw_taskbar(canvas_t *screen, int sw, int sh, int taskbar_h,
                     const char **labels, int nlabels, int btn_w)
{
    int i;
    int ty = sh - taskbar_h;

    /* Light gray bar background. */
    gfx_fill_rect(screen, (rect_t){0, ty, sw, taskbar_h}, WM_TASKBAR_COL);

    /* Top edge: dark separator line + inner highlight (classic 3D look). */
    gfx_fill_rect(screen, (rect_t){0, ty,   sw, 1}, WM_FRAME_SHADOW);
    gfx_fill_rect(screen, (rect_t){0, ty+1, sw, 1}, WM_FRAME_HI);

    /* Raised buttons. */
    for (i = 0; i < nlabels; i++) {
        int bx = i * btn_w + 2;
        int by = ty + 3;
        int bw = btn_w - 4;
        int bh = taskbar_h - 6;

        /* Base fill. */
        gfx_fill_rect(screen, (rect_t){bx, by, bw, bh}, WM_TASKBTN_COL);

        /* 3D raised bevel on the button. */
        raised_bevel(screen, bx, by, bw, bh);

        /* Button label — centred vertically, 6px left padding. */
        gfx_draw_text(screen,
                      bx + 6,
                      by + (bh - GFX_CHAR_H) / 2,
                      labels[i], WM_TASKBTN_TEXT, WM_TASKBTN_COL);
    }
}
