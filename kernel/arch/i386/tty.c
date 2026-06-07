#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/tty.h>
#include <kernel/vbe.h>
#include <kernel/ansi.h>
#include <kernel/spinlock.h>

#include "vga.h"

extern void outb(unsigned short port, unsigned char data);
extern char inb(unsigned short port);

static uint16_t* const VGA_MEMORY = (uint16_t*) 0xB8000;

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* terminal_buffer;

void enable_cursor(uint8_t cursor_start, uint8_t cursor_end)
{
	outb(0x3D4, 0x0A);
	outb(0x3D5, (inb(0x3D5) & 0xC0) | cursor_start);

	outb(0x3D4, 0x0B);
	outb(0x3D5, (inb(0x3D5) & 0xE0) | cursor_end);
}

void update_cursor(int x, int y)
{
	uint16_t pos = (uint16_t)(y * VGA_WIDTH + x);

	outb(0x3D4, 0x0F);
	outb(0x3D5, (uint8_t) (pos & 0xFF));
	outb(0x3D4, 0x0E);
	outb(0x3D5, (uint8_t) ((pos >> 8) & 0xFF));
}

void terminal_initialize(void) {
	terminal_row = 0;
	terminal_column = 0;
	terminal_color = vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
	terminal_buffer = VGA_MEMORY;
	for (size_t y = 0; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			const size_t index = y * VGA_WIDTH + x;
			terminal_buffer[index] = vga_entry(' ', terminal_color);
		}
	}

	enable_cursor(0, 15);
	update_cursor(0, 0);
}

void terminal_setcolor(uint8_t color) {
	terminal_color = color;
}

void terminal_putentryat(unsigned char c, uint8_t color, size_t x, size_t y) {
	const size_t index = y * VGA_WIDTH + x;
	terminal_buffer[index] = vga_entry(c, color);
}

void terminal_scrollline() {
	for (size_t y = 3; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			const size_t index_read = y * VGA_WIDTH + x;
			const size_t index_write = (y-1) * VGA_WIDTH + x;
			terminal_buffer[index_write] = terminal_buffer[index_read];
		}
	}
	for (size_t x = 0; x < VGA_WIDTH; x++) {
		const size_t index = (VGA_HEIGHT-1) * VGA_WIDTH + x;
		terminal_buffer[index] = vga_entry(' ', terminal_color);
	}
}

void terminal_handlenewline() {
	if (++terminal_row == VGA_HEIGHT) {
		terminal_scrollline();
		terminal_row = VGA_HEIGHT - 1;
	}
}

/* -- ANSI state ----------------------------------------------------------- */

static ansi_parser_t ansi_p;  /* zero-initialized: state=NORMAL, len=0    */

/*
 * Current SGR colors (VBE path only -- VGA colors are managed separately
 * through terminal_color / terminal_setcolor).
 *
 * Defaults match what vbe_terminal_init() sets:
 *   fg = VBE_COLOR_LIGHT_GREY (0x00AAAAAA)
 *   bg = VBE_COLOR_DARK_BLUE  (0x00000088)
 */
static uint32_t ansi_fg   = 0x00AAAAAAu;
static uint32_t ansi_bg   = 0x00000088u;
static int      ansi_bold = 0;

/* Saved cursor position for ESC[s / ESC[u. */
static uint32_t ansi_saved_row = 0;
static uint32_t ansi_saved_col = 0;

/* -- ansi_execute --------------------------------------------------------- */
/*
 * Dispatch a parsed CSI command to the VBE or VGA backend.
 *
 * Cursor movement, clear-screen, and erase-line work on both backends.
 * SGR color changes work only on VBE (VGA text mode manages its own color).
 */
