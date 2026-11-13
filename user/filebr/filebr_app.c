/*
 * Quilon OS -- File Browser app (section 14.4; widget kit, ROADMAP3 18.5)
 *
 *   [Up] [ /path/of/this/dir           ]     toolbar: button + path bar
 *   ..                                  |^|
 *   SUBDIR                              |#|  list view + scrollbar
 *   README.TXT                          | |
 *   HELLO.ELF                           |v|
 *
 * Click selects, double-click (or Enter) opens: a directory is entered, an
 * .elf runs in a new Terminal window, anything else opens in a Textview.
 * Backspace or the Up button goes to the parent.  Click the path bar (or
 * press Ctrl-L / '/') to type a path; Enter goes there, Esc cancels.  Wheel,
 * arrows, Page Up/Down, Home/End and the scrollbar scroll.
 *
 * The browser keeps its own directory path and never leaves the process in
 * it: the kernel's cwd isn't per process yet (REVIEW.md), so a lasting
 * chdir() here would move every terminal's shell too.  Listing a directory
 * is chdir there, readdir, chdir back.
 *
 * Non-blocking WM callbacks; per-window state in win->app_state.
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <gfx.h>
#include "../wm/wm.h"
#include "../wm/widgets.h"
#include "fb_path.h"

#define MAX_ENTRIES  256
#define LIST_ITEM_H   16
#define TOOLBAR_H     26
#define UP_BTN        ((rect_t){ 4, 3, 36, 20 })

#define FB_BG         GFX_RGB(20, 20, 20)
#define FB_DIR_FG     GFX_RGB(100, 180, 255)
#define FB_FILE_FG    GFX_RGB(220, 220, 220)
#define FB_PROG_FG    GFX_RGB(120, 220, 120)

typedef struct {
    fb_entry_t entries[MAX_ENTRIES];
    char       cwd[FB_PATH_MAX];      /* the directory shown            */
    wg_slist_t view;                  /* entries + scrollbar            */
    wg_input_t path;                  /* the path bar                   */
    int        editing;               /* 1 = keys go to the path bar    */
    int        up_down;               /* Up button held                 */
} fb_state_t;

static rect_t fb_path_rect(int w) { return (rect_t){ 44, 3, w - 48, 20 }; }

/* Read directory `dir` into st->entries.  -1 (and nothing changed) if it
 * can't be entered. */
static int fb_load(fb_state_t *st, const char *dir)
{
    char saved[FB_PATH_MAX];
    if (!getcwd(saved, sizeof saved)) { saved[0] = '/'; saved[1] = '\0'; }
    if (chdir(dir) != 0) return -1;

    int n = 0;
    if (!(dir[0] == '/' && dir[1] == '\0')) {          /* a way back up */
        fb_copy(st->entries[0].name, "..");
        st->entries[0].is_dir = 1;
        n = 1;
    }
    dirent_t ent;
    for (unsigned int i = 0; n < MAX_ENTRIES && readdir(i, &ent) == 0; i++) {
        /* "." is useless here; ".." we added ourselves (root has none). */
        if (ent.name[0] == '.' && (!ent.name[1] ||
            (ent.name[1] == '.' && !ent.name[2])))
            continue;
        int len = (int)strlen(ent.name);
        if (len >= FB_NAME_MAX) len = FB_NAME_MAX - 1;
        memcpy(st->entries[n].name, ent.name, (size_t)len);
        st->entries[n].name[len] = '\0';
        st->entries[n].is_dir = (ent.type == DIRENT_TYPE_DIR);
        n++;
    }
    chdir(saved);

    fb_sort(st->entries, n);
    if (dir != st->cwd) fb_copy(st->cwd, dir);
    st->view.list.n   = n;
    st->view.list.top = 0;
    st->view.list.sel = n > 0 ? 0 : -1;
    wg_input_set(&st->path, st->cwd);
    return 0;
}

static const char *fb_row(void *ctx, int i, color_t *fg)
{
    fb_state_t *st = (fb_state_t *)ctx;
    fb_entry_t *e  = &st->entries[i];
    switch (fb_open_kind(e->name, e->is_dir)) {
    case FB_OPEN_DIR:  *fg = FB_DIR_FG;  break;
    case FB_OPEN_PROG: *fg = FB_PROG_FG; break;
    default:           *fg = FB_FILE_FG; break;
    }
    return e->name;
}

static void fb_render(window_t *win)
{
    fb_state_t *st = (fb_state_t *)win->app_state;
    canvas_t   *c  = win->backbuf;
    gfx_fill_rect(c, (rect_t){ 0, 0, c->w, TOOLBAR_H }, WG_FACE);
    wg_button_draw(c, UP_BTN, "Up", st->up_down);
    wg_input_draw(c, fb_path_rect(c->w), &st->path, st->editing);
    wg_slist_draw(c, &st->view, FB_BG, fb_row, st);
    win->dirty = 1;
}

static void fb_layout(fb_state_t *st, int w, int h)
{
    wg_slist_layout(&st->view, (rect_t){ 0, TOOLBAR_H, w, h - TOOLBAR_H });
}

/* Go to `path` (relative to the shown directory, or absolute): enter a
 * directory, open a file. */
static void fb_go(fb_state_t *st, const char *name, int is_dir)
{
    char full[FB_PATH_MAX];
    if (fb_path_join(st->cwd, name, full) != 0) return;
    switch (fb_open_kind(full, is_dir)) {
    case FB_OPEN_DIR:  fb_load(st, full);   break;
    case FB_OPEN_PROG: wm_open_program(full); break;
    default:           wm_open_textview(full); break;
    }
}

