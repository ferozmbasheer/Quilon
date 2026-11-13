/*
 * Quilon OS -- Graphical Terminal as a WM app (section 14.3, single-threaded)
 *
 * Single-threaded model: the terminal is driven entirely by non-blocking WM
 * callbacks on the WM loop thread -- there are NO gterm threads.
 *
 *   gterm_open    -- create two pipes, exec /shell.elf, init emulator state.
 *   on_tick       -- (when the pipe is readable -- win->wait_fd) non-blocking
 *                    drain of the shell's stdout pipe; on new
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
#include <signal.h>
#include "gterm.h"
#include "../wm/wm.h"
#include "../wm/widgets.h"

/* Scrollback lines per terminal (ROADMAP3 18.5): GTERM_HIST_LINES x 160
 * cells x 12 bytes = ~480 KiB, allocated with the window. */
#define GTERM_HIST_LINES 256

typedef struct {
    gterm_t term;
    int     to_shell[2];     /* [0]=shell stdin (read), [1]=we write keys      */
    int     from_shell[2];   /* [0]=we read output,     [1]=shell stdout (write)*/
    int     shell_pid;       /* the child: a shell, or a program (prog = 1)     */
    int     exited;          /* 1 once the child has exited                     */
    int     prog;            /* 1 = running a program, not a shell (18.5): on
                                exit the window stays open to show its output,
                                and closes on the next key                      */
    gterm_cell_t (*hist)[GTERM_MAX_COLS];   /* scrollback ring (heap)          */
    int     sb_drag;         /* 1 = dragging the scrollbar thumb                */
    int     sb_grab;         /* cursor y minus the thumb's top, while dragging  */
} gterm_state_t;

/* ── Renderer ────────────────────────────────────────────────────────────── */

/* The scrollbar down the right edge (widget kit, 18.5): the whole history
 * plus the screen, the screen-sized window at the scrolled-back position.
 * The grid is WG_SB_W narrower than the window to make room. */
static wg_vscroll_t gterm_sb(const gterm_t *t, const canvas_t *c)
{
    wg_vscroll_t sb;
    sb.r       = (rect_t){ c->w - WG_SB_W, 0, WG_SB_W, c->h };
    sb.total   = t->hist_n + t->rows;
    sb.visible = t->rows;
    sb.pos     = t->hist_n - t->view;
    return sb;
}

/* Grid size for a w x h content area. */
static void gterm_fit(gterm_t *t, int w, int h)
{
    gterm_resize(t, h / GTERM_CELL_H, (w - WG_SB_W) / GTERM_CELL_W);
}

static void gterm_render(gterm_state_t *st, canvas_t *c, int cur_visible)
{
    gterm_t *t = &st->term;
    int r, col;
    gfx_fill(c, (color_t)GT_DEFAULT_BG);
    for (r = 0; r < t->rows; r++) {
        const gterm_cell_t *row = gterm_view_row(t, r);
        for (col = 0; col < t->cols; col++) {
            const gterm_cell_t *cell = &row[col];
            char str[2] = { cell->ch ? cell->ch : ' ', '\0' };
            gfx_draw_text(c, col * GTERM_CELL_W, r * GTERM_CELL_H,
                          str, (color_t)cell->fg, (color_t)cell->bg);
        }
    }
    wg_vscroll_t sb = gterm_sb(t, c);
    wg_vscroll_draw(c, &sb, st->sb_drag);
    if (t->view > 0) return;              /* scrolled back: no cursor */
    if (cur_visible && t->cursor_col < t->cols) {
        gterm_cell_t *cc = &t->cells[t->cursor_row][t->cursor_col];
        rect_t cr = { t->cursor_col * GTERM_CELL_W,
                      t->cursor_row * GTERM_CELL_H,
                      GTERM_CELL_W, GTERM_CELL_H };
        char str[2] = { cc->ch ? cc->ch : ' ', '\0' };
        gfx_fill_rect(c, cr, (color_t)cc->fg);
        gfx_draw_text(c, cr.x, cr.y, str, (color_t)cc->bg, (color_t)cc->fg);
    }
}

