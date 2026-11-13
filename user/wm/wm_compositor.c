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

void wm_bevel_raised(canvas_t *c, rect_t r) { raised_bevel(c, r.x, r.y, r.w, r.h); }
void wm_bevel_sunken(canvas_t *c, rect_t r) { sunken_bevel(c, r.x, r.y, r.w, r.h); }

/* ── Desktop wallpaper (ROADMAP3 18.5) ───────────────────────────────────── */
static canvas_t *g_wallpaper;
void wm_set_wallpaper(canvas_t *bg) { g_wallpaper = bg; }

/* ── Title-bar buttons + grip (ROADMAP3 18.3) ──────────────────────────── */

/* A 2-px-thick line: the glyphs are too thin to read at 1 px. */
static void thick_line(canvas_t *c, int x0, int y0, int x1, int y1, color_t col)
{
    gfx_draw_line(c, x0, y0, x1, y1, col);
    gfx_draw_line(c, x0 + 1, y0, x1 + 1, y1, col);
}

static void draw_tbtn(canvas_t *c, const window_t *w, int which, int hover)
{
    rect_t r = wm_tbtn_rect(w, which);
    if (wm_rect_empty(r)) return;
    int red = (which == WM_TBTN_CLOSE && hover);
    color_t face  = red ? WM_CLOSE_COL : WM_FRAME_BASE;
    color_t glyph = red ? GFX_WHITE : WM_TBTN_GLYPH;
    gfx_fill_rect(c, r, face);
    raised_bevel(c, r.x, r.y, r.w, r.h);

    int x0 = r.x + 4, y0 = r.y + 4;          /* glyph box: 7 x 7 */
    int x1 = r.x + r.w - 6, y1 = r.y + r.h - 6;
    switch (which) {
    case WM_TBTN_CLOSE:
        thick_line(c, x0, y0, x1 - 1, y1, glyph);
        thick_line(c, x1 - 1, y0, x0, y1, glyph);
        break;
    case WM_TBTN_MAX:
        if (w->maximized) {
            /* "Restore": two overlapping frames. */
            gfx_draw_rect(c, (rect_t){ x0 + 2, y0, x1 - x0 - 1, y1 - y0 - 1 }, glyph);
            gfx_fill_rect(c, (rect_t){ x0, y0 + 2, x1 - x0 - 1, y1 - y0 - 1 }, face);
            gfx_draw_rect(c, (rect_t){ x0, y0 + 2, x1 - x0 - 1, y1 - y0 - 1 }, glyph);
            gfx_fill_rect(c, (rect_t){ x0, y0 + 2, x1 - x0 - 1, 2 }, glyph);
        } else {
            gfx_draw_rect(c, (rect_t){ x0, y0, x1 - x0 + 1, y1 - y0 + 1 }, glyph);
            gfx_fill_rect(c, (rect_t){ x0, y0, x1 - x0 + 1, 2 }, glyph);
        }
        break;
    case WM_TBTN_MIN:
        gfx_fill_rect(c, (rect_t){ x0, y1 - 1, x1 - x0 + 1, 2 }, glyph);
        break;
    }
}

/* Three diagonal ridges in the bottom-right corner of the content. */
static void draw_grip(canvas_t *c, const window_t *w)
{
    rect_t g = wm_grip_rect(w);
    if (wm_rect_empty(g)) return;
    int x1 = g.x + g.w - 2, y1 = g.y + g.h - 2;
    for (int k = 3; k <= 9; k += 3) {
        gfx_draw_line(c, x1 - k, y1, x1, y1 - k, WM_FRAME_HI);
        gfx_draw_line(c, x1 - k + 1, y1, x1, y1 - k + 1, WM_FRAME_SHADOW);
    }
}

void wm_draw_outline(canvas_t *screen, rect_t r)
{
    if (wm_rect_empty(r)) return;
    gfx_draw_rect(screen, r, WM_OUTLINE_DARK);
    if (r.w > 2 && r.h > 2)
        gfx_draw_rect(screen, (rect_t){ r.x + 1, r.y + 1, r.w - 2, r.h - 2 },
                      WM_OUTLINE_LIGHT);
}

/* ── Compositor (painter's algorithm) ─────────────────────────────────────── */