static void fb_up(fb_state_t *st);

static void fb_activate(fb_state_t *st)
{
    int i = st->view.list.sel;
    if (i < 0 || i >= st->view.list.n) return;
    if (strcmp(st->entries[i].name, "..") == 0) { fb_up(st); return; }
    fb_go(st, st->entries[i].name, st->entries[i].is_dir);
}

/* Select the entry called `name`, if there is one. */
static void fb_select_name(fb_state_t *st, const char *name)
{
    for (int i = 0; i < st->view.list.n; i++)
        if (strcmp(st->entries[i].name, name) == 0) {
            wg_list_select(&st->view.list, i);
            return;
        }
}

/* To the parent directory, with the one we came from selected. */
static void fb_up(fb_state_t *st)
{
    char up[FB_PATH_MAX], child[FB_NAME_MAX];
    const char *base = st->cwd;
    for (const char *p = st->cwd; *p; p++)
        if (*p == '/' && p[1]) base = p + 1;
    int n = 0;
    while (base[n] && base[n] != '/' && n < FB_NAME_MAX - 1) { child[n] = base[n]; n++; }
    child[n] = '\0';
    fb_copy(up, st->cwd);
    fb_parent(up);
    if (fb_load(st, up) == 0) fb_select_name(st, child);
}

/* Enter in the path bar: a directory is shown, a file opened. */
static void fb_path_enter(fb_state_t *st)
{
    char full[FB_PATH_MAX];
    stat_t sb;
    st->editing = 0;
    if (fb_path_join(st->cwd, st->path.buf, full) == 0 &&
        stat(full, &sb) == 0) {
        fb_go(st, full, S_ISDIR(sb.st_type));
        if (!S_ISDIR(sb.st_type)) wg_input_set(&st->path, st->cwd);
    } else {
        wg_input_set(&st->path, st->cwd);          /* no such path */
    }
}

static void fb_key(fb_state_t *st, const wm_event_t *ev)
{
    if (st->editing) {
        switch (wg_input_key(&st->path, ev->keycode, ev->ascii)) {
        case WG_IN_ENTER:  fb_path_enter(st); break;
        case WG_IN_CANCEL: st->editing = 0; wg_input_set(&st->path, st->cwd); break;
        default: break;
        }
        return;
    }
    if ((ev->keycode == 'l' && (ev->mods & WM_MOD_CTRL)) || ev->keycode == '/') {
        st->editing = 1;
        if (ev->keycode == '/') wg_input_set(&st->path, "/");
        return;
    }
    switch (ev->keycode) {
    case WM_KEY_ENTER:     fb_activate(st); return;
    case WM_KEY_BACKSPACE: fb_up(st);       return;
    case 'j': wg_list_key(&st->view.list, WM_KEY_DOWN); return;
    case 'k': wg_list_key(&st->view.list, WM_KEY_UP);   return;
    default:  wg_list_key(&st->view.list, ev->keycode); return;
    }
}

static void fb_event(window_t *win, const wm_event_t *ev)
{
    fb_state_t *st = (fb_state_t *)win->app_state;

    switch (ev->type) {
    case WM_EV_CLOSE:
        win->want_close = 1;
        return;
    case WM_EV_RESIZE:                        /* ROADMAP3 18.3 */
        fb_layout(st, ev->width, ev->height);
        break;
    case WM_EV_KEY:
        fb_key(st, ev);
        break;
    case WM_EV_MOUSE_BTN:
        if (ev->button == MOUSE_BTN_LEFT && ev->y < TOOLBAR_H) {
            if (wm_rect_contains(UP_BTN, ev->x, ev->y)) {
                st->up_down = 1;
            } else if (wm_rect_contains(fb_path_rect(win->bounds.w),
                                        ev->x, ev->y)) {
                st->editing = 1;              /* cursor where clicked */
                int col = st->path.scroll +
                          (ev->x - fb_path_rect(win->bounds.w).x - 4 +
                           GFX_CHAR_W / 2) / GFX_CHAR_W;
                st->path.cur = col < 0 ? 0 : col > st->path.len ? st->path.len : col;
            }
            break;
        }
        st->editing = 0;
        if (wg_slist_mouse(&st->view, ev, 1) == WG_SL_ACTIVATE)
            fb_activate(st);
        break;
    case WM_EV_MOUSE_UP:
        if (st->up_down && ev->button == MOUSE_BTN_LEFT) {
            st->up_down = 0;
            if (wm_rect_contains(UP_BTN, ev->x, ev->y)) fb_up(st);
            break;
        }
        if (wg_slist_mouse(&st->view, ev, 1) == WG_SL_NONE) return;
        break;
    default:                                  /* wheel, thumb drag */
        if (wg_slist_mouse(&st->view, ev, 1) == WG_SL_NONE) return;
        break;
    }
    fb_render(win);
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
    memset(st, 0, sizeof *st);
    st->view.list.row_h = LIST_ITEM_H;
    fb_layout(st, win->backbuf->w, win->backbuf->h);

    /* Start where the WM was started (normally "/"). */
    char start[FB_PATH_MAX];
    if (!getcwd(start, sizeof start) || fb_load(st, start) != 0) {
        fb_copy(start, "/");
        fb_load(st, start);
    }

    win->app_state  = st;
    win->on_event   = fb_event;
    win->on_destroy = fb_destroy;

    fb_render(win);
}
