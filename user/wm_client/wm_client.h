/*
 * Quilon OS -- WM Client Library (section 14.2)
 *
 * Thin IPC layer for apps that want to create windows managed by the WM.
 *
 * Typical app lifecycle:
 *
 *   int win  = wm_client_open_window("My App", x, y, w, h);
 *   canvas_t *buf = wm_client_get_backbuf(win);
 *   // draw into buf ...
 *   wm_client_mark_dirty(win);
 *   wm_event_t ev;
 *   while (wm_client_poll_event(win, &ev)) { handle(ev); }
 *   wm_client_close_window(win);
 *
 * IPC mechanism
 * ─────────────
 * The WM exposes a command pipe at WM_CMD_PATH.  The client writes wm_msg_t
 * structs to request window create/destroy/dirty operations.  The WM writes
 * wm_event_t structs to per-window event pipes (WM_EVT_PATH_<id>).
 *
 * Back-buffer sharing
 * ───────────────────
 * Currently the client allocates its own malloc-backed canvas and calls
 * wm_client_push_pixels() to write pixel data to the WM via the event pipe.
 * Future versions will use a shared-memory mapping once SYS_SHM is added.
 */

#ifndef _USER_WM_CLIENT_WM_CLIENT_H
#define _USER_WM_CLIENT_WM_CLIENT_H

#include <stdint.h>
#include <gfx.h>
#include "../wm/wm.h"

/* -- Client state -------------------------------------------------------- */

#define WM_CLIENT_MAX_WINS 4   /* max windows per client process */

typedef struct {
    int       win_id;          /* WM-assigned window ID, or -1  */
    int       cmd_fd;          /* fd open on WM_CMD_PATH         */
    int       evt_fd;          /* fd open on per-window evt pipe */
    canvas_t *backbuf;         /* local draw buffer              */
} wm_win_t;

/* -- API ----------------------------------------------------------------- */

/*
 * wm_client_open_window -- register a window with the WM.
 *
 * Opens the WM command pipe, sends WM_MSG_CREATE, and returns a client
 * window handle (index into an internal table).  Returns -1 on failure.
 */
int wm_client_open_window(const char *title, int x, int y, int w, int h);

/*
 * wm_client_get_backbuf -- return the malloc-backed canvas for this window.
 * Draw your content here; then call wm_client_mark_dirty().
 */
canvas_t *wm_client_get_backbuf(int handle);

/*
 * wm_client_mark_dirty -- tell the WM to re-composite this window.
 */
void wm_client_mark_dirty(int handle);

/*
 * wm_client_poll_event -- non-blocking read of the next WM event.
 * Returns 1 if an event was read into *ev, 0 if the queue is empty.
 */
int wm_client_poll_event(int handle, wm_event_t *ev);

/*
 * wm_client_close_window -- send WM_MSG_DESTROY and free local resources.
 */
void wm_client_close_window(int handle);

#endif /* _USER_WM_CLIENT_WM_CLIENT_H */