/*
 * wm_composite_rect -- repaint the desktop and every window, back to front,
 * restricted to `clip` (ROADMAP3 18.1).  Every primitive used here honours
 * the canvas clip rect, so the work is proportional to the damaged area --
 * except that each window's chrome is still visited, which is cheap.
 * Windows that don't touch `clip` are skipped outright.
 */
void wm_composite_rect(wm_state_t *s, canvas_t *screen, rect_t clip)
{
    int i;

    gfx_set_clip(screen, clip);
    if (!screen->has_clip) { gfx_clear_clip(screen); return; }   /* empty */

    /* 1. Desktop background. */
    if (g_wallpaper)        /* pre-rendered, screen-sized: a straight copy */
        gfx_blit(screen, clip.x, clip.y, g_wallpaper, clip);
    else
        gfx_fill_rect(screen, clip, WM_DESKTOP_COL);

    /* 2. Back-to-front via z_order (z_order[0]=back, z_order[num_windows-1]=front). */
    for (i = 0; i < s->num_windows; i++) {
        window_t *w = &s->windows[s->z_order[i]];
        if (w->minimized) continue;           /* hidden, still alive (18.3) */

        rect_t full  = wm_full_rect(w);
        if (!wm_rect_intersects(full, clip)) continue;

        rect_t tb    = wm_titlebar_rect(w);
        color_t tbar_col = w->focused ? WM_TBAR_FOCUSED : WM_TBAR_UNFOCUSED;

        int fx = full.x, fy = full.y, fw = full.w, fh = full.h;

        /* ── Title bar ─────────────────────────────────────────────────── */
        gfx_fill_rect(screen, tb, tbar_col);
        /* Title text — vertically centred; dimmed when unfocused.  The
         * buttons are drawn over its right end. */
        gfx_draw_text(screen,
                      tb.x + 6,
                      tb.y + (TITLEBAR_H - GFX_CHAR_H) / 2,
                      w->title,
                      w->focused ? WM_TBAR_TEXT_COL : WM_TBAR_TEXT_UNFOC,
                      tbar_col);

        /* ── Title-bar buttons: minimize, maximize, close ──────────────── */
        draw_tbtn(screen, w, WM_TBTN_MIN,   0);
        draw_tbtn(screen, w, WM_TBTN_MAX,   0);
        draw_tbtn(screen, w, WM_TBTN_CLOSE, s->hover_close_id == w->id);

        /* ── App canvas ────────────────────────────────────────────────── */
        if (w->backbuf) {
            /* gfx_blit clips to the screen clip (= the damage rect); limit
             * the source to the content area so an oversized canvas can't
             * spill over the chrome. */
            rect_t src = { 0, 0,
                           w->backbuf->w < w->bounds.w ? w->backbuf->w : w->bounds.w,
                           w->backbuf->h < w->bounds.h ? w->backbuf->h : w->bounds.h };
            gfx_blit(screen, w->bounds.x, w->bounds.y, w->backbuf, src);
        } else {
            gfx_fill_rect(screen, w->bounds, GFX_RGB(20, 20, 20));
        }

        /* Sunken border around content area — drawn over blit edges. */
        sunken_bevel(screen,
                     w->bounds.x, w->bounds.y,
                     w->bounds.w, w->bounds.h);
        draw_grip(screen, w);

        /* ── 3D raised frame (outermost layer, drawn last) ─────────────── */
        raised_bevel(screen, fx, fy, fw, fh);
    }

    gfx_clear_clip(screen);
    /* The cursor is drawn by the caller AFTER the taskbar, so it stays on top
     * of everything (including the taskbar). */
}

/* Full-screen composite (initial frame, tests).  Clears every dirty flag. */
void wm_composite(wm_state_t *s, canvas_t *screen)
{
    rect_t all = { 0, 0, screen->w, screen->h };
    wm_composite_rect(s, screen, all);
    for (int i = 0; i < s->num_windows; i++)
        s->windows[s->z_order[i]].dirty = 0;
    if (s->resize_win_id >= 0)
        wm_draw_outline(screen, wm_resize_outline(s));
}

/* ── Taskbar (ROADMAP3 18.4) ─────────────────────────────────────────────── */

/* A taskbar/dialog button: raised, or sunken with the label nudged 1 px when
 * pressed.  The label is centred, or left-aligned (window list), and cut
 * with ".." to fit. */