/* Mouse on the scrollbar: arrows a line, the track a page, the thumb drags.
 * Returns 1 if the event was the scrollbar's. */
static int gterm_sb_mouse(gterm_state_t *st, canvas_t *c, const wm_event_t *ev)
{
    gterm_t *t = &st->term;
    wg_vscroll_t sb = gterm_sb(t, c);
    int page = t->rows > 1 ? t->rows - 1 : 1;
    switch (ev->type) {
    case WM_EV_MOUSE_BTN:
        if (ev->button != MOUSE_BTN_LEFT) return 0;
        switch (wg_vscroll_hit(&sb, ev->x, ev->y)) {
        case WG_SB_UP:   gterm_scroll_view(t, 1);     return 1;
        case WG_SB_DOWN: gterm_scroll_view(t, -1);    return 1;
        case WG_SB_PGUP: gterm_scroll_view(t, page);  return 1;
        case WG_SB_PGDN: gterm_scroll_view(t, -page); return 1;
        case WG_SB_THUMB:
            st->sb_drag = 1;
            st->sb_grab = ev->y - wg_vscroll_thumb(&sb).y;
            return 1;
        default:
            return wm_rect_contains(sb.r, ev->x, ev->y);
        }
    case WM_EV_MOUSE_MOVE:
        if (!st->sb_drag) return 0;
        t->view = t->hist_n - wg_vscroll_pos_at(&sb, ev->y - st->sb_grab);
        if (t->view < 0) t->view = 0;
        return 1;
    case WM_EV_MOUSE_UP:
        if (!st->sb_drag || ev->button != MOUSE_BTN_LEFT) return 0;
        st->sb_drag = 0;
        return 1;
    default:
        return 0;
    }
}

/* ── Callbacks ───────────────────────────────────────────────────────────── */

/* Non-blocking drain of the shell's output pipe; re-render on new data.
 * Called when the WM's wait_input() reports the pipe readable (wait_fd). */
#define GTERM_DRAIN_MAX  16     /* chunks per tick: bounded, never starves */
static void gterm_tick(window_t *win)
{
    gterm_state_t *st = (gterm_state_t *)win->app_state;
    if (st->exited) return;

    /* Drain what is there (up to a bound) and render once, rather than
     * re-rendering every 256 bytes.  Anything left keeps the pipe readable,
     * so the WM ticks us again straight away.  read_nonblock reports 0 both
     * for "empty" and for EOF; wait_input also wakes us at EOF (a hung-up
     * pipe counts as ready), which is how we learn the shell has exited. */
    char ibuf[256];
    int got = 0;
    for (int i = 0; i < GTERM_DRAIN_MAX; i++) {
        int n = read_nonblock(st->from_shell[0], ibuf, sizeof(ibuf));
        if (n <= 0) break;
        gterm_write(&st->term, ibuf, n);
        got = 1;
    }
    if (got) {
        gterm_render(st, win->backbuf, 1);
        win->dirty = 1;
        return;
    }

    /* Woken with nothing to read: the pipe is at EOF, i.e. our shell -- the
     * only owner of the write end -- has exited (`exit` at the terminal's
     * top-level prompt; a nested shell's `exit` just returns to its parent
     * and never closes the pipe).  Reap it and close the window.  The shell
     * closes its pipe ends a moment before it becomes a zombie; if we catch
     * it in between, wait_nohang() says "still running" and the hung-up
     * pipe wakes us again at once. */
    int code = 0;
    if (st->shell_pid > 0 && wait_nohang(st->shell_pid, &code) != 1) {
        st->shell_pid = -1;               /* reaped: nothing to queue */
        st->exited    = 1;
        if (!st->prog) { win->want_close = 1; return; }
        /* A program: keep its output on screen until a key is pressed. */
        static const char pre[]  = "\r\n[exited with code ";
        static const char post[] = " -- press a key to close]";
        char num[12];
        int  n = 0;
        unsigned u = code < 0 ? (unsigned)-code : (unsigned)code;
        do { num[n++] = (char)('0' + u % 10); u /= 10; } while (u);
        if (code < 0) num[n++] = '-';
        gterm_write(&st->term, pre, (int)sizeof pre - 1);
        while (n > 0) gterm_write(&st->term, &num[--n], 1);
        gterm_write(&st->term, post, (int)sizeof post - 1);
        st->term.view = 0;
        gterm_render(st, win->backbuf, 0);
        win->dirty = 1;
        win->wait_fd = -1;                /* nothing more to read */
    }
}

