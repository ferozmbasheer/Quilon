/*
 * Quilon OS -- Power-off dialog (desktop Power button)
 *
 * A small confirmation window opened by the taskbar's Power button:
 *
 *     Switch the computer off?
 *     [ Power off ]  [  Cancel  ]
 *
 * "Power off" (or y / Enter) asks the WM to call poweroff() (which never
 * returns) once the "Powering off..." frame has been flushed.
 * "Cancel" (or n / Esc, or the close box) just closes the dialog.  Like every app
 * it is a set of non-blocking callbacks on the WM loop.
 */

#include <unistd.h>
#include <gfx.h>
#include "wm.h"

#define DLG_BG    WM_FRAME_BASE
#define DLG_TEXT  GFX_BLACK

static void power_render(window_t *win, const char *msg, int buttons)
{
    canvas_t *c = win->backbuf;
    int cw = win->bounds.w, ch = win->bounds.h;
    gfx_fill(c, DLG_BG);
    gfx_draw_text(c, 16, 18, msg, DLG_TEXT, DLG_BG);
    if (buttons) {
        wm_draw_button(c, wm_dialog_btn_rect(WM_DLG_BTN_OK, cw, ch), "Power off");
        wm_draw_button(c, wm_dialog_btn_rect(WM_DLG_BTN_CANCEL, cw, ch), "Cancel");
    }
    win->dirty = 1;
}

static void power_now(window_t *win)
{
    /* The WM calls poweroff() after this frame is on screen, so the user
     * sees the message rather than a frozen dialog. */
    power_render(win, "Powering off...", 0);
    win->on_event = (wm_on_event_fn)0;    /* no second click */
    wm_request_poweroff();
}

static void power_event(window_t *win, const wm_event_t *ev)
{
    switch (ev->type) {
    case WM_EV_CLOSE:
        win->want_close = 1;
        break;
    case WM_EV_KEY:
        if (ev->keycode == 'y' || ev->keycode == WM_KEY_ENTER)
            power_now(win);
        else if (ev->keycode == 'n' || ev->keycode == WM_KEY_ESC)
            win->want_close = 1;
        break;
    case WM_EV_MOUSE_BTN:
        if (ev->button != MOUSE_BTN_LEFT) break;
        switch (wm_dialog_btn_hit(ev->x, ev->y, win->bounds.w, win->bounds.h)) {
        case WM_DLG_BTN_OK:     power_now(win);       break;
        case WM_DLG_BTN_CANCEL: win->want_close = 1;  break;
        default: break;
        }
        break;
    default:
        break;
    }
}

static void power_destroy(window_t *win)
{
    if (win->backbuf) { canvas_free(win->backbuf); win->backbuf = (canvas_t *)0; }
}

void power_dialog_open(window_t *win)
{
    win->on_event   = power_event;
    win->on_destroy = power_destroy;
    power_render(win, "Switch the computer off?", 1);
}
