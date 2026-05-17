/*
 * Quilon OS -- WM Client Library implementation (section 14.2)
 */

#include "wm_client.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static wm_win_t g_wins[WM_CLIENT_MAX_WINS];
static int g_init = 0;

static void client_init(void)
{
    if (g_init) return;
    int i;
    for (i = 0; i < WM_CLIENT_MAX_WINS; i++) {
        g_wins[i].win_id  = -1;
        g_wins[i].cmd_fd  = -1;
        g_wins[i].evt_fd  = -1;
        g_wins[i].backbuf = (canvas_t *)0;
    }
    g_init = 1;
}

static int alloc_slot(void)
{
    int i;
    for (i = 0; i < WM_CLIENT_MAX_WINS; i++) {
        if (g_wins[i].win_id < 0) return i;
    }
    return -1;
}

int wm_client_open_window(const char *title, int x, int y, int w, int h)
{
    client_init();

    int slot = alloc_slot();
    if (slot < 0) {
        printf("[wm_client] no free client slots\r\n");
        return -1;
    }

    /* Open the WM command pipe. */
    int cmd_fd = open(WM_CMD_PATH);
    if (cmd_fd < 0) {
        printf("[wm_client] cannot open WM command pipe '%s'\r\n", WM_CMD_PATH);
        return -1;
    }

    /* Build the CREATE message. */
    wm_msg_t msg;
    msg.type = WM_MSG_CREATE;
    msg.id   = 0;  /* WM assigns the real id */
    msg.x = x; msg.y = y; msg.w = w; msg.h = h;

    int i;
    for (i = 0; i < WM_TITLE_LEN - 1 && title && title[i]; i++)
        msg.title[i] = title[i];
    msg.title[i] = '\0';

    if (write(cmd_fd, &msg, sizeof(msg)) != (int)sizeof(msg)) {
        printf("[wm_client] failed to send CREATE message\r\n");
        close(cmd_fd);
        return -1;
    }

    /* Allocate a local back-buffer. */
    int bw = w < WM_MIN_W ? WM_MIN_W : w;
    int bh = h < WM_MIN_H ? WM_MIN_H : h;
    canvas_t *buf = canvas_create(bw, bh);
    if (!buf) {
        printf("[wm_client] canvas_create(%d,%d) failed\r\n", bw, bh);
        close(cmd_fd);
        return -1;
    }
    gfx_fill(buf, GFX_RGB(30, 30, 30));

    g_wins[slot].win_id  = slot + 1;  /* placeholder — WM assigns real ID */
    g_wins[slot].cmd_fd  = cmd_fd;
    g_wins[slot].evt_fd  = -1;
    g_wins[slot].backbuf = buf;

    return slot;
}

canvas_t *wm_client_get_backbuf(int handle)
{
    if (handle < 0 || handle >= WM_CLIENT_MAX_WINS) return (canvas_t *)0;
    return g_wins[handle].backbuf;
}

void wm_client_mark_dirty(int handle)
{
    if (handle < 0 || handle >= WM_CLIENT_MAX_WINS) return;
    wm_win_t *win = &g_wins[handle];
    if (win->cmd_fd < 0) return;

    wm_msg_t msg;
    msg.type = WM_MSG_DIRTY;
    msg.id   = win->win_id;
    msg.x = msg.y = msg.w = msg.h = 0;
    msg.title[0] = '\0';
    write(win->cmd_fd, &msg, sizeof(msg));
}

int wm_client_poll_event(int handle, wm_event_t *ev)
{
    if (handle < 0 || handle >= WM_CLIENT_MAX_WINS || !ev) return 0;
    wm_win_t *win = &g_wins[handle];
    if (win->evt_fd < 0) return 0;
    return read(win->evt_fd, ev, sizeof(*ev)) == (int)sizeof(*ev) ? 1 : 0;
}

void wm_client_close_window(int handle)
{
    if (handle < 0 || handle >= WM_CLIENT_MAX_WINS) return;
    wm_win_t *win = &g_wins[handle];

    if (win->cmd_fd >= 0) {
        wm_msg_t msg;
        msg.type = WM_MSG_DESTROY;
        msg.id   = win->win_id;
        msg.x = msg.y = msg.w = msg.h = 0;
        msg.title[0] = '\0';
        write(win->cmd_fd, &msg, sizeof(msg));
        close(win->cmd_fd);
    }
    if (win->evt_fd >= 0) close(win->evt_fd);
    if (win->backbuf)     canvas_free(win->backbuf);

    win->win_id  = -1;
    win->cmd_fd  = -1;
    win->evt_fd  = -1;
    win->backbuf = (canvas_t *)0;
}
