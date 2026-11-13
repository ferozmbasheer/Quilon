/*
 * Quilon OS -- a small widget kit for WM apps (ROADMAP3 18.5)
 *
 * "Retained-nothing": a widget is a plain struct the app owns (in its
 * app_state), a draw function (widgets.c, libgfx) and pure event/hit-test
 * helpers (this header -- no syscalls, no drawing, host-tested in
 * tests/test_widgets.c).  The WM knows nothing about widgets.
 *
 *   label      wg_label_draw                 text cut with ".." to a width
 *   button     wg_button_draw                bevelled; sunk while pressed
 *   scrollbar  wg_vscroll_t                  arrows, track paging, thumb drag
 *   list view  wg_list_t                     rows, selection, keyboard nav
 *   scroll list wg_slist_t                   a list + its scrollbar + wheel,
 *                                            the whole mouse protocol
 *   text input wg_input_t                    one editable line with a cursor
 *
 * All rects are in the window's content (canvas) coordinates, the same
 * coordinates WM mouse events use.
 */

#ifndef _USER_WM_WIDGETS_H
#define _USER_WM_WIDGETS_H

#include <gfx.h>
#include "wm.h"

#ifndef MOUSE_BTN_LEFT                /* <unistd.h> on the target */
#define MOUSE_BTN_LEFT 0x01
#endif

/* Colours shared by the apps that use the kit. */
#define WG_FACE        WM_FRAME_BASE             /* buttons, scrollbar thumb */
#define WG_TRACK       GFX_RGB(176, 176, 176)    /* scrollbar track          */
#define WG_TEXT        GFX_BLACK
#define WG_FIELD_BG    GFX_WHITE                 /* text input background    */
#define WG_SEL_BG      GFX_RGB( 50,  50, 150)    /* selected list row        */
#define WG_SEL_FG      GFX_WHITE

/* libgfx's font draws its glyphs in the top 8 rows of the GFX_CHAR_H (16)
 * cell -- the rest is background.  Widgets centre text on the glyph. */
#define WG_GLYPH_H     8

static inline int wg_min(int a, int b) { return a < b ? a : b; }
static inline int wg_max(int a, int b) { return a > b ? a : b; }

/* ── Vertical scrollbar ─────────────────────────────────────────────────────
 * Shows a window of `visible` items starting at `pos` out of `total`.
 *
 *     [^]   up arrow     (WG_SB_W square)
 *     |#|   thumb        (height ~ visible/total of the track, >= WG_SB_MIN)
 *     | |   track        (a click pages up/down)
 *     [v]   down arrow
 */
#define WG_SB_W     14
#define WG_SB_MIN    8      /* smallest thumb */

enum {
    WG_SB_NONE = 0, WG_SB_UP, WG_SB_DOWN, WG_SB_PGUP, WG_SB_PGDN, WG_SB_THUMB
};

typedef struct {
    rect_t r;               /* whole bar, arrows included */
    int    total;
    int    visible;
    int    pos;
} wg_vscroll_t;

static inline int wg_vscroll_max(const wg_vscroll_t *sb)
{
    return wg_max(0, sb->total - sb->visible);
}

/* The track: the bar between the arrows. */
static inline rect_t wg_vscroll_track(const wg_vscroll_t *sb)
{
    int a = wg_min(WG_SB_W, sb->r.h / 2);
    return (rect_t){ sb->r.x, sb->r.y + a, sb->r.w, wg_max(0, sb->r.h - 2 * a) };
}

/* The thumb.  Fills the track when everything is visible. */
static inline rect_t wg_vscroll_thumb(const wg_vscroll_t *sb)
{
    rect_t t = wg_vscroll_track(sb);
    int max = wg_vscroll_max(sb);
    if (max == 0 || sb->total <= 0) return t;
    int th = (int)((long long)t.h * sb->visible / sb->total);
    th = wg_max(th, wg_min(WG_SB_MIN, t.h));
    int pos = wg_max(0, wg_min(sb->pos, max));
    int ty = t.y + (int)((long long)(t.h - th) * pos / max);
    return (rect_t){ t.x, ty, t.w, th };
}