static void ansi_execute(const ansi_event_t *ev)
{
    int p0 = (ev->nparams > 0) ? ev->params[0] : 0;
    int p1 = (ev->nparams > 1) ? ev->params[1] : 0;

    switch (ev->cmd) {

    /* -- Cursor position: ESC [ row ; col H  (1-indexed, 0 = 1) ----------- */
    case 'H':
    case 'f': {
        uint32_t row = (p0 > 0) ? (uint32_t)(p0 - 1) : 0u;
        uint32_t col = (p1 > 0) ? (uint32_t)(p1 - 1) : 0u;
        if (vbe_active()) {
            vbe_terminal_set_cursor(row, col);
        } else {
            terminal_row    = (row < VGA_HEIGHT) ? row : VGA_HEIGHT - 1u;
            terminal_column = (col < VGA_WIDTH)  ? col : VGA_WIDTH  - 1u;
            update_cursor((int)terminal_column, (int)terminal_row);
        }
        break;
    }

    /* -- Cursor up: ESC [ n A ----------------------------------------------- */
    case 'A': {
        int n = (p0 > 0) ? p0 : 1;
        if (vbe_active()) {
            uint32_t row, col;
            vbe_terminal_get_cursor(&row, &col);
            row = ((uint32_t)n > row) ? 0u : row - (uint32_t)n;
            vbe_terminal_set_cursor(row, col);
        } else {
            terminal_row = ((size_t)n > terminal_row) ? 0u : terminal_row - (size_t)n;
            update_cursor((int)terminal_column, (int)terminal_row);
        }
        break;
    }

    /* -- Cursor down: ESC [ n B -------------------------------------------- */
    case 'B': {
        int n = (p0 > 0) ? p0 : 1;
        if (vbe_active()) {
            uint32_t row, col;
            vbe_terminal_get_cursor(&row, &col);
            vbe_terminal_set_cursor(row + (uint32_t)n, col);
        } else {
            size_t nr = terminal_row + (size_t)n;
            terminal_row = (nr < VGA_HEIGHT) ? nr : VGA_HEIGHT - 1u;
            update_cursor((int)terminal_column, (int)terminal_row);
        }
        break;
    }

    /* -- Cursor right: ESC [ n C ------------------------------------------- */
    case 'C': {
        int n = (p0 > 0) ? p0 : 1;
        if (vbe_active()) {
            uint32_t row, col;
            vbe_terminal_get_cursor(&row, &col);
            vbe_terminal_set_cursor(row, col + (uint32_t)n);
        } else {
            size_t nc = terminal_column + (size_t)n;
            terminal_column = (nc < VGA_WIDTH) ? nc : VGA_WIDTH - 1u;
            update_cursor((int)terminal_column, (int)terminal_row);
        }
        break;
    }

    /* -- Cursor left: ESC [ n D -------------------------------------------- */
    case 'D': {
        int n = (p0 > 0) ? p0 : 1;
        if (vbe_active()) {
            uint32_t row, col;
            vbe_terminal_get_cursor(&row, &col);
            col = ((uint32_t)n > col) ? 0u : col - (uint32_t)n;
            vbe_terminal_set_cursor(row, col);
        } else {
            terminal_column = ((size_t)n > terminal_column) ? 0u : terminal_column - (size_t)n;
            update_cursor((int)terminal_column, (int)terminal_row);
        }
        break;
    }

    /* -- Erase in display: ESC [ 2 J (only mode 2 = full clear) ----------- */
    case 'J': {
        if (p0 == 2) {
            if (vbe_active()) {
                vbe_terminal_clear_screen();
            } else {
                for (size_t y = 0; y < VGA_HEIGHT; y++)
                    for (size_t x = 0; x < VGA_WIDTH; x++)
                        terminal_buffer[y * VGA_WIDTH + x] =
                            vga_entry(' ', terminal_color);
                terminal_row    = 0;
                terminal_column = 0;
                update_cursor(0, 0);
            }
        }
        break;
    }

    /* -- Erase in line: ESC [ n K ------------------------------------------ */
    case 'K': {
        if (vbe_active()) {
            vbe_terminal_erase_line(p0);
        } else {
            size_t start, end;
            switch (p0) {
            case 1: start = 0;               end = terminal_column + 1u; break;
            case 2: start = 0;               end = VGA_WIDTH;            break;
            default: start = terminal_column; end = VGA_WIDTH;            break;
            }
            for (size_t x = start; x < end; x++)
                terminal_buffer[terminal_row * VGA_WIDTH + x] =
                    vga_entry(' ', terminal_color);
        }
        break;
    }

    /* -- Save cursor: ESC [ s ---------------------------------------------- */
    case 's': {
        if (vbe_active())
            vbe_terminal_get_cursor(&ansi_saved_row, &ansi_saved_col);
        else {
            ansi_saved_row = (uint32_t)terminal_row;
            ansi_saved_col = (uint32_t)terminal_column;
        }
        break;
    }

    /* -- Restore cursor: ESC [ u ------------------------------------------- */
    case 'u': {
        if (vbe_active())
            vbe_terminal_set_cursor(ansi_saved_row, ansi_saved_col);
        else {
            terminal_row    = (size_t)ansi_saved_row;
            terminal_column = (size_t)ansi_saved_col;
            update_cursor((int)terminal_column, (int)terminal_row);
        }
        break;
    }

    /* -- SGR (select graphic rendition): ESC [ params m ------------------- */
    case 'm': {
        int np = ev->nparams;
        if (np == 0) {
            /* ESC[m == ESC[0m: reset all attributes */
            ansi_bold = 0;
            ansi_fg   = 0x00AAAAAAu;
            ansi_bg   = 0x00000088u;
            if (vbe_active()) vbe_terminal_setcolor(ansi_fg, ansi_bg);
            break;
        }
        for (int i = 0; i < np; i++) {
            int p = ev->params[i];
            if (p == 0) {
                ansi_bold = 0;
                ansi_fg   = 0x00AAAAAAu;
                ansi_bg   = 0x00000088u;
            } else if (p == 1) {
                ansi_bold = 1;
            } else if (p == 22) {
                ansi_bold = 0;
            } else if (p >= 30 && p <= 37) {
                /* Standard fg: use bright variant when bold is active */
                uint32_t c = ansi_sgr_color(ansi_bold ? p + 60 : p);
                if (c != 0xFFFFFFFFu) ansi_fg = c;
            } else if (p >= 40 && p <= 47) {
                uint32_t c = ansi_sgr_color(p);
                if (c != 0xFFFFFFFFu) ansi_bg = c;
            } else if (p >= 90 && p <= 97) {
                uint32_t c = ansi_sgr_color(p);
                if (c != 0xFFFFFFFFu) ansi_fg = c;
            } else if (p >= 100 && p <= 107) {
                uint32_t c = ansi_sgr_color(p);
                if (c != 0xFFFFFFFFu) ansi_bg = c;
            }
        }
        if (vbe_active()) vbe_terminal_setcolor(ansi_fg, ansi_bg);
        break;
    }

    default:
        /* Unknown CSI command -- silently ignore. */
        break;
    }
}

