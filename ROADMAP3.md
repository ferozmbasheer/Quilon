# Quilon OS — Roadmap 3

The previous two roadmaps took Quilon from a bare multiboot stub to a
feature-complete hobby kernel. Here is what we now have:

- Higher-half kernel linked at `0xC0100000`
- Per-process page directories, demand paging, CoW fork
- Preemptive round-robin scheduler across up to `SMP_MAX_CPUS` cores (APIC)
- 25 system calls: write/read/open/close, fork/exec/wait/exit, sbrk, pipe,
  signals, PCI access, raw network send/recv, VBE query
- Ring-3 user space with a working libc (stdio, stdlib, string, unistd)
- Ring-3 shell (`user/shell/`) running as PID 1
- FAT16 read+write, VFS layer, initrd, pipes
- RTL8139 NIC with a full TCP/IP stack (Ethernet, ARP, IPv4, ICMP, UDP, TCP,
  DHCP) and SMP spinlocks protecting shared state
- VBE/VESA 32 bpp linear framebuffer with a bitmap-font text renderer

The theme of this roadmap is **turning a working OS into a usable one**: a
real terminal, POSIX-compatible I/O, a graphical desktop, a socket API user
programs can use, more hardware support, and the ability to run (and eventually
build) real programs inside Quilon.

---

## Table of Contents