static void draw_button(canvas_t *c, rect_t r, const char *label,
                        int pressed, color_t text, int centre)
{
    if (r.w <= 0 || r.h <= 0) return;
    gfx_fill_rect(c, r, pressed ? WM_TASKBTN_DOWN : WM_TASKBTN_COL);
    if (pressed) sunken_bevel(c, r.x, r.y, r.w, r.h);
    else         raised_bevel(c, r.x, r.y, r.w, r.h);

    char buf[WM_TITLE_LEN];
    int max = (r.w - 12) / GFX_CHAR_W, len = 0;
    if (max <= 0) return;
    if (max > WM_TITLE_LEN - 1) max = WM_TITLE_LEN - 1;
    while (label[len] && len < max) { buf[len] = label[len]; len++; }
    if (label[len] && len >= 3) buf[len - 1] = buf[len - 2] = '.';
    buf[len] = '\0';

    int lx = centre ? r.x + (r.w - len * GFX_CHAR_W) / 2 : r.x + 6;
    if (lx < r.x + 6) lx = r.x + 6;
    int ly = r.y + (r.h - GFX_CHAR_H) / 2;
    if (pressed) { lx++; ly++; }
    gfx_draw_text(c, lx, ly, buf, text, pressed ? WM_TASKBTN_DOWN : WM_TASKBTN_COL);
}

void wm_draw_taskbar(canvas_t *screen, int sw, int sh, int taskbar_h,
                     const wm_taskbar_view_t *v)
{
    int ty = sh - taskbar_h;

    /* Light gray bar background. */
    gfx_fill_rect(screen, (rect_t){0, ty, sw, taskbar_h}, WM_TASKBAR_COL);

    /* Top edge: dark separator line + inner highlight (classic 3D look). */
    gfx_fill_rect(screen, (rect_t){0, ty,   sw, 1}, WM_FRAME_SHADOW);
    gfx_fill_rect(screen, (rect_t){0, ty+1, sw, 1}, WM_FRAME_HI);

    /* Apps button: held down while its menu is open. */
    draw_button(screen, wm_taskbar_start_rect(sh, taskbar_h), WM_START_LABEL,
                v->start_down, WM_TASKBTN_TEXT, 1);

    /* One button per window; the focused one pressed in, minimized ones
     * dimmed. */
    for (int i = 0; i < v->n; i++)
        draw_button(screen, wm_winlist_btn_rect(i, v->n, sw, sh, taskbar_h),
                    v->label[i], v->pressed[i],
                    v->minimized[i] ? WM_TASKBTN_DIM : WM_TASKBTN_TEXT, 0);

    /* Tray clock: sunken, like a status field. */
    rect_t cr = wm_taskbar_clock_rect(sw, sh, taskbar_h);
    sunken_bevel(screen, cr.x, cr.y, cr.w, cr.h);
    gfx_draw_text(screen, cr.x + (cr.w - 5 * GFX_CHAR_W) / 2,
                  cr.y + (cr.h - GFX_CHAR_H) / 2, v->clock,
                  WM_TASKBTN_TEXT, WM_TASKBAR_COL);

    /* Power button, right-aligned. */
    draw_button(screen, wm_taskbar_power_rect(sw, sh, taskbar_h),
                WM_POWER_LABEL, 0, WM_TASKBTN_TEXT, 1);
}

void wm_draw_menu(canvas_t *screen, int sh, int taskbar_h,
                  const char *const *labels, int n, int hover)
{
    rect_t m = wm_menu_rect(n, sh, taskbar_h);
    gfx_fill_rect(screen, m, WM_FRAME_BASE);
    raised_bevel(screen, m.x, m.y, m.w, m.h);
    for (int i = 0; i < n; i++) {
        rect_t r = wm_menu_item_rect(i, n, sh, taskbar_h);
        color_t bg = (i == hover) ? WM_TBAR_FOCUSED : WM_FRAME_BASE;
        color_t fg = (i == hover) ? WM_TBAR_TEXT_COL : WM_TASKBTN_TEXT;
        gfx_fill_rect(screen, r, bg);
        gfx_draw_text(screen, r.x + 10, r.y + (r.h - GFX_CHAR_H) / 2,
                      labels[i], fg, bg);
    }
}

void wm_draw_button(canvas_t *c, rect_t r, const char *label)
{
    draw_button(c, r, label, 0, WM_TASKBTN_TEXT, 1);
}