static void gterm_event(window_t *win, const wm_event_t *ev)
{
    gterm_state_t *st = (gterm_state_t *)win->app_state;
    if (ev->type == WM_EV_CLOSE) { win->want_close = 1; return; }
    if (ev->type == WM_EV_RESIZE) {           /* ROADMAP3 18.3 */
        gterm_fit(&st->term, ev->width, ev->height);
        gterm_render(st, win->backbuf, !st->exited);
        win->dirty = 1;
        return;
    }

    /* Scrollback (18.5): the scrollbar, the wheel, Shift-PgUp/PgDn,
     * Shift-Up/Down. */
    if (gterm_sb_mouse(st, win->backbuf, ev)) {
        gterm_render(st, win->backbuf, !st->exited);
        win->dirty = 1;
        return;
    }
    int back = 0;
    if (ev->type == WM_EV_SCROLL) {
        back = -ev->wheel * WM_WHEEL_ROWS;
    } else if (ev->type == WM_EV_KEY && (ev->mods & WM_MOD_SHIFT)) {
        int page = st->term.rows > 1 ? st->term.rows - 1 : 1;
        switch (ev->keycode) {
        case WM_KEY_PGUP: back = page;  break;
        case WM_KEY_PGDN: back = -page; break;
        case WM_KEY_UP:   back = 1;     break;
        case WM_KEY_DOWN: back = -1;    break;
        default: break;
        }
    }
    if (back) {
        if (gterm_scroll_view(&st->term, back)) {
            gterm_render(st, win->backbuf, !st->exited);
            win->dirty = 1;
        }
        return;
    }
    if (ev->type != WM_EV_KEY || ev->seq_len <= 0) return;

    /* A finished program's window closes on the next key. */
    if (st->exited) { win->want_close = 1; return; }

    /* Typing returns a scrolled-back view to the live screen. */
    if (st->term.view) {
        st->term.view = 0;
        gterm_render(st, win->backbuf, 1);
        win->dirty = 1;
    }

    /* Ctrl-C (ROADMAP3 17.0.1): the WM reads the keyboard on the shell's
     * behalf, so the kernel can't route it -- ask it to SIGINT the shell's
     * foreground job.  With no job running (shell at its prompt) the byte
     * goes to the shell as input, which abandons the current line. */
    if (ev->seq_len == 1 && ev->ascii == 0x03 && st->shell_pid > 0) {
        if (st->prog) {               /* no shell in between: signal it */
            kill(st->shell_pid, SIGINT);
            return;
        }
        if (intr(st->shell_pid) == 1) return;
    }
    /* The shell reads a terminal byte stream: forward exactly the bytes the
     * key arrived as (arrows, Alt-x, F-keys are escape sequences), not the
     * WM's decoded keycode.  ROADMAP3 18.2. */
    write(st->to_shell[1], ev->seq, (unsigned)ev->seq_len);
}

static void reap_queue_add(int pid);

