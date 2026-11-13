/*
 * Quilon OS -- widget kit drawing (ROADMAP3 18.5)
 *
 * The draw half of widgets.h.  Only libgfx calls and the WM's bevels; the
 * logic lives in the header.  Linked into the WM and into tests/test_widgets.
 */

#include <gfx.h>
#include "widgets.h"

void wg_label_draw(canvas_t *c, int x, int y, int max_w, const char *text,
                   color_t fg, color_t bg)
{
    char buf[128];
    int max = max_w / GFX_CHAR_W, n = 0;
    if (max <= 0) return;
    if (max > (int)sizeof buf - 1) max = (int)sizeof buf - 1;
    while (text[n] && n < max) { buf[n] = text[n]; n++; }
    if (text[n] && n >= 3) buf[n - 1] = buf[n - 2] = '.';
    buf[n] = '\0';
    gfx_draw_text(c, x, y, buf, fg, bg);
}

void wg_button_draw(canvas_t *c, rect_t r, const char *label, int pressed)
{
    if (r.w <= 0 || r.h <= 0) return;
    gfx_fill_rect(c, r, WG_FACE);
    if (pressed) wm_bevel_sunken(c, r);
    else         wm_bevel_raised(c, r);
    int len = 0;
    while (label[len]) len++;
    int lx = r.x + (r.w - len * GFX_CHAR_W) / 2;
    if (lx < r.x + 3) lx = r.x + 3;
    int ly = r.y + (r.h - WG_GLYPH_H) / 2;
    if (pressed) { lx++; ly++; }
    /* The cell's blank lower half must not paint over the bevel. */
    gfx_set_clip(c, (rect_t){ r.x + 2, r.y + 2, r.w - 4, r.h - 4 });
    wg_label_draw(c, lx, ly, r.x + r.w - 3 - lx, label, WG_TEXT, WG_FACE);
    gfx_clear_clip(c);
}

/* A small filled triangle pointing up (dir < 0) or down, centred in r. */
static void draw_arrow(canvas_t *c, rect_t r, int dir)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    for (int i = 0; i < 4; i++) {
        int y = dir < 0 ? cy - 2 + i : cy + 1 - i;
        gfx_fill_rect(c, (rect_t){ cx - i, y, 2 * i + 1, 1 }, WG_TEXT);
    }
}

void wg_vscroll_draw(canvas_t *c, const wg_vscroll_t *sb, int thumb_down)
{
    rect_t t = wg_vscroll_track(sb);
    if (sb->r.w <= 0 || sb->r.h <= 0) return;
    gfx_fill_rect(c, t, WG_TRACK);

    rect_t up = { sb->r.x, sb->r.y, sb->r.w, t.y - sb->r.y };
    rect_t dn = { sb->r.x, t.y + t.h, sb->r.w, sb->r.y + sb->r.h - (t.y + t.h) };
    gfx_fill_rect(c, up, WG_FACE);  wm_bevel_raised(c, up);  draw_arrow(c, up, -1);
    gfx_fill_rect(c, dn, WG_FACE);  wm_bevel_raised(c, dn);  draw_arrow(c, dn, 1);

    if (wg_vscroll_max(sb) > 0) {      /* no thumb when nothing scrolls */
        rect_t th = wg_vscroll_thumb(sb);
        gfx_fill_rect(c, th, WG_FACE);
        if (thumb_down) wm_bevel_sunken(c, th);
        else            wm_bevel_raised(c, th);
    }
}

void wg_list_draw(canvas_t *c, const wg_list_t *l, color_t bg,
                  wg_row_fn row, void *ctx)
{
    gfx_fill_rect(c, l->r, bg);
    int rows = wg_list_rows(l) + 1;          /* + a partly visible last row */
    for (int k = 0; k < rows; k++) {
        int i = l->top + k;
        if (i >= l->n) break;
        int y = l->r.y + k * l->row_h;
        if (y >= l->r.y + l->r.h) break;
        color_t fg = WG_TEXT, rbg = bg;
        const char *text = row(ctx, i, &fg);
        rect_t rr = wm_rect_intersect((rect_t){ l->r.x, y, l->r.w, l->row_h },
                                      l->r);
        gfx_clear_clip(c);                    /* the last row's */
        if (i == l->sel) {
            rbg = WG_SEL_BG;
            fg  = WG_SEL_FG;
            gfx_fill_rect(c, rr, rbg);
        }
        gfx_set_clip(c, rr);                  /* text cell taller than a row */
        wg_label_draw(c, l->r.x + 4, y + (l->row_h - WG_GLYPH_H) / 2,
                      l->r.w - 4, text ? text : "", fg, rbg);
    }
    gfx_clear_clip(c);
}

void wg_slist_draw(canvas_t *c, wg_slist_t *s, color_t bg,
                   wg_row_fn row, void *ctx)
{
    wg_slist_sync(s);
    wg_list_draw(c, &s->list, bg, row, ctx);
    wg_vscroll_draw(c, &s->sb, s->drag);
}

void wg_input_draw(canvas_t *c, rect_t r, wg_input_t *in, int focused)
{
    if (r.w <= 4 || r.h <= 4) return;
    gfx_fill_rect(c, r, WG_FIELD_BG);
    wm_bevel_sunken(c, r);

    int chars = (r.w - 8) / GFX_CHAR_W;
    wg_input_fit(in, chars);
    int tx = r.x + 4, ty = r.y + (r.h - WG_GLYPH_H) / 2;

    rect_t inner = { r.x + 2, r.y + 2, r.w - 4, r.h - 4 };
    gfx_set_clip(c, inner);
    char buf[WG_INPUT_MAX];
    int n = 0;
    for (int i = in->scroll; i < in->len && n < chars; i++) buf[n++] = in->buf[i];
    buf[n] = '\0';
    gfx_draw_text(c, tx, ty, buf, WG_TEXT, WG_FIELD_BG);
    if (focused) {
        int cx = tx + (in->cur - in->scroll) * GFX_CHAR_W;
        gfx_fill_rect(c, (rect_t){ cx, ty - 2, 2, WG_GLYPH_H + 4 }, WG_TEXT);
    }
    gfx_clear_clip(c);
}