/* Which part of the bar is at (x, y)? */
static inline int wg_vscroll_hit(const wg_vscroll_t *sb, int x, int y)
{
    if (!wm_rect_contains(sb->r, x, y)) return WG_SB_NONE;
    rect_t t = wg_vscroll_track(sb);
    if (y < t.y)         return WG_SB_UP;
    if (y >= t.y + t.h)  return WG_SB_DOWN;
    if (wg_vscroll_max(sb) == 0) return WG_SB_NONE;   /* nothing to scroll */
    rect_t th = wg_vscroll_thumb(sb);
    if (y < th.y)        return WG_SB_PGUP;
    if (y >= th.y + th.h) return WG_SB_PGDN;
    return WG_SB_THUMB;
}

/* The pos that puts the thumb's top at `thumb_y` (a thumb drag). */
static inline int wg_vscroll_pos_at(const wg_vscroll_t *sb, int thumb_y)
{
    rect_t t  = wg_vscroll_track(sb);
    rect_t th = wg_vscroll_thumb(sb);
    int max = wg_vscroll_max(sb), room = t.h - th.h;
    if (max == 0 || room <= 0) return 0;
    int off = wg_max(0, wg_min(thumb_y - t.y, room));
    return (int)(((long long)off * max + room / 2) / room);
}

/* ── List view ──────────────────────────────────────────────────────────────
 * `n` rows of `row_h` pixels in `r`; `top` is the first visible row, `sel`
 * the selected one (-1 = no selection: a plain scrolling view). */
typedef struct {
    rect_t r;
    int    n;
    int    top;
    int    sel;
    int    row_h;
} wg_list_t;

static inline int wg_list_rows(const wg_list_t *l)
{
    return l->row_h > 0 ? wg_max(1, l->r.h / l->row_h) : 1;
}

/* Keep `top` in range (after a resize or a new n). */
static inline void wg_list_clamp(wg_list_t *l)
{
    l->top = wm_scroll_clamp(l->top, 0, l->n, wg_list_rows(l));
    if (l->sel >= l->n) l->sel = l->n - 1;
}

/* Row index at (x, y), or -1 (outside, or below the last row). */
static inline int wg_list_hit(const wg_list_t *l, int x, int y)
{
    if (!wm_rect_contains(l->r, x, y) || l->row_h <= 0) return -1;
    int i = l->top + (y - l->r.y) / l->row_h;
    return i < l->n ? i : -1;
}

/* Scroll so row i is visible. */
static inline void wg_list_show(wg_list_t *l, int i)
{
    int rows = wg_list_rows(l);
    if (i < l->top)         l->top = i;
    if (i >= l->top + rows) l->top = i - rows + 1;
    wg_list_clamp(l);
}

/* Select row i (clamped) and scroll it into view. */
static inline void wg_list_select(wg_list_t *l, int i)
{
    if (l->n <= 0) { l->sel = -1; l->top = 0; return; }
    l->sel = wg_max(0, wg_min(i, l->n - 1));
    wg_list_show(l, l->sel);
}

/* Navigation keys: Up/Down, PgUp/PgDn, Home/End.  With a selection they
 * move it; without one they scroll.  1 = the key was used. */
static inline int wg_list_key(wg_list_t *l, int keycode)
{
    int rows = wg_list_rows(l), page = wg_max(1, rows - 1), d;
    switch (keycode) {
    case WM_KEY_UP:   d = -1;     break;
    case WM_KEY_DOWN: d = 1;      break;
    case WM_KEY_PGUP: d = -page;  break;
    case WM_KEY_PGDN: d = page;   break;
    case WM_KEY_HOME: d = -l->n - rows; break;
    case WM_KEY_END:  d = l->n + rows;  break;
    default: return 0;
    }
    if (l->sel >= 0) wg_list_select(l, l->sel + d);
    else             l->top = wm_scroll_clamp(l->top, d, l->n, rows);
    return 1;
}

