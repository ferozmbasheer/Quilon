/*
 * Quilon OS -- Graphical Terminal as a WM app (section 14.3)
 *
 * gterm_app() runs as a clone(CLONE_VM) thread of the WM.  It must NOT call
 * fork() — fork() inside a CLONE_VM thread triggers paging_fork_address_space()
 * which marks every writable shared page as CoW, silently diverging the WM and
 * gterm threads' views of the canvas and window state.
 *
 * Instead, we use exec() directly.  Quilon's SYS_EXEC creates a child process
 * and returns its PID; the child inherits the caller's stdin_fd/stdout_fd
 * overrides AND the fd table, so the pipe fds are accessible in the shell.
 *
 * Thread structure:
 *   gterm_app thread  — sets up pipes, execs shell, reads shell stdout,
 *                       renders cell grid into win->backbuf, sets win->dirty
 *   kbd_fwd thread    — polls win->events (WM pushes key events here),
 *                       writes each keystroke to the shell's stdin pipe
 */

#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <gfx.h>
#include "gterm.h"
#include "../wm/wm.h"

/* ── Keyboard-forwarding thread ─────────────────────────────────────────── */

typedef struct { wm_evqueue_t *ev; int shell_in; } kbd_arg_t;

static void *kbd_fwd_thread(void *arg)
{
    kbd_arg_t *a = (kbd_arg_t *)arg;
    wm_event_t ev;
    while (1) {
        if (wm_evqueue_poll(a->ev, &ev)) {
            if (ev.type == WM_EV_CLOSE)
                break;
            if (ev.type == WM_EV_KEY && ev.ascii)
                write(a->shell_in, &ev.ascii, 1);
        }
    }
    return NULL;
}

/* ── Renderer ────────────────────────────────────────────────────────────── */

static void gterm_render(gterm_t *t, canvas_t *c, int cur_visible)
{
    int r, col;
    gfx_fill(c, (color_t)GT_DEFAULT_BG);
    for (r = 0; r < GTERM_ROWS; r++) {
        for (col = 0; col < GTERM_COLS; col++) {
            gterm_cell_t *cell = &t->cells[r][col];
            char str[2] = { cell->ch ? cell->ch : ' ', '\0' };
            gfx_draw_text(c,
                          col * GTERM_CELL_W,
                          r   * GTERM_CELL_H,
                          str,
                          (color_t)cell->fg,
                          (color_t)cell->bg);
        }
    }
    if (cur_visible) {
        gterm_cell_t *cc = &t->cells[t->cursor_row][t->cursor_col];
        rect_t cr = {
            t->cursor_col * GTERM_CELL_W,
            t->cursor_row * GTERM_CELL_H,
            GTERM_CELL_W,
            GTERM_CELL_H
        };
        char str[2] = { cc->ch ? cc->ch : ' ', '\0' };
        gfx_fill_rect(c, cr, (color_t)cc->fg);
        gfx_draw_text(c, cr.x, cr.y, str, (color_t)cc->bg, (color_t)cc->fg);
    }
}

/* ── App entry point ─────────────────────────────────────────────────────── */

void gterm_app(window_t *win)
{
    /*
     * 1. Create two pipes:
     *      to_shell[0]   = shell reads  (its stdin)
     *      to_shell[1]   = we write     (keys → shell)
     *      from_shell[0] = we read      (shell output → render)
     *      from_shell[1] = shell writes (its stdout/stderr)
     */
    int to_shell[2], from_shell[2];
    if (pipe(to_shell) || pipe(from_shell)) {
        win->active = 0;
        return;
    }

    /*
     * 2. Point THIS process's stdin/stdout at the pipe ends so that the
     *    exec'd shell inherits them.  exec() in Quilon inherits both
     *    the stdin_fd/stdout_fd fields AND the fd table from the caller,
     *    so the shell can access these fds immediately.
     *
     *    This does NOT call fork(), so no CoW is triggered on the shared
     *    WM address space.
     */
    dup2(to_shell[0],   STDIN_FILENO);
    dup2(from_shell[1], STDOUT_FILENO);
    dup2(from_shell[1], STDERR_FILENO);

    int shell_pid = exec("/shell.elf");
    if (shell_pid < 0) {
        win->active = 0;
        return;
    }

    /*
     * 3. Do NOT close to_shell[0] or from_shell[1] here.
     *
     *    Quilon uses a single global fd table shared by all processes.
     *    Closing these fds now would mark their table entries as in_use=0,
     *    making the shell's inherited stdin_fd/stdout_fd immediately invalid.
     *    The shell owns these ends for its lifetime; we leave them open.
     */

    /*
     * 4. Keyboard-forwarding thread: polls win->events, writes to shell stdin.
     *    This is another CLONE_VM thread (via pthread_create), so it shares
     *    our address space and can access win->events directly.
     */
    kbd_arg_t karg = { &win->events, to_shell[1] };
    pthread_t kbd_tid;
    pthread_create(&kbd_tid, NULL, kbd_fwd_thread, &karg);

    /*
     * 5. Render the initial blank terminal, then loop on shell output.
     */
    gterm_t term;
    gterm_init(&term);
    gterm_render(&term, win->backbuf, 1);
    win->dirty = 1;

    char ibuf[256];
    while (1) {
        int n = read(from_shell[0], ibuf, sizeof(ibuf));
        if (n > 0) {
            gterm_write(&term, ibuf, n);
            gterm_render(&term, win->backbuf, 1);
            win->dirty = 1;
        } else if (n == 0) {
            /* Shell closed its stdout — display notice and stop. */
            gterm_write(&term, "\r\n[shell exited]\r\n", 18);
            gterm_render(&term, win->backbuf, 0);
            win->dirty = 1;
            break;
        }
        /* n < 0: transient error — retry */
    }

    close(to_shell[1]);
    close(from_shell[0]);
    win->active = 0;
}