11. [Terminal Emulator & Graphics Maturity](#11-terminal-emulator--graphics-maturity)
    - 11.1 ANSI Escape Code Terminal Emulator
    - 11.2 PSF2 Bitmap Font Loader
    - 11.3 Double-Buffered Framebuffer

12. [Extended POSIX System Calls](#12-extended-posix-system-calls)
    - 12.1 File Metadata & Directory Ops (stat, mkdir, chdir, getcwd)
    - 12.2 Blocking I/O — Sleep/Wakeup Instead of Spin-Wait
    - 12.3 Kernel Threads (clone)

13. [PS/2 Mouse Driver](#13-ps2-mouse-driver)

14. [Graphical Shell & Desktop Environment](#14-graphical-shell--desktop-environment)
    - 14.1 2D Graphics Library
    - 14.2 Window Manager & Compositor
    - 14.3 Graphical Terminal
    - 14.4 Simple GUI Apps

15. [BSD Socket API for User Space](#15-bsd-socket-api-for-user-space)
    - 15.1 Socket System Calls
    - 15.2 DNS Resolver
    - 15.3 HTTP Demo Utility

16. [More Device Drivers](#16-more-device-drivers)
    - 16.1 ATA Bus Master DMA
    - 16.2 Ext2 Filesystem

17. [Porting & Self-Hosting](#17-porting--self-hosting)
    - 17.1 Porting Lua
    - 17.2 Porting a Text Editor
    - 17.3 Self-Hosting: Compiling on Quilon

---

## Where We Are Now

| Component | File(s) | Status |
|-----------|---------|--------|
| Higher-half kernel | `linker.ld` (`0xC0100000`) | ✓ Complete |
| Demand paging + VMAs | `exceptions.c`, `vma.c` | ✓ Complete |
| CoW fork | `paging.c` (`paging_fork_address_space`) | ✓ Complete |
| SMP + APIC | `smp.c`, `apic.c` | ✓ Complete |
| 25 syscalls | `syscall.c` | ✓ Partial — POSIX coverage thin |
| Ring-3 shell | `user/shell/main.c` | ✓ Complete |
| User libc | `user/libc/` | ✓ Partial — missing stat, threads |
| FAT16 R/W, VFS, pipes | `fat16.c`, `vfs.c`, `pipe.c` | ✓ Complete |
| TCP/IP stack | `net.c`, `rtl8139.c` | ✓ Partial — no user-space sockets |
| VBE framebuffer | `vbe.c` | ✓ Complete — ANSI codes, PSF2 font, double-buffer |

---

## 11. Terminal Emulator & Graphics Maturity

### 11.1 ANSI Escape Code Terminal Emulator

**Why it matters:** Every Unix utility — `ls`, `grep`, `vim`, `gcc` — uses ANSI
escape sequences to produce coloured output and move the cursor. Without an ANSI
parser, ported programs either look wrong or crash.

The VBE driver already draws text into a linear framebuffer. The terminal layer
above it (`tty.c`) must learn to interpret escape sequences before passing
characters to `vbe_putchar`.

**Minimum subset to implement** (covers 90% of real-world terminal usage):

| Sequence | Meaning |
|----------|---------|
| `\033[H` | Move cursor to (1,1) |
| `\033[<r>;<c>H` | Move cursor to row r, col c |
| `\033[A` / `B` / `C` / `D` | Cursor up/down/right/left |
| `\033[2J` | Clear screen |
| `\033[K` | Erase to end of line |
| `\033[0m` | Reset attributes |
| `\033[1m` | Bold |
| `\033[3<n>m` | Set foreground colour (30–37 standard, 90–97 bright) |
| `\033[4<n>m` | Set background colour |

**Implementation sketch:**

```c
/* kernel/arch/i386/tty.c — add a small state machine to terminal_write */

typedef enum {
    ANSI_NORMAL,
    ANSI_ESC,       /* just saw \033 */
    ANSI_CSI,       /* just saw \033[ */
} ansi_state_t;

static ansi_state_t ansi_state = ANSI_NORMAL;
static char         ansi_buf[16];
static int          ansi_len = 0;

void terminal_write_char(char c)
{
    switch (ansi_state) {
    case ANSI_NORMAL:
        if (c == '\033') { ansi_state = ANSI_ESC; return; }
        vbe_putchar(c);       /* or tty_putchar for text-mode fallback */
        return;

    case ANSI_ESC:
        if (c == '[') { ansi_state = ANSI_CSI; ansi_len = 0; return; }
        ansi_state = ANSI_NORMAL;
        return;

    case ANSI_CSI:
        if (c >= 0x40 && c <= 0x7E) {          /* final byte — dispatch */
            ansi_buf[ansi_len] = '\0';
            ansi_dispatch(ansi_buf, c);
            ansi_state = ANSI_NORMAL;
        } else if (ansi_len < (int)sizeof(ansi_buf) - 1) {
            ansi_buf[ansi_len++] = c;
        }
        return;
    }
}

static void ansi_dispatch(const char *params, char cmd)
{
    int p[4] = {0, 0, 0, 0};
    /* parse up to 4 semicolon-separated integers */
    int n = sscanf_simple(params, p);   /* implement a tiny sscanf */

    switch (cmd) {
    case 'H': vbe_move_cursor(p[0] ? p[0]-1 : 0, p[1] ? p[1]-1 : 0); break;
    case 'J': if (p[0] == 2) vbe_clear(); break;
    case 'K': vbe_erase_line(); break;
    case 'A': vbe_move_cursor_rel(-p[0] ? -p[0] : -1, 0); break;
    case 'B': vbe_move_cursor_rel(p[0] ? p[0] : 1, 0);    break;
    case 'C': vbe_move_cursor_rel(0, p[0] ? p[0] : 1);    break;
    case 'D': vbe_move_cursor_rel(0, -p[0] ? -p[0] : -1); break;
    case 'm': ansi_sgr(p, n); break;
    }
}
```

> **Learning note:** ANSI parsing is a state machine, not a string search. Real
> terminal emulators (xterm, VTE) have hundreds of states; this minimal version
> is enough to run most CLI tools. The SGR (`m`) handler must decode sequences
> like `\033[1;32m` (bold + green) by splitting on semicolons.

---

### 11.2 PSF2 Bitmap Font Loader

**Why it matters:** The current VBE driver uses a hardcoded 8×16 font compiled
into the kernel binary. PC Screen Font 2 (PSF2) is the font format used by the
Linux console; thousands of free fonts are available. Loading one at boot makes
the terminal look sharp at any resolution and makes it easy to swap fonts.

PSF2 is a simple binary format: a 32-byte header followed by bitmap glyphs,
optionally followed by a Unicode table.

```c
/* include/kernel/psf.h */
typedef struct {
    uint32_t magic;          /* 0x864AB572 */
    uint32_t version;        /* 0 */
    uint32_t header_size;    /* offset to glyph data (bytes) */
    uint32_t flags;          /* 0=no unicode table, 1=has unicode table */
    uint32_t glyph_count;
    uint32_t bytes_per_glyph;
    uint32_t height;         /* pixels */
    uint32_t width;          /* pixels */
} __attribute__((packed)) psf2_header_t;

#define PSF2_MAGIC 0x864AB572u

int  psf2_load(const uint8_t *data, uint32_t len);
void psf2_draw_glyph(uint32_t ch, uint32_t x, uint32_t y,
                     uint32_t fg, uint32_t bg);
```

**Steps:**
1. Include a PSF2 font (e.g. `Tamsyn8x16r.psf`) in the initrd.
2. In `kernel_main`, after mounting initrd, read the font file and call
   `psf2_load()`.
3. `psf2_load` validates the magic, stores a pointer to the glyph bitmap data,
   and records `height`, `width`, `bytes_per_glyph`.
4. Replace the hardcoded `font8x16` array in `vbe.c` with a call to
   `psf2_draw_glyph`.

> **Learning note:** A PSF2 glyph for a character `c` starts at byte
> `c * bytes_per_glyph` in the glyph data. Each row of the glyph is
> `ceil(width / 8)` bytes; bit 7 of byte 0 is the leftmost pixel.

---

### 11.3 Double-Buffered Framebuffer

**Why it matters:** Writing directly to the VBE framebuffer causes visible
tearing: fast scrolling or screen clears show partial states. Double buffering
maintains a shadow buffer in RAM; the terminal renders into the shadow, then a
single `memcpy` flushes it to the physical framebuffer.

```c
/* vbe.c */
static uint8_t *shadow_buf;     /* kmalloc'd after paging is ready */

void vbe_flush(void)
{
    /* Copy shadow to physical framebuffer in one shot. */
    memcpy((void *)(uintptr_t)vbe.addr, shadow_buf,
           vbe.height * vbe.pitch);
}
```

Call `vbe_flush()` at the end of `terminal_write` (or on a VSync IRQ if the
hardware supports it). All `fb_write` calls go to `shadow_buf` instead of
`vbe.addr`.

At 800×600×32 the shadow buffer is 1.83 MiB — well within what `kmalloc` can
provide. At 1024×768×32 it is 3 MiB; if `kmalloc` cannot handle this in one
allocation, allocate contiguous pages directly from the PMM.

> **Learning note:** True VSync requires reading the Display Status register of
> the VGA controller — or using a hardware timer tuned to the monitor's refresh
> rate (typically 60 Hz). For a hobby OS, flushing on every `terminal_write` is
> fine and eliminates most visible tearing.

---

## 12. Extended POSIX System Calls

### 12.1 File Metadata & Directory Operations

**What is missing:** Programs like `ls -l`, `cp`, and `mkdir` require syscalls
that Quilon does not yet have:

| Syscall | What it does |
|---------|-------------|
| `stat` | Get file size, type, timestamps |
| `mkdir` | Create a directory |
| `chdir` / `getcwd` | Change/query working directory |
| `unlink` | Remove a file (alias for `SYS_REMOVE`) |
| `rename` | Rename a file |
| `lseek` | Seek within an open file |

**Adding `lseek` is the most impactful first step** because `SYS_READ` currently
always starts from offset 0 (re-reading from the beginning on every call).
Adding a per-file-descriptor offset field makes cat, read loops, and random
access work correctly.

```c
/* kernel/vfs.h — extend the fd table entry */
typedef struct {
    vfs_node_t *node;
    uint32_t    offset;     /* ← NEW: current read/write position */
    int         flags;
} vfs_fd_t;

/* syscall.h — new numbers */
#define SYS_STAT    26  /* stat(path, struct stat *) → 0 or -1 */
#define SYS_MKDIR   27  /* mkdir(path) → 0 or -1               */
#define SYS_CHDIR   28  /* chdir(path) → 0 or -1               */
#define SYS_GETCWD  29  /* getcwd(buf, len) → 0 or -1          */
#define SYS_LSEEK   30  /* lseek(fd, offset, whence) → new pos */
#define SYS_RENAME  31  /* rename(old, new) → 0 or -1          */
```

**Implementation priority:** `lseek` → `stat` → `getcwd`/`chdir` → `mkdir` →
`rename`. Each one is a thin wrapper over the VFS + FAT16 driver.

---

### 12.2 Blocking I/O — Sleep/Wakeup Instead of Spin-Wait

**Why it matters:** Several places in the kernel currently busy-loop:

```c
/* keyboard.c — current: wastes 100% of one CPU core */
char keyboard_getchar(void) {
    while (kb_read_pos == kb_write_pos);  /* spin */
    return kb_buffer[kb_read_pos++ % KEYBOARD_BUFFER_SIZE];
}

/* pipe.c — current: blocks one CPU for the entire wait */
/* net.c — DHCP/ping/TCP poll loops also spin */
```

Replace each spin-wait with a **wait queue**: a list of blocked processes. When
no data is available, the process is marked `PROC_BLOCKED` and taken off the run
queue. When data arrives (in the IRQ handler or producer side), it is marked
`PROC_READY` and added back.

```c
/* include/kernel/waitq.h */
typedef struct waitq_entry {
    process_t          *proc;
    struct waitq_entry *next;
} waitq_entry_t;

typedef struct {
    waitq_entry_t *head;
} waitq_t;

/* Block the current process on this queue. */
void waitq_sleep(waitq_t *wq);

/* Wake one process waiting on this queue (call from IRQ handler). */
void waitq_wake_one(waitq_t *wq);

/* Wake all processes waiting on this queue. */
void waitq_wake_all(waitq_t *wq);
```

```c
/* kernel/waitq.c — waitq_sleep */
void waitq_sleep(waitq_t *wq)
{
    waitq_entry_t entry = { .proc = current_process, .next = wq->head };
    wq->head = &entry;                 /* push onto wait list          */
    current_process->state = PROC_BLOCKED;
    scheduler_yield();                 /* give up the CPU              */
    /* When we return, we have been woken and are PROC_RUNNING again.  */
}
```

Add a `waitq_t` to `keyboard.c`, `pipe_t`, and the TCP RX slot. This change
makes the OS far more efficient on SMP — idle cores run other processes instead
of spin-burning.

> **Learning note:** `waitq_sleep` must be called with interrupts disabled (or
> while holding the appropriate spinlock) to avoid the classic "lost wakeup" race:
> the data could arrive between the "nothing available" check and the sleep call,
> and the wakeup would go undelivered.

---

### 12.3 Kernel Threads (clone)

**Why it matters:** User-space threading libraries (`pthreads`) need a way to
ask the kernel to create a new thread of execution sharing the same address
space as the calling process.

The Linux `clone()` syscall is the foundation. A minimal version for Quilon:

```c
/* syscall.h */
#define SYS_CLONE   32  /* clone(fn, stack, flags) → tid or -1 */

/* flags (minimal subset) */
#define CLONE_VM        0x0100   /* share address space (thread, not process) */
#define CLONE_FS        0x0200   /* share cwd */
#define CLONE_FILES     0x0400   /* share fd table */
```

```c
/* process.h — extend process_t */
uint32_t thread_group;     /* 0 = process leader; set to parent pid for threads */
```

**Implementation:**

1. Allocate a new `process_t` slot.
2. If `CLONE_VM` is set, copy the parent's `cr3` value *without* duplicating
   pages — both threads share the same page directory.
3. If `CLONE_FILES` is set, copy the fd table by value (both see the same files).
4. Set up the new thread's kernel stack with a context-switch frame so it starts
   executing at `fn(arg)` in user mode.
5. Mark it `PROC_READY`.

With `clone` working, `user/libc/pthread.c` can wrap it to provide
`pthread_create` / `pthread_join`.

> **Learning note:** Sharing a page directory between two threads means a write
> by one is immediately visible to the other — they truly share memory. This is
> different from `fork()` where each process gets its own copy. The kernel must
> ensure that when a thread exits it does *not* free the shared page directory —
> only when the last thread in the group exits should the address space be torn
> down.

---

## 13. PS/2 Mouse Driver

**Why it matters:** A graphical environment needs a pointing device. QEMU
emulates a PS/2 mouse on IRQ12 (the second PS/2 port), and the PS/2 controller
on the same 8042 chip as the keyboard.

The mouse sends 3-byte packets: button state, X delta, Y delta.

```c
/* arch/i386/mouse.c */
#define PS2_DATA    0x60
#define PS2_CMD     0x64

static int    mouse_x, mouse_y;
static uint8_t mouse_cycle = 0;   /* which byte of the 3-byte packet */
static uint8_t mouse_packet[3];

void irq12_handler(void)
{
    mouse_packet[mouse_cycle++] = inb(PS2_DATA);
    if (mouse_cycle == 3) {
        mouse_cycle = 0;

        /* Byte 0: bits 0-2 = buttons, bit 4 = X sign, bit 5 = Y sign */
        int dx = (int)mouse_packet[1];
        int dy = (int)mouse_packet[2];

        /* Sign-extend if sign bit set */
        if (mouse_packet[0] & 0x10) dx -= 256;
        if (mouse_packet[0] & 0x20) dy -= 256;

        mouse_x = clamp(mouse_x + dx, 0, (int)vbe_width() - 1);
        mouse_y = clamp(mouse_y - dy, 0, (int)vbe_height() - 1); /* Y is inverted */

        mouse_buttons = mouse_packet[0] & 0x07;

        /* Redraw cursor */
        vbe_draw_cursor(mouse_x, mouse_y);
    }
    outb(0x20, 0x20);   /* EOI to master PIC */
    outb(0xA0, 0x20);   /* EOI to slave PIC  */
}
```

**Initialization:**
```c
void mouse_initialize(void)
{
    /* Enable PS/2 auxiliary device */
    ps2_send_cmd(0xA8);

    /* Enable data reporting */
    ps2_send_cmd(0xD4);   /* route next byte to mouse */
    ps2_send_data(0xF4);  /* enable */

    /* Register IRQ12 */
    idt_set_gate(44, (uint32_t)irq12_stub, 0x08, 0x8E);
    irq_unmask(12);
}
```

Add `SYS_MOUSE_READ` to expose `(x, y, buttons)` to user space, or use a pipe
written by the IRQ handler.

---

## 14. Graphical Shell & Desktop Environment

**Prerequisites:** sections 11.2 (PSF2 font), 11.3 (double-buffered
framebuffer), 12.2 (blocking I/O), and 13 (PS/2 mouse). All four must be
working before starting this section — together they give you pixel-accurate
text, a tear-free display, a sleeping event loop, and a pointing device.

The goal is a minimal but usable graphical environment: a desktop background, a
windowed terminal that runs the existing ring-3 shell, and a handful of small
apps. The design stays as simple as possible — no compositor protocol, no GPU
acceleration — so every line of it is understandable.

---

### 14.1 2D Graphics Library

**Why it matters:** Drawing directly into `shadow_buf` in every application leads
to duplicated coordinate arithmetic, no clipping, and nothing to reuse. A thin
`libgfx` built on top of `vbe.c` gives all user programs the same drawing
primitives and makes the window manager straightforward to write.

**Kernel additions — three new syscalls:**

```c
/* syscall.h */
#define SYS_GFX_INFO   40  /* gfx_info(gfx_info_t *out) → 0 or -1 */
#define SYS_GFX_MAP    41  /* gfx_map() → user-space VA of shadow buffer, or -1 */
#define SYS_GFX_FLUSH  42  /* gfx_flush() → 0; copies shadow buf → hw framebuffer */
```

`SYS_GFX_MAP` maps the kernel's shadow buffer read-write into the calling
process's address space so that the window manager can write pixels without a
syscall per pixel. Only one process should call `SYS_GFX_MAP` (the compositor);
all other apps draw into their own off-screen buffers and hand them to the
compositor via shared memory or a pipe.

```c
/* include/kernel/gfx.h — passed from kernel to user via SYS_GFX_INFO */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      /* bytes per scanline */
    uint32_t bpp;        /* bits per pixel — always 32 for VBE */
} gfx_info_t;
```

**User-space library — `user/libgfx/`:**

```c
/* user/libgfx/include/gfx.h */

typedef struct { uint32_t *pixels; int w, h, pitch; } canvas_t;
typedef struct { int x, y, w, h; }                    rect_t;
typedef uint32_t                                       color_t;

#define GFX_RGB(r,g,b) (((r)<<16)|((g)<<8)|(b))
#define GFX_RGBA(r,g,b,a) (((a)<<24)|((r)<<16)|((g)<<8)|(b))

/* Create an off-screen canvas backed by malloc'd memory. */
canvas_t *canvas_create(int w, int h);
void      canvas_free(canvas_t *c);

/* Primitives — all respect the clip rectangle. */
void gfx_fill(canvas_t *c, color_t col);
void gfx_fill_rect(canvas_t *c, rect_t r, color_t col);
void gfx_draw_rect(canvas_t *c, rect_t r, color_t col);
void gfx_blit(canvas_t *dst, int dx, int dy,
              const canvas_t *src, rect_t src_rect);
void gfx_draw_text(canvas_t *c, int x, int y,
                   const char *str, color_t fg, color_t bg);

/* Clip rectangle — drawing outside it is silently discarded. */
void gfx_set_clip(canvas_t *c, rect_t clip);
void gfx_clear_clip(canvas_t *c);
```

**Implementation of `gfx_fill_rect`** (the foundation of everything else):

```c
/* user/libgfx/gfx.c */
void gfx_fill_rect(canvas_t *c, rect_t r, color_t col)
{
    /* Intersect r with the clip rect if one is active. */
    int x0 = r.x < 0 ? 0 : r.x;
    int y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > c->w ? c->w : r.x + r.w;
    int y1 = r.y + r.h > c->h ? c->h : r.y + r.h;
    if (x0 >= x1 || y0 >= y1) return;

    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->pixels + y * c->pitch + x0;
        for (int x = x0; x < x1; x++)
            *row++ = col;
    }
}
```

`gfx_blit` is equally simple — a double loop that copies a rectangle from one
canvas to another, respecting both clip rects.

`gfx_draw_text` calls the PSF2 renderer from section 11.2 (exposed to user space
either via a syscall or by mapping the font glyph data read-only into user space).

> **Learning note:** The clip rectangle turns rendering into a safe operation:
> even if a window thinks it is at (−10, −10) with size 100×100, no pixel outside
> the visible screen is ever written. This makes the window manager trivially safe
> to compose — it sets each window's clip to its on-screen bounds before blitting.

---

### 14.2 Window Manager & Compositor

**Why it matters:** A window manager is the process that owns the screen. It
decides which window is in front, draws decorations (title bars, close buttons),
routes mouse clicks to the right window, and composites everything into the final
frame before flushing.

Quilon's WM is a single-process compositor. It uses `SYS_GFX_MAP` to get
exclusive write access to the shadow framebuffer, then composites all registered
windows on every frame.

**Window descriptor:**

```c
/* user/wm/wm.h */
#define WM_MAX_WINDOWS  16
#define WM_TITLE_LEN    32
#define TITLEBAR_H      20   /* pixels */

typedef struct {
    int        id;
    rect_t     bounds;       /* position and size on screen, excluding title bar */
    char       title[WM_TITLE_LEN];
    canvas_t  *backbuf;      /* the app draws here */
    int        focused;
    int        active;
} window_t;
```

**Compositor loop:**

```c
/* user/wm/wm.c */

static canvas_t screen;      /* wraps the kernel shadow buffer via SYS_GFX_MAP */
static window_t windows[WM_MAX_WINDOWS];
static int      num_windows;

void wm_composite(void)
{
    /* 1. Clear the desktop with a background colour. */
    gfx_fill(&screen, GFX_RGB(30, 30, 60));   /* dark blue desktop */

    /* 2. Composite windows back-to-front (painter's algorithm). */
    for (int i = 0; i < num_windows; i++) {
        window_t *w = &windows[i];
        if (!w->active) continue;

        /* Draw title bar. */
        rect_t tbar = { w->bounds.x, w->bounds.y - TITLEBAR_H,
                        w->bounds.w, TITLEBAR_H };
        color_t tbar_col = w->focused ? GFX_RGB(80,80,200) : GFX_RGB(60,60,60);
        gfx_fill_rect(&screen, tbar, tbar_col);
        gfx_draw_text(&screen, tbar.x + 4, tbar.y + 4, w->title,
                      GFX_RGB(255,255,255), tbar_col);

        /* Close button — red square in top-right corner. */
        rect_t close_btn = { tbar.x + tbar.w - 18, tbar.y + 2, 16, 16 };
        gfx_fill_rect(&screen, close_btn, GFX_RGB(200,40,40));

        /* Blit the app's back-buffer into its content area. */
        gfx_set_clip(&screen, w->bounds);
        gfx_blit(&screen, w->bounds.x, w->bounds.y,
                 w->backbuf, (rect_t){0, 0, w->backbuf->w, w->backbuf->h});
        gfx_clear_clip(&screen);

        /* Window border. */
        gfx_draw_rect(&screen, (rect_t){tbar.x, tbar.y,
                                        tbar.w, tbar.h + w->bounds.h},
                      GFX_RGB(100,100,100));
    }

    /* 3. Draw the mouse cursor. */
    wm_draw_cursor();

    /* 4. Flush shadow buffer to hardware framebuffer. */
    syscall(SYS_GFX_FLUSH, 0, 0, 0);
}
```

**Mouse hit-testing** (called in the main event loop when a click arrives):

```c
static int wm_hittest(int mx, int my)
{
    /* Iterate windows front-to-back; first hit wins. */
    for (int i = num_windows - 1; i >= 0; i--) {
        window_t *w = &windows[i];
        if (!w->active) continue;
        rect_t full = { w->bounds.x, w->bounds.y - TITLEBAR_H,
                        w->bounds.w, w->bounds.h + TITLEBAR_H };
        if (mx >= full.x && mx < full.x + full.w &&
            my >= full.y && my < full.y + full.h)
            return i;
    }
    return -1;
}
```

**Event loop structure:**

```c
void wm_run(void)
{
    int mouse_fd = open("/dev/mouse", O_RDONLY);
    int kbd_fd   = open("/dev/kbd",   O_RDONLY);

    while (1) {
        /* Block until a mouse or keyboard event arrives (uses 12.2 wait queues). */
        mouse_event_t me;
        if (read(mouse_fd, &me, sizeof(me)) == sizeof(me))
            wm_handle_mouse(&me);

        kbd_event_t ke;
        if (read(kbd_fd, &ke, sizeof(ke)) == sizeof(ke))
            wm_handle_key(&ke);

        /* Redraw apps that marked their back-buffer dirty. */
        wm_composite();
    }
}
```

**IPC between WM and apps:** The simplest approach is a named pipe per window.
When an app calls `wm_open_window(title, w, h)`, the WM creates a window entry
and returns a file descriptor to a shared-memory region for the back-buffer. Apps
write pixels directly into that region. A second "event pipe" carries keyboard
and mouse events from the WM to the app. This avoids inventing a new IPC
mechanism: pipes and shared memory are already in the kernel.

> **Learning note:** The painter's algorithm (drawing windows back-to-front) is
> the simplest correct compositing strategy. It overdraws every pixel on every
> frame, but at 800×600×32 bpp the shadow buffer is only 1.83 MiB — a single
> `memcpy` to flush is ~200 µs at 10 GB/s, well within a 60 Hz frame budget.
> The 1980s Nintendo hardware used the same approach (background, then sprites,
> front-to-back). Damage tracking (only redrawing changed regions) is an
> optimization you can add later.

---

### 14.3 Graphical Terminal

**Why it matters:** The ring-3 shell currently writes to the text-mode VGA
buffer. Replacing that with a window managed by the compositor gives you a
proper terminal application — one you can resize, move, and run alongside other
apps.

The graphical terminal (`user/gterm/`) is itself a user-space program. It:
1. Registers a window with the WM.
2. Forks a child running the existing `shell.elf`.
3. Redirects the child's `stdin`/`stdout`/`stderr` to a pair of pipes.
4. Reads the child's output, interprets ANSI sequences, and renders text into its
   back-buffer using `libgfx`.
5. Forwards keyboard events from the WM into the child's stdin.

```c
/* user/gterm/main.c — skeleton */

#include <gfx.h>
#include <wm_client.h>
#include <stdio.h>
#include <unistd.h>

#define COLS  80
#define ROWS  24
#define CELL_W  8
#define CELL_H 16

static char  cell_char  [ROWS][COLS];
static color_t cell_fg  [ROWS][COLS];
static color_t cell_bg  [ROWS][COLS];
static int   cursor_row, cursor_col;
static canvas_t *backbuf;

/* ANSI state machine (mirrors the kernel tty.c machine but runs in user space) */
static void gterm_write(const char *data, int len) { /* ... */ }

static void gterm_render(void)
{
    gfx_fill(backbuf, GFX_RGB(0, 0, 0));
    for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
        char ch = cell_char[r][c] ? cell_char[r][c] : ' ';
        gfx_draw_text(backbuf, c * CELL_W, r * CELL_H,
                      (char[]){ch, 0}, cell_fg[r][c], cell_bg[r][c]);
    }
    /* Draw block cursor. */
    gfx_fill_rect(backbuf,
                  (rect_t){cursor_col*CELL_W, cursor_row*CELL_H, CELL_W, CELL_H},
                  GFX_RGB(200, 200, 200));
    wm_mark_dirty();
}

int main(void)
{
    int win = wm_open_window("Terminal", COLS*CELL_W, ROWS*CELL_H);
    backbuf = wm_get_backbuf(win);

    int to_shell[2], from_shell[2];
    pipe(to_shell); pipe(from_shell);

    if (fork() == 0) {
        /* Child: rewire stdin/stdout/stderr to the pipes. */
        dup2(to_shell[0],   0);
        dup2(from_shell[1], 1);
        dup2(from_shell[1], 2);
        close(to_shell[0]); close(to_shell[1]);
        close(from_shell[0]); close(from_shell[1]);
        exec("/shell.elf");
    }
    close(to_shell[0]); close(from_shell[1]);

    char buf[256];
    while (1) {
        /* Read shell output → render. */
        int n = read(from_shell[0], buf, sizeof(buf));
        if (n > 0) { gterm_write(buf, n); gterm_render(); }

        /* Read WM keyboard events → forward to shell. */
        kbd_event_t ke;
        if (wm_read_key(win, &ke))
            write(to_shell[1], &ke.ascii, 1);
    }
}
```

The ANSI state machine in `gterm_write` is identical in structure to the one
described in section 11.1, but it updates the `cell_char`/`cell_fg`/`cell_bg`
grid instead of writing to a kernel buffer. This separation keeps the terminal
logic out of the kernel entirely.

**Scrollback:** Add a ring buffer of `SCROLLBACK` rows above the visible grid.
When the terminal scrolls up (a new line arrives at the bottom), copy row 0 into
the scrollback buffer and shift the grid up by one. Mouse wheel events from the
WM adjust a `scroll_offset` and re-render.

> **Learning note:** Separating the terminal *emulator* (ANSI state machine +
> cell grid) from the terminal *renderer* (drawing cells into a canvas) is the
> same split used by every real terminal emulator (VTE, kitty, alacritty). The
> emulator is pure logic with no rendering knowledge; the renderer is pure drawing
> with no ANSI knowledge. This makes both independently testable.

---

### 14.4 Simple GUI Apps

With the graphics library and WM in place, small apps are straightforward to
write. Each registers a window, draws into its back-buffer, and handles events.

---

#### Clock

The simplest possible app. It calls `SYS_GETTICKS` every second, converts to
HH:MM:SS, and draws the string centred in a small window.

```c
/* user/clock/main.c */
int main(void)
{
    int win = wm_open_window("Clock", 160, 40);
    canvas_t *buf = wm_get_backbuf(win);

    while (1) {
        uint32_t ticks = getticks();   /* seconds since boot */
        int h = (ticks / 3600) % 24, m = (ticks / 60) % 60, s = ticks % 60;
        char str[9];
        snprintf(str, sizeof(str), "%02d:%02d:%02d", h, m, s);
        gfx_fill(buf, GFX_RGB(10, 10, 10));
        gfx_draw_text(buf, 40, 12, str, GFX_RGB(0,255,128), GFX_RGB(10,10,10));
        wm_mark_dirty();
        sleep(1);
    }
}
```

---

#### File Browser

A scrollable list of the current directory. Clicking a directory entry navigates
into it; clicking a file sends its path to a viewer app.

```c
/* user/filebr/main.c — key structs */

#define LIST_ITEM_H  16
#define MAX_ENTRIES  64

static char entries[MAX_ENTRIES][256];
static int  is_dir[MAX_ENTRIES];
static int  num_entries, scroll_top, selected;

static void fb_render(canvas_t *buf, int w, int h)
{
    gfx_fill(buf, GFX_RGB(20, 20, 20));
    for (int i = scroll_top; i < num_entries; i++) {
        int y = (i - scroll_top) * LIST_ITEM_H;
        if (y + LIST_ITEM_H > h) break;
        color_t bg = (i == selected) ? GFX_RGB(50,50,150) : GFX_RGB(20,20,20);
        gfx_fill_rect(buf, (rect_t){0, y, w, LIST_ITEM_H}, bg);
        color_t fg = is_dir[i] ? GFX_RGB(100,180,255) : GFX_RGB(220,220,220);
        gfx_draw_text(buf, 4, y + 1, entries[i], fg, bg);
    }
}
```

Mouse click handling: convert Y coordinate to `(click_y / LIST_ITEM_H) +
scroll_top` to find which entry was clicked. A double-click (two clicks within
300 ms) on a directory calls `chdir` + re-reads the directory; on a file it
launches the text viewer.

---

#### Text Viewer

Opens a file, reads it into a buffer, and renders it one screen-height at a time.
Mouse wheel or Page Up/Down adjust a line offset.

```c
/* user/textview/main.c — render loop */
static void tv_render(canvas_t *buf, char **lines, int nlines,
                      int scroll, int rows, int cols)
{
    gfx_fill(buf, GFX_RGB(15, 15, 15));
    for (int r = 0; r < rows && scroll + r < nlines; r++) {
        char line[256];
        strncpy(line, lines[scroll + r], cols);
        line[cols] = '\0';
        gfx_draw_text(buf, 0, r * 16, line,
                      GFX_RGB(200,200,200), GFX_RGB(15,15,15));
    }
}
```

**Suggested layout of `user/` after section 14:**

```
user/
├── libgfx/         ← 2D graphics library (canvas, primitives, text)
├── wm/             ← compositor (owns /dev/screen, dispatches events)
├── wm_client/      ← thin client library for app ↔ WM IPC
├── gterm/          ← graphical terminal (wraps shell.elf)
├── clock/          ← digital clock app
├── filebr/         ← file browser
├── textview/       ← text file viewer
└── shell/          ← existing ring-3 text shell (unchanged)
```

> **Learning note:** Every app in section 14 is a completely ordinary ring-3 user
> program. None of them need kernel changes beyond the three syscalls added in
> 14.1. This is the power of a clean user-space abstraction: once the kernel
> exposes a framebuffer and flush, the entire desktop is user-space code. Writing
> a new app is no different from writing any other Quilon program.

---

## 15. BSD Socket API for User Space

### 15.1 Socket System Calls

**Why it matters:** Quilon currently has `SYS_NET_SEND` and `SYS_NET_RECV` for
raw Ethernet frames — these are too low-level for user programs. The BSD socket
API (`socket`, `bind`, `connect`, `send`, `recv`, `close`) is what every network
program expects.

**New syscalls:**

```c
/* syscall.h */
#define SYS_SOCKET   43  /* socket(domain, type, proto) → fd or -1 */
#define SYS_BIND     44  /* bind(fd, port) → 0 or -1               */
#define SYS_CONNECT  45  /* connect(fd, ip, port) → 0 or -1        */
#define SYS_SEND     46  /* send(fd, buf, len) → bytes or -1        */
#define SYS_RECV     47  /* recv(fd, buf, len) → bytes or -1        */
#define SYS_LISTEN   48  /* listen(fd) → 0 or -1                    */
#define SYS_ACCEPT   49  /* accept(fd) → new fd or -1               */

/* socket domains */
#define AF_INET    2

/* socket types */
#define SOCK_STREAM  1   /* TCP */
#define SOCK_DGRAM   2   /* UDP */
```

**Kernel side:** A `socket_t` object lives in the VFS `vfs_node_t` (the fd
table already handles close/read/write generically). `SYS_SOCKET` allocates a
`socket_t`, creates a VFS node for it, and returns an fd. `SYS_CONNECT` calls
`net_tcp_connect()` (which you will add to `net.c`). `SYS_RECV` on a TCP socket
calls `net_tcp_recv()` and sleeps on a wait queue if no data is ready.

**User-space side** (`user/libc/include/sys/socket.h`):

```c
int socket(int domain, int type, int protocol);
int bind(int fd, uint32_t ip, uint16_t port);
int connect(int fd, uint32_t ip, uint16_t port);
int send(int fd, const void *buf, int len);
int recv(int fd, void *buf, int len);
int listen(int fd);
int accept(int fd);
```

Each function is a thin `int $0x80` wrapper — the same pattern as `write()` in
`user/libc/syscall.S`.

> **Learning note:** Sockets look like files to user programs because the VFS
> abstracts them behind `read`/`write`/`close`. The kernel's fd table already
> stores per-fd function pointers (`vfs_ops_t`); a socket just provides its own
> implementation of those ops.

---

### 15.2 DNS Resolver

**Why it matters:** Every networked program uses hostnames, not raw IP addresses.
A stub resolver translates `"quilon.local"` → `192.168.1.1` by sending a UDP
query to a DNS server.

DNS is a binary protocol over UDP port 53. A minimal resolver handles only type
A queries (IPv4 addresses) and ignores everything else.

```c
/* kernel/net.c — add: */
int net_dns_lookup(const char *hostname, uint32_t dns_server_ip,
                   uint32_t *out_ip);
```

**Wire format (minimal):** A DNS query is a 12-byte header followed by the
encoded hostname and a 4-byte type/class trailer. The reply is the same
structure with an answer section appended.

```c
typedef struct {
    uint16_t id;
    uint16_t flags;      /* 0x0100 = standard query, recursion desired */
    uint16_t qdcount;    /* number of questions (1) */
    uint16_t ancount;    /* number of answers in reply */
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed)) dns_hdr_t;
```

The hostname `"api.example.com"` encodes as
`\x03api\x07example\x03com\x00`. Parse the response's answer section for an
`A` record (type 1, class 1) and read the 4-byte IP.

With DNS working, add `SYS_GETADDRINFO` (or implement it in user-space libc
using `SYS_NET_SEND`/`SYS_NET_RECV` directly).

---

### 15.3 HTTP Demo Utility

**Why it matters:** It is a concrete, testable milestone: `wget http://...` or
`curl http://...` running on Quilon and printing a response is something you can
demo, and it exercises everything in sections 15.1 and 15.2.

A minimal HTTP/1.0 GET request is four lines:

```
GET /path HTTP/1.0\r\n
Host: example.com\r\n
\r\n
```

**User-space implementation** (`user/wget/main.c`):

```c
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    /* 1. DNS lookup */
    uint32_t ip;
    if (dns_resolve(argv[1], &ip) != 0) { puts("DNS failed"); return 1; }

    /* 2. TCP connect to port 80 */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, ip, 80) != 0) { puts("connect failed"); return 1; }

    /* 3. Send request */
    char req[256];
    snprintf(req, sizeof(req), "GET / HTTP/1.0\r\nHost: %s\r\n\r\n", argv[1]);
    send(fd, req, strlen(req));

    /* 4. Print response */
    char buf[512];
    int n;
    while ((n = recv(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);

    close(fd);
    return 0;
}
```

> **Learning note:** HTTP/1.0 is the right target — no persistent connections,
> no chunked transfer encoding, no header compression. The server sends headers,
> a blank line, then the body, then closes the connection. This simplicity means
> `recv` in a loop is all you need.

---

## 16. More Device Drivers

### 16.1 ATA Bus Master DMA

**Why it matters:** The current ATA driver uses Programmed I/O (PIO): the CPU
copies each 512-byte sector between the disk and memory one word at a time.
On a 100 MHz 486 this took 10% of the CPU; on QEMU it is still measurable. Bus
Master DMA lets the disk controller copy data to memory directly — the CPU only
sets up the transfer and waits for an IRQ.

**How it works:**

1. The ATA controller's Bus Master registers are at PCI BAR4 (found via PCI
   enumeration). Offset 0 = Primary channel command, offset 8 = Secondary.
2. Build a Physical Region Descriptor Table (PRDT) in memory: one or more
   entries, each describing a physical buffer address and byte count.
3. Write the PRDT address to the Bus Master PRDT register, set the direction
   bit, and issue the ATA DMA command (`0xC8` for read, `0xCA` for write).
4. When the transfer completes, the ATA controller fires IRQ14. The IRQ handler
   clears the Bus Master status register and wakes the waiting process.

```c
/* arch/i386/ata_dma.c */
typedef struct {
    uint32_t phys_addr;   /* physical address of buffer */
    uint16_t byte_count;  /* 0 = 65536 bytes */
    uint16_t reserved;    /* bit 15 must be 1 for last entry */
} __attribute__((packed)) prdt_entry_t;

void ata_dma_read(uint32_t lba, uint32_t count, void *buf)
{
    /* Set up PRDT, issue command, wait on IRQ14 */
}
```

> **Learning note:** DMA buffers must be physically contiguous and below 4 GiB
> (which is guaranteed for you since you have no memory above 4 GiB). You also
> cannot DMA into virtual addresses — you must pass a *physical* address. Use
> `paging_virt_to_phys()` to convert if the buffer is in the kernel heap.

---

### 16.2 Ext2 Filesystem

**Why it matters:** FAT16 has hard limits: 8.3 filenames, no permissions, no
symbolic links, a maximum of 512 root directory entries. Ext2 (the classic Linux
filesystem) removes all of these limits. It is also what the cross-compiler
toolchain produces when building a disk image.

Ext2 is significantly more complex than FAT16, but the OSDev Wiki has a
complete specification. The key structures:

| Structure | Location | Purpose |
|-----------|----------|---------|
| Superblock | Byte 1024 | Overall FS metadata (block size, inode count) |
| Block group descriptor | After superblock | One per block group |
| Inode table | Per block group | File metadata (size, permissions, block pointers) |
| Directory entry | In inode's data blocks | Name → inode number mapping |

**Implementation plan:**
1. Read the superblock; validate the magic (`0xEF53`); compute block size
   (`1024 << s_log_block_size`).
2. Read the block group descriptor table.
3. Implement `ext2_read_inode(ino_num)` — computes which block group and
   which inode table block, then reads the 128-byte inode.
4. Implement `ext2_read_file(inode, offset, len, buf)` — follows direct,
   indirect, and doubly-indirect block pointers.
5. Implement `ext2_readdir(inode)` — iterates the variable-length directory
   entry structures in the inode's data blocks.
6. Register as a VFS driver alongside FAT16.

Write support (allocating inodes and blocks, updating the block group
descriptor) follows the same pattern as FAT16 write but with more moving parts.

> **Learning note:** Ext2 inodes store 12 direct block pointers, 1 singly
> indirect pointer (points to a block of block pointers), 1 doubly indirect
> pointer, and 1 triply indirect pointer. For a 1 KiB block size this covers
> files up to (12 + 256 + 256² + 256³) × 1024 ≈ 16 GiB. You only need to
> implement direct + singly indirect to support files up to about 268 KiB —
> enough for ported programs.

---

## 17. Porting & Self-Hosting

### 17.1 Porting Lua

**Why it matters:** Porting an existing program to Quilon is the acid test of
POSIX compatibility. Lua 5.4 is a good first target: it is written in portable C,
has minimal dependencies (just `libc`), and provides a useful scripting layer.

**What Lua needs from Quilon:**
- `malloc`/`free` — already provided via `sbrk`-backed user-space allocator
- `fopen`/`fread`/`fwrite`/`fclose` — needs `lseek` and `stat` (section 12.1)
- `printf` — already working
- `time()` — add `SYS_GETTICKS` wrapper that returns seconds since boot
- No threads, no signals required for the base interpreter

**Steps:**
1. Download the Lua 5.4 source.
2. Cross-compile with the `i686-elf-gcc` toolchain, passing
   `-DLUA_USE_POSIX -ffreestanding -nostdlib` and linking against
   `user/libc/libc.a`.
3. Fix compilation errors by stubbing out missing POSIX functions
   (`setvbuf`, `tmpnam`, etc.) in `user/libc/posix_stubs.c`.
4. Add the `lua.elf` binary to the initrd.
5. Run `exec lua.elf` from the shell.

**Expected stubs:** About 5–10 small functions that Lua calls but Quilon does
not support. Each stub either returns an error code or is a no-op.

---

### 17.2 Porting a Text Editor

**Why it matters:** A text editor running inside Quilon closes the loop: you can
write and edit files without leaving the OS. It also provides a real-world test
of the ANSI terminal emulator (section 11.1).

**Recommended target: `ed`** — the POSIX line editor. It has no cursor movement
(no ANSI required), reads/writes files, and the entire POSIX specification is
short enough to implement from scratch if the full source is unavailable.

**Alternative: a minimal `vi` clone.** `vi` requires a working terminal
(cursor movement, screen clearing) but is more useful once ANSI is working.
Several minimal vi implementations are under 2000 lines of portable C.

**What a text editor needs that Quilon may not yet have:**
- `lseek` (section 12.1) — to rewrite a file in place
- `rename` (section 12.1) — for atomic save (write to temp, rename)
- `SIGINT` delivery when Ctrl-C is pressed in the shell (currently only
  sent programmatically via `signal_send`)

To add Ctrl-C: in the keyboard IRQ handler, check if the new character is `\x03`
(ASCII ETX); if so, call `signal_send(current_process, SIGINT)` instead of
buffering it.

---

### 17.3 Self-Hosting: Compiling on Quilon

**Why it matters:** A self-hosting OS — one that can compile its own programs
inside itself — is the classic milestone for a hobby OS project. Even a partial
solution (compiling C programs *to* Quilon inside QEMU) is a major achievement.

**The challenge:** The cross-compiler (`i686-elf-gcc`) runs on the host. Getting
a compiler to run *inside* Quilon requires porting GCC or an alternative.

**Practical path — port TinyCC (tcc):**

TinyCC is a minimal C compiler (~50 000 lines vs GCC's millions) that compiles
itself and produces working binaries. It supports `i386` output natively.

Requirements:
- A working libc (with `malloc`, file I/O, `exec`) — nearly there after section 12.
- `mmap` or `sbrk` for large allocations — `sbrk` already exists.
- No threads or signals required for the compiler itself.

**Steps:**
1. Cross-compile TinyCC for i686-elf on the host.
2. Bundle it in the disk image.
3. Fix missing libc functions iteratively.
4. Verify: run `tcc hello.c -run` inside Quilon and see `Hello, World!`.

Once TinyCC works, Quilon can compile C programs written in Quilon's own shell,
editing with the ported text editor, without ever leaving the virtual machine.

> **Learning note:** Self-hosting does not require the OS to compile its own
> *kernel* — that requires a cross-compiler toolchain that is far more complex.
> Self-hosting for user-space programs (compile, link, run within the OS) is
> already a remarkable and satisfying milestone.

---

## Suggested Milestones

| Milestone | Sections | What it proves |
|-----------|----------|----------------|
| **Milestone 7 — Colour terminal** | 11.1 + 11.2 | ANSI colour output; ported tools look correct |
| **Milestone 8 — Proper file I/O** | 12.1 | `cat`, `cp`, `ls -l` work from the ring-3 shell |
| **Milestone 9 — Efficient scheduler** | 12.2 | CPU idles on `hlt` instead of spin-burning |
| **Milestone 10 — Mouse** | 13 | Cursor drawn on screen, moves with the mouse |
| **Milestone 11 — Desktop** | 11.3 + 14.1 + 14.2 | Window manager composites windows; double-buffer eliminates tearing |
| **Milestone 12 — Graphical terminal** | 14.3 | Shell runs inside a moveable terminal window |
| **Milestone 13 — Apps** | 14.4 | Clock, file browser, and text viewer run alongside the terminal |
| **Milestone 14 — Socket programs** | 15.1 + 15.2 + 15.3 | `wget` fetches a page over TCP from inside QEMU |
| **Milestone 15 — Ext2** | 16.2 | Quilon boots from an ext2 disk image |
| **Milestone 16 — Lua** | 17.1 | `exec lua.elf` at the shell prompt runs a Lua script |
| **Milestone 17 — Self-hosting** | 17.2 + 17.3 | TinyCC compiles a C program inside Quilon |

---

## Reference Reading

| Resource | What it covers |
|----------|----------------|
| **VT100 / ANSI escape code reference** (vt100.net) | Complete escape sequence definitions. Start with the "Control Sequences" page. |
| **PSF2 font format** (man 5 psfheader) | One page. The format is simpler than the man page makes it look. |
| **Linux `clone(2)` man page** | Defines all the `CLONE_*` flags and their semantics. Quilon only needs `CLONE_VM`, `CLONE_FS`, `CLONE_FILES`. |
| **serenityOS source** (github.com/SerenityOS/serenity) | A hobby OS with a full GUI written from scratch in C++. The LibGfx and WindowServer components are the clearest real-world reference for section 14's design. |
| **Xlib protocol overview** (X.Org docs) | X11 client-server window model. Quilon's WM is far simpler, but reading the overview clarifies why Quilon's direct-mapped framebuffer design avoids a lot of complexity. |
| **"Computer Graphics: Principles and Practice"** (Foley et al.) | Clipping, blitting, painter's algorithm, and compositing theory. Chapters 3 and 19 are directly relevant to libgfx. |
| **RFC 1035** (DNS) | The DNS wire format. Section 3 (domain name encoding) and Section 4 (message format) are all you need. |
| **ATA/ATAPI-6 specification** | Chapter 9 covers DMA and Bus Master operation. Free PDF from the T13 committee. |
| **Ext2 OSDev Wiki** (wiki.osdev.org/Ext2) | Clear walk-through of every structure. More accessible than the original paper. |
| **TinyCC source** (repo.or.cz/tinycc.git) | Read `tcc.c` and `i386-gen.c` to understand how a simple C compiler works. ~10 kloc of readable C. |
| **"Lions' Commentary on UNIX"** | John Lions' annotated Unix V6 source. Illuminates how real processes, files, and the VFS work in a minimal Unix. |

---

*Document written for Quilon OS — May 2026.*