static void gterm_destroy(window_t *win)
{
    gterm_state_t *st = (gterm_state_t *)win->app_state;
    if (st) {
        /* Close only OUR pipe ends.  to_shell[0] / from_shell[1] were handed
         * to the shell by exec_redir -- it owns them and the kernel closes
         * them when it exits.  Closing them here too was a double close of a
         * global fd slot that another process might already have reused.
         * Closing the write end gives the shell EOF on stdin, so it exits. */
        close(st->to_shell[1]);
        close(st->from_shell[0]);
        /* The WM loop must not block in wait(): queue the pid and let
         * gterm_reap_shells() collect it from the loop (REVIEW.md E). */
        if (st->shell_pid > 0) reap_queue_add(st->shell_pid);
        if (st->hist) free(st->hist);
        free(st);
        win->app_state = (void *)0;
    }
    if (win->backbuf) { canvas_free(win->backbuf); win->backbuf = (canvas_t *)0; }
}

/* ── Reaping closed terminals' shells (REVIEW.md E) ─────────────────────────
 * The shell exits shortly after its window closes (EOF on stdin).  The WM is
 * its parent and must reap it, or its process slot and memory are held until
 * the WM itself exits.  The loop calls gterm_reap_shells() every iteration;
 * it never blocks. */
#define REAP_MAX 16
static int reap_pids[REAP_MAX];
static int reap_count;

static void reap_queue_add(int pid)
{
    if (reap_count < REAP_MAX) reap_pids[reap_count++] = pid;
}

int gterm_reap_pending(void) { return reap_count > 0; }

void gterm_reap_shells(void)
{
    for (int i = 0; i < reap_count; ) {
        if (wait_nohang(reap_pids[i], (int *)0) != 1)  /* reaped, or gone */
            reap_pids[i] = reap_pids[--reap_count];
        else
            i++;
    }
}

/* ── Launcher entry points ───────────────────────────────────────────────── */

/* Start `path` with its stdin/stdout on this window's pipes.  prog = 0: it is
 * a shell, and its exit closes the window. */
static void gterm_start(window_t *win, const char *path, int prog)
{
    gterm_state_t *st = (gterm_state_t *)malloc(sizeof(gterm_state_t));
    if (!st) { win->want_close = 1; return; }
    st->shell_pid = -1;
    st->exited    = 0;
    st->prog      = prog;

    if (pipe(st->to_shell) || pipe(st->from_shell)) {
        free(st);
        win->want_close = 1;
        return;
    }

    /* Spawn the child with its stdin = to_shell[0], stdout = from_shell[1],
     * using exec_redir so the WM's OWN stdin/stdout are untouched -- the WM
     * loop keeps reading the real keyboard via read_nonblock(STDIN). */
    st->shell_pid = exec_redir(path, st->to_shell[0], st->from_shell[1]);

    if (st->shell_pid < 0) {
        close(st->to_shell[0]); close(st->to_shell[1]);
        close(st->from_shell[0]); close(st->from_shell[1]);
        free(st);
        win->want_close = 1;
        return;
    }

    gterm_init(&st->term);
    /* Scrollback is a nicety: without the memory, a terminal still works. */
    st->hist = (gterm_cell_t (*)[GTERM_MAX_COLS])
               malloc(sizeof(gterm_cell_t) * GTERM_MAX_COLS * GTERM_HIST_LINES);
    gterm_set_history(&st->term, st->hist, st->hist ? GTERM_HIST_LINES : 0);
    st->sb_drag = 0;
    gterm_fit(&st->term, win->backbuf->w, win->backbuf->h);

    win->app_state  = st;
    win->on_tick    = gterm_tick;
    win->wait_fd    = st->from_shell[0];   /* tick when the child writes */
    win->on_event   = gterm_event;
    win->on_destroy = gterm_destroy;

    gterm_render(st, win->backbuf, 1);
    win->dirty = 1;
}

void gterm_open(window_t *win) { gterm_start(win, "/shell.elf", 0); }

/* Run one program in the window (Files opening an .elf, ROADMAP3 18.5). */
void gterm_open_prog(window_t *win, const char *path) { gterm_start(win, path, 1); }