/* ── Scroll list: a list view with a scrollbar on its right ─────────────── */
typedef struct {
    wg_list_t    list;
    wg_vscroll_t sb;
    int          drag;      /* 1 = dragging the thumb */
    int          grab;      /* cursor y minus thumb top, while dragging */
} wg_slist_t;

/* Lay the list + scrollbar out in `r`. */
static inline void wg_slist_layout(wg_slist_t *s, rect_t r)
{
    s->list.r = (rect_t){ r.x, r.y, wg_max(0, r.w - WG_SB_W), r.h };
    s->sb.r   = (rect_t){ r.x + r.w - WG_SB_W, r.y, WG_SB_W, r.h };
    wg_list_clamp(&s->list);
}

/* Bring the scrollbar's numbers up to date with the list. */
static inline void wg_slist_sync(wg_slist_t *s)
{
    s->sb.total   = s->list.n;
    s->sb.visible = wg_list_rows(&s->list);
    s->sb.pos     = s->list.top;
}

/* What a mouse event on a scroll list did. */
enum {
    WG_SL_NONE = 0,     /* not for us / no change                          */
    WG_SL_REDRAW,       /* scrolled or selection moved: redraw              */
    WG_SL_SELECT,       /* a row was clicked (list.sel = it): redraw        */
    WG_SL_ACTIVATE,     /* a row was double-clicked (list.sel = it)         */
};

/*
 * Feed a WM mouse event (WM_EV_MOUSE_BTN / _MOVE / _UP / _SCROLL).  Wheel
 * scrolls WM_WHEEL_ROWS per notch; scrollbar arrows scroll a row, the track
 * a page; the thumb drags.  When `selectable`, a left press on a row
 * selects it and a double-click (ev->clicks == 2) on the selected row
 * activates it.
 */
static inline int wg_slist_mouse(wg_slist_t *s, const wm_event_t *ev,
                                 int selectable)
{
    wg_list_t *l = &s->list;
    int rows = wg_list_rows(l), old_top = l->top;
    wg_slist_sync(s);

    switch (ev->type) {
    case WM_EV_SCROLL:
        l->top = wm_scroll_clamp(l->top, ev->wheel * WM_WHEEL_ROWS, l->n, rows);
        return l->top != old_top ? WG_SL_REDRAW : WG_SL_NONE;

    case WM_EV_MOUSE_BTN: {
        if (ev->button != MOUSE_BTN_LEFT) return WG_SL_NONE;
        int part = wg_vscroll_hit(&s->sb, ev->x, ev->y);
        int d = 0;
        switch (part) {
        case WG_SB_UP:   d = -1; break;
        case WG_SB_DOWN: d = 1;  break;
        case WG_SB_PGUP: d = -wg_max(1, rows - 1); break;
        case WG_SB_PGDN: d = wg_max(1, rows - 1);  break;
        case WG_SB_THUMB:
            s->drag = 1;
            s->grab = ev->y - wg_vscroll_thumb(&s->sb).y;
            return WG_SL_REDRAW;                   /* thumb drawn pressed */
        default: break;
        }
        if (d) {
            l->top = wm_scroll_clamp(l->top, d, l->n, rows);
            return l->top != old_top ? WG_SL_REDRAW : WG_SL_NONE;
        }
        int i = wg_list_hit(l, ev->x, ev->y);
        if (i < 0 || !selectable) return WG_SL_NONE;
        if (ev->clicks >= 2 && i == l->sel) return WG_SL_ACTIVATE;
        wg_list_select(l, i);
        return WG_SL_SELECT;
    }

    case WM_EV_MOUSE_MOVE:
        if (!s->drag) return WG_SL_NONE;
        l->top = wg_vscroll_pos_at(&s->sb, ev->y - s->grab);
        wg_list_clamp(l);
        return l->top != old_top ? WG_SL_REDRAW : WG_SL_NONE;

    case WM_EV_MOUSE_UP:
        if (!s->drag || ev->button != MOUSE_BTN_LEFT) return WG_SL_NONE;
        s->drag = 0;
        return WG_SL_REDRAW;

    default:
        return WG_SL_NONE;
    }
}

