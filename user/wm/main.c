/*
 * Quilon OS -- Window Manager & Compositor process (section 14.2)
 *
 * This is the WM process (wm.elf).  It:
 *   1. Maps the kernel shadow framebuffer via SYS_GFX_MAP.
 *   2. Maintains the window list (wm_state_t).
 *   3. Reads mouse events via SYS_MOUSE_READ and keyboard via SYS_READ(stdin).
 *   4. On each event, re-composites all windows and calls gfx_flush().
 *
 * IPC with apps
 * ─────────────
 * Apps write wm_msg_t structs to the WM command pipe (/wm_cmd).
 * The WM creates per-window event pipes (/wm_evt_<id>) and writes
 * wm_event_t structs into the focused window's event pipe on each keypress.
 *
 * Compositor implementation is in wm_compositor.c (linked separately so
 * the test suite can exercise it without pulling in this main()).
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gfx.h>
#include "wm.h"

/* mouse_read() is declared in <unistd.h> as:
 *   int mouse_read(mouse_event_t *out);
 * We use it directly — no local stub needed.                            */

/* -- Demo window population -------------------------------------------- */

static void wm_populate_demo(wm_state_t *s)
{
    int id;

    id = wm_create_window(s, "About Quilon", 60, 80, 240, 120);
    if (id > 0) {
        window_t *w = wm_find_window(s, id);
        if (w) {
            w->backbuf = canvas_create(240, 120);
            if (w->backbuf) {
                gfx_fill(w->backbuf, GFX_RGB(20, 20, 40));
                gfx_draw_text(w->backbuf,  8,  8, "Quilon OS",
                              GFX_WHITE, GFX_RGB(20,20,40));
                gfx_draw_text(w->backbuf,  8, 28, "Window Manager 14.2",
                              GFX_RGB(180,180,255), GFX_RGB(20,20,40));
                gfx_draw_text(w->backbuf,  8, 48, "Painter algorithm",
                              GFX_RGB(100,200,100), GFX_RGB(20,20,40));
                gfx_draw_text(w->backbuf,  8, 68, "Back-to-front composite",
                              GFX_RGB(200,200,100), GFX_RGB(20,20,40));
            }
        }
    }

    id = wm_create_window(s, "Terminal", 320, 80, 280, 180);
    if (id > 0) {
        window_t *w = wm_find_window(s, id);
        if (w) {
            w->backbuf = canvas_create(280, 180);
            if (w->backbuf) {
                gfx_fill(w->backbuf, GFX_BLACK);
                gfx_draw_text(w->backbuf, 4,  4, "quilon> _",
                              GFX_GREEN, GFX_BLACK);
            }
        }
    }

    id = wm_create_window(s, "Clock", 160, 340, 160, 40);
    if (id > 0) {
        window_t *w = wm_find_window(s, id);
        if (w) {
            w->backbuf = canvas_create(160, 40);
            if (w->backbuf) {
                gfx_fill(w->backbuf, GFX_RGB(10, 10, 10));
                gfx_draw_text(w->backbuf, 40, 12, "00:00:00",
                              GFX_RGB(0, 255, 128), GFX_RGB(10,10,10));
            }
            w->focused = 1;
        }
    }
}

static void wm_free_demo(wm_state_t *s)
{
    int i;
    for (i = 0; i < s->num_windows; i++) {
        if (s->windows[i].backbuf) {
            canvas_free(s->windows[i].backbuf);
            s->windows[i].backbuf = (canvas_t *)0;
        }
    }
}

/* -- WM command-pipe processing ---------------------------------------- */

static void wm_process_msg(wm_state_t *s, const wm_msg_t *m)
{
    switch (m->type) {
    case WM_MSG_CREATE: {
        int id = wm_create_window(s, m->title, m->x, m->y, m->w, m->h);
        if (id > 0) {
            window_t *w = wm_find_window(s, id);
            if (w) {
                w->backbuf = canvas_create(w->bounds.w, w->bounds.h);
                if (w->backbuf) gfx_fill(w->backbuf, GFX_RGB(30,30,30));
            }
        }
        break;
    }
    case WM_MSG_DESTROY:
        wm_destroy_window(s, m->id);
        break;
    case WM_MSG_DIRTY: {
        window_t *w = wm_find_window(s, m->id);
        if (w) w->dirty = 1;
        break;
    }
    default:
        break;
    }
}

/* -- Main event loop ---------------------------------------------------- */

int main(void)
{
    printf("[wm] Quilon Window Manager starting (section 14.2)\r\n");

    gfx_info_t info;
    canvas_t *screen = gfx_screen_init(&info);
    if (!screen) {
        printf("[wm] ERROR: could not map framebuffer (VBE not active?)\r\n");
        return 1;
    }
    printf("[wm] screen %ux%u bpp=%u\r\n",
           (unsigned)info.width, (unsigned)info.height, (unsigned)info.bpp);

    wm_state_t state;
    wm_state_init(&state, (int)info.width, (int)info.height);

    wm_populate_demo(&state);
    wm_composite(&state, screen);
    gfx_flush();
    printf("[wm] initial frame: %d windows composited\r\n", state.num_windows);

    int cmd_fd = open(WM_CMD_PATH);

    while (1) {
        int dirty = 0;

        /* Mouse. */
        mouse_event_t me;
        if (mouse_read(&me) > 0) {
            uint8_t prev = state.mouse_buttons;
            if (me.buttons && !prev)
                wm_handle_mouse_down(&state, me.x, me.y, me.buttons);
            else if (!me.buttons && prev)
                wm_handle_mouse_up(&state);
            else
                wm_handle_mouse_move(&state, me.x, me.y);
            dirty = 1;
        }

        /* Keyboard → forward to focused window. */
        char ch;
        if (read(0, &ch, 1) == 1) {
            int i;
            for (i = state.num_windows - 1; i >= 0; i--) {
                window_t *w = &state.windows[i];
                if (w->focused && w->evt_fd >= 0) {
                    wm_event_t ev;
                    ev.type  = WM_EV_KEY;
                    ev.ascii = ch;
                    ev.x = ev.y = 0;
                    ev.buttons  = 0;
                    write(w->evt_fd, &ev, sizeof(ev));
                    break;
                }
            }
            dirty = 1;
        }

        /* App command pipe. */
        if (cmd_fd >= 0) {
            wm_msg_t msg;
            if (read(cmd_fd, &msg, sizeof(msg)) == (int)sizeof(msg)) {
                wm_process_msg(&state, &msg);
                dirty = 1;
            }
        }

        if (dirty) {
            wm_composite(&state, screen);
            gfx_flush();
        }
    }

    wm_free_demo(&state);
    return 0;
}