/* -- Terminal lock (SMP safety) ------------------------------------------- */
/*
 * All terminal output -- putchar and write -- is serialized through tty_lock.
 * Without this, concurrent printf() calls from the BSP and APs interleave
 * individual characters, producing garbled output like "spPinlock: PMM p]rotected".
 *
 * tty_lock is acquired IRQ-SAFE: printf() runs in thread context (WM/app
 * threads, IF=1, preemptible) AND in interrupt/exception context (the page-fault
 * handler prints "[pf] ..." etc.).  With a plain spinlock, a thread preempted
 * while holding tty_lock would wedge any later printf from an IRQ handler -- or,
 * under -smp 2, the other CPU spins forever on a lock whose holder was
 * descheduled.  That is exactly the freeze observed when many apps printf while
 * keyboard/mouse IRQs fire.  Disabling interrupts while held closes the window.
 */
static spinlock_t tty_lock = SPINLOCK_INIT;

/* -- Public terminal API -------------------------------------------------- */

/* Unlocked single-character output.  Must only be called with tty_lock held. */
static void terminal_putchar_impl(char c)
{
    ansi_event_t ev;
    int r = ansi_feed(&ansi_p, c, &ev);

    if (r == ANSI_CSI) {
        ansi_execute(&ev);
        return;
    }
    if (r != ANSI_CHAR)
        return;

    c = ev.ch;

    if (vbe_active()) { vbe_terminal_putchar(c); return; }

    /* VGA text mode */
    unsigned char uc = (unsigned char)c;
    if(uc == '\n') {
        terminal_handlenewline();
        update_cursor((int)terminal_column, (int)terminal_row);
        return;
    }
    if(uc == '\r') {
        terminal_column = 0;
        update_cursor((int)terminal_column, (int)terminal_row);
        return;
    }
    if(uc == '\b') {
        if (terminal_column > 0) {
            terminal_column--;
        } else if (terminal_row > 0) {
            terminal_row--;
            terminal_column = VGA_WIDTH - 1;
        }
        terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
        update_cursor((int)terminal_column, (int)terminal_row);
        return;
    }
    if (uc < 0x20u) return;

    terminal_putentryat(uc, terminal_color, terminal_column, terminal_row);
    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        terminal_handlenewline();
    }
    update_cursor((int)terminal_column, (int)terminal_row);
}

void terminal_putchar(char c) {
    uint32_t flags = spinlock_acquire_irqsave(&tty_lock);
    terminal_putchar_impl(c);
    spinlock_release_irqrestore(&tty_lock, flags);
}

void terminal_write(const char* data, size_t size) {
    /* IRQ-safe acquire: holding tty_lock with interrupts enabled let the timer
     * preempt the holder mid-write, after which any printf from an IF=0 context
     * (the page-fault handler's error path, e.g. a demand-paged thread stack)
     * spins on tty_lock forever -- the holder can never be rescheduled to
     * release it.  Disabling interrupts while held makes the critical section
     * non-preemptible, so the lock is never held across a context switch. */
    uint32_t flags = spinlock_acquire_irqsave(&tty_lock);
    for (size_t i = 0; i < size; i++)
        terminal_putchar_impl(data[i]);
    spinlock_release_irqrestore(&tty_lock, flags);
    /* vbe_flush() is called by pit_tick() at 100 Hz so we never hold
     * tty_lock across the slow hardware framebuffer copy.  Holding it
     * here would deadlock any concurrent terminal_write that fires while
     * the flush is running (e.g. kernel demand-fault printf vs. a user
     * thread's SYS_WRITE). */
}

void terminal_writestring(const char* data) {
    terminal_write(data, strlen(data));
}