/* ── Text input: one editable line ──────────────────────────────────────── */
#define WG_INPUT_MAX  256

typedef struct {
    char buf[WG_INPUT_MAX];
    int  len;
    int  cur;               /* cursor: insert position, 0..len */
    int  scroll;            /* first character shown */
} wg_input_t;

enum { WG_IN_NONE = 0, WG_IN_CHANGED, WG_IN_ENTER, WG_IN_CANCEL };

static inline void wg_input_set(wg_input_t *in, const char *s)
{
    int n = 0;
    while (s[n] && n < WG_INPUT_MAX - 1) { in->buf[n] = s[n]; n++; }
    in->buf[n] = '\0';
    in->len = in->cur = n;
    in->scroll = 0;
}

/* Edit with a key: printable characters insert at the cursor; Backspace,
 * Delete, Left/Right, Home/End edit and move; Enter / Esc are reported. */
static inline int wg_input_key(wg_input_t *in, int keycode, char ascii)
{
    int i;
    switch (keycode) {
    case WM_KEY_ENTER: return WG_IN_ENTER;
    case WM_KEY_ESC:   return WG_IN_CANCEL;
    case WM_KEY_LEFT:  if (in->cur > 0) in->cur--;       return WG_IN_CHANGED;
    case WM_KEY_RIGHT: if (in->cur < in->len) in->cur++; return WG_IN_CHANGED;
    case WM_KEY_HOME:  in->cur = 0;                      return WG_IN_CHANGED;
    case WM_KEY_END:   in->cur = in->len;                return WG_IN_CHANGED;
    case WM_KEY_BACKSPACE:
        if (in->cur == 0) return WG_IN_NONE;
        for (i = in->cur - 1; i < in->len; i++) in->buf[i] = in->buf[i + 1];
        in->cur--; in->len--;
        return WG_IN_CHANGED;
    case WM_KEY_DELETE:
        if (in->cur >= in->len) return WG_IN_NONE;
        for (i = in->cur; i < in->len; i++) in->buf[i] = in->buf[i + 1];
        in->len--;
        return WG_IN_CHANGED;
    default:
        break;
    }
    unsigned char c = (unsigned char)ascii;
    if (c < 32 || c > 126 || in->len >= WG_INPUT_MAX - 1) return WG_IN_NONE;
    for (i = in->len; i >= in->cur; i--) in->buf[i + 1] = in->buf[i];
    in->buf[in->cur++] = (char)c;
    in->len++;
    return WG_IN_CHANGED;
}

/* Keep the cursor inside a field `chars` characters wide: adjust scroll. */
static inline void wg_input_fit(wg_input_t *in, int chars)
{
    if (chars < 1) chars = 1;
    if (in->cur < in->scroll) in->scroll = in->cur;
    if (in->cur > in->scroll + chars - 1) in->scroll = in->cur - chars + 1;
    if (in->scroll < 0) in->scroll = 0;
}

/* ── Drawing (widgets.c) ────────────────────────────────────────────────── */

/* Text at (x, y), cut to max_w pixels (".." marks the cut). */
void wg_label_draw(canvas_t *c, int x, int y, int max_w, const char *text,
                   color_t fg, color_t bg);
void wg_button_draw(canvas_t *c, rect_t r, const char *label, int pressed);
void wg_vscroll_draw(canvas_t *c, const wg_vscroll_t *sb, int thumb_down);

/* Row text for wg_list_draw: return the text of row i, set *fg. */
typedef const char *(*wg_row_fn)(void *ctx, int i, color_t *fg);
void wg_list_draw(canvas_t *c, const wg_list_t *l, color_t bg,
                  wg_row_fn row, void *ctx);
void wg_slist_draw(canvas_t *c, wg_slist_t *s, color_t bg,
                   wg_row_fn row, void *ctx);
/* A sunken field; the cursor is shown when `focused`. */
void wg_input_draw(canvas_t *c, rect_t r, wg_input_t *in, int focused);

#endif /* _USER_WM_WIDGETS_H */
