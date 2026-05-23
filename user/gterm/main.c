/*
 * Quilon OS -- Graphical Terminal emulator (section 14.3)
 *
 * Standalone: maps the framebuffer directly via gfx_screen_init(); no WM.
 *
 * Architecture:
 *   main thread  — reads shell stdout, updates cell grid, re-renders.
 *   kbd thread   — reads keyboard (fd 0) and forwards each byte to the shell.
 *
 * Layout:
 *   Row 0..TITLEBAR_H-1  : title bar
 *   Row TITLEBAR_H..     : GTERM_COLS × GTERM_ROWS cell grid
 */

#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <gfx.h>
#include "gterm.h"

#define TITLEBAR_H  16   /* pixels — one character-cell tall */

/* Written once before the keyboard thread starts; read-only thereafter. */
static int g_shell_in = -1;

static void *keyboard_thread(void *arg)
{
    (void)arg;
    char c;
    while (read(0, &c, 1) == 1)
        write(g_shell_in, &c, 1);
    return 0;
}

/* ── Render the full cell grid into *screen at y offset y0. ─────────────── */

static void gterm_render(gterm_t *t, canvas_t *screen, int y0, int cur_visible)
{
    int r, c;

    /* Background fill for the terminal area only. */
    rect_t bg = { 0, y0, GTERM_PX_W, GTERM_PX_H };
    gfx_fill_rect(screen, bg, (color_t)GT_DEFAULT_BG);

    for (r = 0; r < GTERM_ROWS; r++) {
        for (c = 0; c < GTERM_COLS; c++) {
            gterm_cell_t *cell = &t->cells[r][c];
            char str[2] = { cell->ch ? cell->ch : ' ', '\0' };
            gfx_draw_text(screen,
                          c * GTERM_CELL_W,
                          y0 + r * GTERM_CELL_H,
                          str,
                          (color_t)cell->fg,
                          (color_t)cell->bg);
        }
    }

    if (cur_visible) {
        gterm_cell_t *cc = &t->cells[t->cursor_row][t->cursor_col];
        rect_t cr = {
            t->cursor_col * GTERM_CELL_W,
            y0 + t->cursor_row * GTERM_CELL_H,
            GTERM_CELL_W,
            GTERM_CELL_H
        };
        char str[2] = { cc->ch ? cc->ch : ' ', '\0' };
        gfx_fill_rect(screen, cr, (color_t)cc->fg);
        gfx_draw_text(screen,
                      t->cursor_col * GTERM_CELL_W,
                      y0 + t->cursor_row * GTERM_CELL_H,
                      str,
                      (color_t)cc->bg,
                      (color_t)cc->fg);
    }
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    /* 1. Map the hardware shadow buffer. */
    gfx_info_t info;
    canvas_t *screen = gfx_screen_init(&info);
    if (!screen) {
        printf("[gterm] ERROR: gfx_screen_init failed\r\n");
        return 1;
    }

    /* 2. Draw title bar. */
    rect_t tbar = { 0, 0, (int)info.width, TITLEBAR_H };
    gfx_fill_rect(screen, tbar, GFX_DARK_BLUE);
    gfx_draw_text(screen, 4, 0, "Terminal", GFX_WHITE, GFX_DARK_BLUE);
    gfx_flush();

    /* 3. Create pipes: gterm→shell (to_shell) and shell→gterm (from_shell). */
    int to_shell[2], from_shell[2];
    if (pipe(to_shell) != 0 || pipe(from_shell) != 0) {
        printf("[gterm] ERROR: pipe() failed\r\n");
        return 1;
    }

    g_shell_in = to_shell[1];

    /* 4. Fork.  The child redirects its stdio and exec()s the shell. */
    int child_pid = fork();
    if (child_pid < 0) {
        printf("[gterm] ERROR: fork() failed\r\n");
        return 1;
    }

    if (child_pid == 0) {
        /* ── Child ──────────────────────────────────────────────────────── */
        dup2(to_shell[0],   STDIN_FILENO);
        dup2(from_shell[1], STDOUT_FILENO);
        dup2(from_shell[1], STDERR_FILENO);
        close(to_shell[0]);   close(to_shell[1]);
        close(from_shell[0]); close(from_shell[1]);

        int shell_pid = exec("/shell.elf");
        if (shell_pid < 0) {
            printf("[gterm] ERROR: exec(/shell.elf) failed\r\n");
            exit(1);
        }
        wait(shell_pid, (int *)0);
        exit(0);
    }

    /* ── Parent (gterm) ─────────────────────────────────────────────────── */
    close(to_shell[0]);
    close(from_shell[1]);

    /* 5. Start keyboard-forwarding thread. */
    pthread_t kbd_tid;
    pthread_create(&kbd_tid, NULL, keyboard_thread, NULL);

    /* 6. Initialise terminal state and do an initial render. */
    gterm_t term;
    gterm_init(&term);
    gterm_render(&term, screen, TITLEBAR_H, 1);
    gfx_flush();

    /* 7. Event loop: read shell output → parse → render → flush. */
    char ibuf[256];
    while (1) {
        int n = read(from_shell[0], ibuf, sizeof(ibuf));
        if (n > 0) {
            gterm_write(&term, ibuf, n);
            gterm_render(&term, screen, TITLEBAR_H, 1);
            gfx_flush();
        } else if (n == 0) {
            /* Shell exited. */
            gterm_write(&term, "\r\n[shell exited]\r\n", 18);
            gterm_render(&term, screen, TITLEBAR_H, 0);
            gfx_flush();
            close(from_shell[0]);
            break;
        }
        /* n < 0: transient error — keep trying. */
    }

    close(to_shell[1]);
    return 0;
}
