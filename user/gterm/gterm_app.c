/*
 * Quilon OS -- Graphical Terminal as a WM app (section 14.3, single-threaded)
 *
 * Single-threaded model: the terminal is driven entirely by non-blocking WM
 * callbacks on the WM loop thread -- there are NO gterm threads.
 *
 *   gterm_open    -- create two pipes, exec /shell.elf, init emulator state.
 *   on_tick       -- non-blocking drain of the shell's stdout pipe; on new
 *                    bytes, feed the ANSI emulator and re-render.
 *   on_event      -- key  -> write byte to the shell's stdin pipe.
 *                    close -> request destroy.
 *   on_destroy    -- close pipe fds, free state.
 *
 * Because everything runs on the WM loop with no blocking read(), the terminal
 * cannot stall the desktop, and there is no shared-state race with the WM.
 *
 * It does NOT call fork(): fork() in a CLONE_VM context would CoW the shared
 * address space.  exec() creates a child process that inherits the pipe fds.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <gfx.h>
#include "gterm.h"
#include "../wm/wm.h"

typedef struct {
    gterm_t term;
    int     to_shell[2];     /* [0]=shell stdin (read), [1]=we write keys      */
    int     from_shell[2];   /* [0]=we read output,     [1]=shell stdout (write)*/
    int     shell_pid;
    int     exited;          /* 1 once "[shell exited]" has been shown          */
} gterm_state_t;

/* ── Renderer ────────────────────────────────────────────────────────────── */

static void gterm_render(gterm_t *t, canvas_t *c, int cur_visible)
{
    int r, col;
    gfx_fill(c, (color_t)GT_DEFAULT_BG);
    for (r = 0; r < GTERM_ROWS; r++) {
        for (col = 0; col < GTERM_COLS; col++) {
            gterm_cell_t *cell = &t->cells[r][col];
            char str[2] = { cell->ch ? cell->ch : ' ', '\0' };
            gfx_draw_text(c, col * GTERM_CELL_W, r * GTERM_CELL_H,
                          str, (color_t)cell->fg, (color_t)cell->bg);
        }
    }
    if (cur_visible) {
        gterm_cell_t *cc = &t->cells[t->cursor_row][t->cursor_col];
        rect_t cr = { t->cursor_col * GTERM_CELL_W,
                      t->cursor_row * GTERM_CELL_H,
                      GTERM_CELL_W, GTERM_CELL_H };
        char str[2] = { cc->ch ? cc->ch : ' ', '\0' };
        gfx_fill_rect(c, cr, (color_t)cc->fg);
        gfx_draw_text(c, cr.x, cr.y, str, (color_t)cc->bg, (color_t)cc->fg);
    }
}

/* ── Callbacks ───────────────────────────────────────────────────────────── */

/* Non-blocking drain of the shell's output pipe; re-render on new data. */
static void gterm_tick(window_t *win)
{
    gterm_state_t *st = (gterm_state_t *)win->app_state;
    if (st->exited) return;

    char ibuf[256];
    int n = read_nonblock(st->from_shell[0], ibuf, sizeof(ibuf));
    if (n > 0) {
        gterm_write(&st->term, ibuf, n);
        gterm_render(&st->term, win->backbuf, 1);
        win->dirty = 1;
    } else if (n == 0) {
        /* No data ready -- but distinguish "empty" from "writer closed" (EOF).
         * vfs reports 0 for both via read_nonblock; we detect a dead shell by
         * a blocking read returning 0 only when there are no writers.  Keep it
         * simple: rely on read_nonblock and treat persistent 0 as idle.  A
         * proper EOF notice would need a readable()-returns-EOF signal. */
    }
}

static void gterm_event(window_t *win, const wm_event_t *ev)
{
    gterm_state_t *st = (gterm_state_t *)win->app_state;
    if (ev->type == WM_EV_CLOSE) { win->want_close = 1; return; }
    if (ev->type == WM_EV_KEY && ev->ascii)
        write(st->to_shell[1], &ev->ascii, 1);
}

static void gterm_destroy(window_t *win)
{
    gterm_state_t *st = (gterm_state_t *)win->app_state;
    if (st) {
        /* Close our pipe ends.  The shell child will see EOF on its stdin and
         * a broken pipe on its stdout, and exit; its own fds are closed when it
         * is reaped (SYS_EXIT close-on-exit).  We do not wait() for it here --
         * the WM loop must not block. */
        close(st->to_shell[0]);
        close(st->to_shell[1]);
        close(st->from_shell[0]);
        close(st->from_shell[1]);
        free(st);
        win->app_state = (void *)0;
    }
    if (win->backbuf) { canvas_free(win->backbuf); win->backbuf = (canvas_t *)0; }
}

/* ── Launcher entry point ────────────────────────────────────────────────── */

void gterm_open(window_t *win)
{
    gterm_state_t *st = (gterm_state_t *)malloc(sizeof(gterm_state_t));
    if (!st) { win->want_close = 1; return; }
    st->shell_pid = -1;
    st->exited    = 0;

    if (pipe(st->to_shell) || pipe(st->from_shell)) {
        free(st);
        win->want_close = 1;
        return;
    }

    /* Spawn the shell with its stdin = to_shell[0], stdout = from_shell[1],
     * using exec_redir so the WM's OWN stdin/stdout are untouched -- the WM
     * loop keeps reading the real keyboard via read_nonblock(STDIN). */
    st->shell_pid = exec_redir("/shell.elf", st->to_shell[0], st->from_shell[1]);

    if (st->shell_pid < 0) {
        close(st->to_shell[0]); close(st->to_shell[1]);
        close(st->from_shell[0]); close(st->from_shell[1]);
        free(st);
        win->want_close = 1;
        return;
    }

    gterm_init(&st->term);

    win->app_state  = st;
    win->on_tick    = gterm_tick;
    win->on_event   = gterm_event;
    win->on_destroy = gterm_destroy;

    gterm_render(&st->term, win->backbuf, 1);
    win->dirty = 1;
}
