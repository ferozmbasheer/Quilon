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

*(Updated 2026-06: Sections 11–14 are complete. The graphical desktop is stable
after a substantial round of concurrency hardening and a WM rearchitecture —
see the note under 14.2. Next milestone is Section 15, the BSD socket API.)*

| Component | File(s) | Status |
|-----------|---------|--------|
| Higher-half kernel | `linker.ld` (`0xC0100000`) | ✓ Complete |
| Demand paging + VMAs | `exceptions.c`, `vma.c` | ✓ Complete |
| CoW fork | `paging.c` (`paging_fork_address_space`) | ✓ Complete |
| >4 MiB physical access | `paging.c` (`kmap`/`pmm_alloc_data_page`) | ✓ Complete — data pages above the 4 MiB identity map |
| SMP + APIC | `smp.c`, `apic.c` | ✓ Complete — IRQ stubs load kernel segments |
| IRQ-safe kernel locks | `spinlock.h`, `pmm.c`, `tty.c`, `ata.c`, `waitq.c` | ✓ Complete — see [thread-concurrency memory] |
| ~28 syscalls | `syscall.c` | ✓ Partial — POSIX coverage thin; user-ptr validated |
| Ring-3 shell | `user/shell/main.c` | ✓ Complete |
| User libc | `user/libc/` | ✓ Partial — missing stat wrapper, real threads |
| FAT16 R/W, VFS, pipes | `fat16.c`, `vfs.c`, `pipe.c` | ✓ Complete; per-process std fds via `owns_std_fds` |
| TCP/IP stack | `net.c`, `rtl8139.c` | ✓ Partial — **no user-space sockets (Section 15)** |
| VBE framebuffer | `vbe.c` | ✓ Complete — ANSI codes, PSF2 font, double-buffer, graphics-mode gating |
| Terminal emulator (11) | `gterm.h`, `tty.c` | ✓ Complete |
| Extended POSIX (12) | `vfs.c`, `waitq.c`, `syscall.c` | ✓ Complete (stat/mkdir/chdir/getcwd, blocking I/O, clone) |
| PS/2 mouse (13) | `mouse.c` | ✓ Complete |
| Graphical desktop (14) | `user/wm/`, `user/{gterm,clock,filebr,textview}/` | ✓ Complete — **single-threaded event loop** (see 14.2 note) |

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

> **⚠ ARCHITECTURE UPDATE (2026-06): the WM is now SINGLE-THREADED.**
>
> The threaded design described in the rest of this section (every app a
> `clone(CLONE_VM)` thread sharing `g_wm`) was **implemented, shipped, and then
> replaced**. It was fundamentally race-prone: the WM main thread, app threads,
> the keyboard thread, and the mouse IRQ all touched the same `wm_state_t` and
> the same shadow framebuffer with no synchronization. This produced a long tail
> of corruption/freeze bugs (g_wm overwritten on window close, framebuffer
> glitches on redraw, the terminal close button doing nothing).
>
> **Current model — one thread, one event loop, apps as callbacks:**
> Only the WM loop ever touches `wm_state_t` and the framebuffer, so the entire
> class of shared-state races is gone (no locks, no SPSC event queue, no
> `pthread_join` reaping). Each window registers non-blocking callbacks instead
> of running a thread:
> ```c
> typedef struct wm_window_ {
>     int id; rect_t bounds; char title[WM_TITLE_LEN];
>     canvas_t *backbuf; int focused, active, dirty, want_close;
>     void  *app_state;                       /* per-window heap state */
>     wm_on_event_fn   on_event;              /* key / WM_EV_CLOSE      */
>     wm_on_tick_fn    on_tick;               /* periodic, non-blocking */
>     wm_on_destroy_fn on_destroy;            /* free app_state+backbuf */
> } window_t;
> ```
> The loop each iteration: poll mouse (non-blocking) → poll keyboard
> (`read_nonblock`) → tick every window → destroy windows that set `want_close`
> → composite + taskbar + **cursor (drawn last, on top)** + flush.
>
> Anything that would block (the terminal reading its shell's stdout pipe) is
> done with a non-blocking poll inside `on_tick`. Apps are launched via
> `*_open(win)` entry points that install the callbacks. The terminal spawns its
> shell with `exec_redir(path, in_fd, out_fd)` (a new syscall) so the WM keeps
> reading the real keyboard instead of dup2-hijacking its own stdio.
>
> New supporting syscalls: `SYS_READ_NB` (35, non-blocking read) and
> `SYS_EXEC_REDIR` (36, exec with explicit child stdin/stdout). The kernel
> tracks the single graphics owner (`gfx_owner_pid`) to refuse a second WM and
> to route the owner's console output to serial only (no text flash on the
> composited desktop). See `user/wm/wm.h`, `user/wm/main.c`, and the
> "WM architecture" project memory for the full design.
>
> The historical threaded description below is kept for context but no longer
> reflects the code.

---

**Why it matters:** A window manager is the process that owns the screen. It
decides which window is in front, draws decorations, routes events to the right
app, and composites everything into the final frame before flushing. Getting the
app↔WM relationship right is the foundation for everything in 14.3 and 14.4.

**(SUPERSEDED) The IPC problem and its original solution**

A naive design gives each app its own process (via `fork`) and uses pipes or
shared memory to exchange pixels and events with the WM. That requires
`SYS_MMAP` or a kernel shared-memory primitive that Quilon does not have.

The original solution for Quilon was: **every graphical app runs as a
`clone(CLONE_VM)` thread of the WM**. Because `CLONE_VM` shares the entire
virtual address space, any pointer allocated in the WM heap is directly readable
and writable by the app thread — no IPC, no copies, no new syscalls. *(This is
the design that was later replaced — see the architecture-update note above.)*

```
WM process (owns address space)
│
├── WM main thread    ← composites + flushes + routes events
├── gterm thread      ← clone(CLONE_VM), draws into win->canvas directly
├── clock thread      ← clone(CLONE_VM), draws into win->canvas directly
└── filebr thread     ← clone(CLONE_VM), draws into win->canvas directly
```

Each app still `fork()`s child *processes* when it needs to run a separate
program (e.g. gterm forks `shell.elf`). The `clone` is only for the graphical
layer between the WM and its UI threads.

**Shared data structures (`user/wm/wm.h`)**

```c
/* SPSC ring buffer: WM writes events, app thread reads them.
 * Single-producer / single-consumer — no lock needed.             */
#define WM_EVQUEUE_DEPTH  64

typedef enum { WM_EV_KEY = 1, WM_EV_MOUSE, WM_EV_CLOSE } wm_ev_type_t;

typedef struct {
    wm_ev_type_t type;
    char         ascii;      /* WM_EV_KEY: ASCII char, 0 if non-printable */
    int          mx, my;     /* WM_EV_MOUSE: screen coords */
    int          buttons;    /* WM_EV_MOUSE: button mask */
} wm_event_t;

typedef struct {
    volatile int head;                   /* WM increments after writing  */
    volatile int tail;                   /* app increments after reading */
    wm_event_t   buf[WM_EVQUEUE_DEPTH];
} wm_evqueue_t;

#define WM_MAX_WINDOWS  8
#define WM_TITLE_LEN   32
#define TITLEBAR_H     20

typedef struct {
    int           id;
    rect_t        bounds;        /* content area (excludes title bar)    */
    char          title[WM_TITLE_LEN];
    canvas_t     *canvas;        /* app draws here; WM blits to screen   */
    wm_evqueue_t  events;        /* WM → app keyboard / mouse events     */
    volatile int  dirty;         /* app sets 1; WM clears after blit     */
    int           active;        /* 1 once the app has opened it         */
    int           focused;       /* 1 for the top-most interactive window */
} window_t;
```

**App entry-point convention**

```c
/* Every graphical app is a function with this signature. */
typedef void (*wm_app_fn_t)(window_t *win);

/* WM launches an app by:
 *   1. Allocating a window_t and its canvas in the WM heap.
 *   2. Calling clone(CLONE_VM|CLONE_FS|CLONE_FILES, trampoline, win).
 *   3. The trampoline calls app_fn(win) then exits.                   */
```

**Event-queue helpers (pure inline — testable on host)**

```c
/* Push an event from the WM thread (producer side). */
static inline void wm_evqueue_push(wm_evqueue_t *q, wm_event_t ev)
{
    int next = (q->head + 1) % WM_EVQUEUE_DEPTH;
    if (next == q->tail) return;   /* drop on overflow */
    q->buf[q->head] = ev;
    q->head = next;
}

/* Poll an event from the app thread (consumer side). Returns 1 on success. */
static inline int wm_evqueue_poll(wm_evqueue_t *q, wm_event_t *out)
{
    if (q->tail == q->head) return 0;
    *out = q->buf[q->tail];
    q->tail = (q->tail + 1) % WM_EVQUEUE_DEPTH;
    return 1;
}
```

**Compositor (`user/wm/wm_compositor.c`)**

```c
/* Painter's algorithm: background → windows back-to-front → cursor. */
void wm_composite(canvas_t *screen, window_t *windows, int n,
                  int cur_x, int cur_y)
{
    /* 1. Desktop background. */
    gfx_fill(screen, GFX_DARK_BLUE);

    /* 2. Windows back-to-front. */
    for (int i = 0; i < n; i++) {
        window_t *w = &windows[i];
        if (!w->active) continue;

        rect_t tbar = { w->bounds.x, w->bounds.y - TITLEBAR_H,
                        w->bounds.w, TITLEBAR_H };
        color_t tc = w->focused ? GFX_RGB(80,80,200) : GFX_RGB(60,60,60);

        gfx_fill_rect(screen, tbar, tc);
        gfx_draw_text(screen, tbar.x + 4, tbar.y + 2, w->title,
                      GFX_WHITE, tc);

        /* Close button. */
        rect_t x_btn = { tbar.x + tbar.w - 18, tbar.y + 2, 16, 16 };
        gfx_fill_rect(screen, x_btn, GFX_RED);

        /* Blit the app canvas, clipped to its content rect. */
        if (w->canvas) {
            gfx_set_clip(screen, w->bounds);
            gfx_blit(screen, w->bounds.x, w->bounds.y, w->canvas,
                     (rect_t){0, 0, w->canvas->w, w->canvas->h});
            gfx_clear_clip(screen);
        }

        gfx_draw_rect(screen,
                      (rect_t){tbar.x, tbar.y, tbar.w, tbar.h + w->bounds.h},
                      GFX_RGB(100,100,100));

        w->dirty = 0;
    }

    /* 3. Mouse cursor (8×8 filled triangle sprite). */
    wm_draw_cursor(screen, cur_x, cur_y);
}
```

**WM main loop and desktop (`user/wm/main.c`)**

The WM is both compositor and desktop launcher. It draws a taskbar at the
bottom of the screen with one button per registered app. Clicking a button
calls `clone(CLONE_VM|CLONE_FS|CLONE_FILES, app_trampoline, win)`.

```c
/* Taskbar layout constants. */
#define TASKBAR_H     28
#define TASKBAR_BTN_W 100

/* Built-in app table. */
typedef struct { const char *label; wm_app_fn_t fn; } app_entry_t;
extern void gterm_app(window_t *);
extern void clock_app(window_t *);
extern void filebr_app(window_t *);

static const app_entry_t g_apps[] = {
    { "Terminal", gterm_app  },
    { "Clock",    clock_app  },
    { "Files",    filebr_app },
};

/* --- main ---------------------------------------------------------------- */
int main(void)
{
    gfx_info_t info;
    canvas_t *screen = gfx_screen_init(&info);

    /* Initialise global WM state. */
    wm_state_t wm;
    wm_init(&wm, screen, (int)info.width, (int)info.height - TASKBAR_H);

    mouse_event_t prev_mouse = {0};

    while (1) {
        /* --- Mouse -------------------------------------------------------- */
        mouse_event_t me;
        mouse_read(&me);
        wm_handle_mouse(&wm, me.x, me.y, me.buttons, prev_mouse.buttons);
        prev_mouse = me;

        /* Check taskbar clicks: launch apps. */
        if ((me.buttons & MOUSE_BTN_LEFT) &&
            !(prev_mouse.buttons & MOUSE_BTN_LEFT)) {
            int ty = (int)info.height - TASKBAR_H;
            if (me.y >= ty) {
                int idx = me.x / TASKBAR_BTN_W;
                if (idx >= 0 && idx < (int)(sizeof g_apps / sizeof g_apps[0]))
                    wm_launch_app(&wm, &g_apps[idx]);
            }
        }

        /* --- Keyboard ----------------------------------------------------- */
        /* poll keyboard; route to focused window's event queue */
        char c;
        if (kbd_poll(&c)) {                    /* non-blocking keyboard read */
            wm_event_t ev = { .type = WM_EV_KEY, .ascii = c };
            window_t *fw = wm_focused_window(&wm);
            if (fw) wm_evqueue_push(&fw->events, ev);
        }

        /* --- Composite + taskbar ------------------------------------------ */
        wm_composite(screen, wm.windows, wm.num_windows,
                     me.x, me.y);
        wm_draw_taskbar(screen, g_apps, sizeof g_apps / sizeof g_apps[0],
                        (int)info.width, (int)info.height, TASKBAR_H);
        gfx_flush();
    }
}
```

**Launching an app**

```c
void wm_launch_app(wm_state_t *wm, const app_entry_t *app)
{
    /* Find a free window slot and set it up. */
    window_t *win = wm_alloc_window(wm, app->label, 640, 400, 80, 40);
    if (!win) return;

    /* Allocate canvas in WM heap — visible to the clone'd app immediately. */
    win->canvas = canvas_create(win->bounds.w, win->bounds.h);

    /* clone(CLONE_VM|CLONE_FS|CLONE_FILES): shares WM address space. */
    pthread_t tid;
    pthread_create(&tid, NULL, (void *(*)(void *))app->fn, win);
}
```

Because the clone shares the WM's address space, `win->canvas->pixels` is the
same physical memory in both threads. The app thread writes pixels; the WM
thread reads and blits them. The `volatile int dirty` flag signals a repaint is
needed without any syscall.

> **Learning note:** This is exactly how early windowing systems worked. The
> original Mac OS (1984) ran the entire application and the OS in the same
> address space with no memory protection. The Win16 cooperative model
> was similar. The `clone(CLONE_VM)` approach gives Quilon a real scheduler
> (preemptive) while keeping the graphical data sharing trivially simple.
> Memory-isolated windowing (like X11 or Wayland) requires a display server
> protocol, shared-memory extensions, and a socket layer — all of which are
> unnecessary complexity for a hobby OS at this stage.

---

### 14.3 startx and the Graphical Terminal

#### 14.3.1 startx — switching from text to graphical mode

**Why it matters:** The user boots into the existing ring-3 text shell. Typing
`startx` should hand the screen over to the WM the same way Linux hands off to
X11. This is a one-liner in the shell, but naming it explicitly in the roadmap
makes the boot flow obvious.

Add `startx` as a command in both shells:

```c
/* user/shell/main.c and kernel/kernel/shell.c */
} else if (strcmp(cmd, "startx") == 0) {
    int pid = exec("/wm.elf");
    if (pid < 0) { puts("startx: cannot exec /wm.elf\n"); continue; }
    wait(pid, NULL);
```

When the WM exits (the user shuts down the graphical session), the text shell
resumes. The VBE framebuffer is always active; there is no mode switch — the
WM simply takes ownership of the shadow buffer and starts compositing.

#### 14.3.2 Graphical Terminal as a WM app

gterm becomes a `wm_app_fn_t` rather than a standalone program. It receives a
`window_t *` already containing a canvas; it does not open a window itself.

```c
/* user/gterm/gterm_app.c */

#include <unistd.h>
#include <pthread.h>
#include <gfx.h>
#include "gterm.h"
#include "../wm/wm.h"

/* ── Keyboard-forwarding thread ─────────────────────────────────────────── */

typedef struct { wm_evqueue_t *ev; int shell_in; } kbd_arg_t;

static void *kbd_thread(void *arg)
{
    kbd_arg_t *a = (kbd_arg_t *)arg;
    wm_event_t ev;
    while (1) {
        /* Spin-poll the event queue; yield if empty to avoid burning CPU. */
        if (wm_evqueue_poll(a->ev, &ev)) {
            if (ev.type == WM_EV_KEY && ev.ascii)
                write(a->shell_in, &ev.ascii, 1);
            else if (ev.type == WM_EV_CLOSE)
                break;
        }
    }
    return NULL;
}

/* ── Render ─────────────────────────────────────────────────────────────── */

static void gterm_render(gterm_t *t, canvas_t *c, int cur_visible)
{
    int r, c2;
    gfx_fill(c, (color_t)GT_DEFAULT_BG);
    for (r = 0; r < GTERM_ROWS; r++)
    for (c2 = 0; c2 < GTERM_COLS; c2++) {
        gterm_cell_t *cell = &t->cells[r][c2];
        char str[2] = { cell->ch ? cell->ch : ' ', '\0' };
        gfx_draw_text(c, c2 * GTERM_CELL_W, r * GTERM_CELL_H,
                      str, (color_t)cell->fg, (color_t)cell->bg);
    }
    if (cur_visible) {
        gterm_cell_t *cc = &t->cells[t->cursor_row][t->cursor_col];
        rect_t cr = { t->cursor_col * GTERM_CELL_W,
                      t->cursor_row * GTERM_CELL_H,
                      GTERM_CELL_W, GTERM_CELL_H };
        char str[2] = { cc->ch ? cc->ch : ' ', '\0' };
        gfx_fill_rect(c, cr, (color_t)cc->fg);
        gfx_draw_text(c, cr.x, cr.y, str,
                      (color_t)cc->bg, (color_t)cc->fg);
    }
}

/* ── App entry point ─────────────────────────────────────────────────────── */

void gterm_app(window_t *win)
{
    /* 1. Pipes: parent (gterm) ↔ child (shell). */
    int to_shell[2], from_shell[2];
    if (pipe(to_shell) || pipe(from_shell)) { win->active = 0; return; }

    /* 2. Fork the shell as a separate process. */
    int child = fork();
    if (child == 0) {
        dup2(to_shell[0],   STDIN_FILENO);
        dup2(from_shell[1], STDOUT_FILENO);
        dup2(from_shell[1], STDERR_FILENO);
        close(to_shell[0]);  close(to_shell[1]);
        close(from_shell[0]); close(from_shell[1]);
        int pid = exec("/shell.elf");
        if (pid >= 0) wait(pid, NULL);
        exit(0);
    }
    close(to_shell[0]); close(from_shell[1]);

    /* 3. Keyboard-forwarding thread: evqueue → shell stdin. */
    kbd_arg_t karg = { &win->events, to_shell[1] };
    pthread_t kbd_tid;
    pthread_create(&kbd_tid, NULL, kbd_thread, &karg);

    /* 4. Main loop: shell stdout → parse → render → mark dirty. */
    gterm_t term;
    gterm_init(&term);
    gterm_render(&term, win->canvas, 1);
    win->dirty = 1;

    char ibuf[256];
    while (1) {
        int n = read(from_shell[0], ibuf, sizeof(ibuf));
        if (n > 0) {
            gterm_write(&term, ibuf, n);
            gterm_render(&term, win->canvas, 1);
            win->dirty = 1;
        } else if (n == 0) {
            gterm_write(&term, "\r\n[shell exited]\r\n", 18);
            gterm_render(&term, win->canvas, 0);
            win->dirty = 1;
            break;
        }
    }

    close(to_shell[1]);
    close(from_shell[0]);
    win->active = 0;
}
```

The `kbd_thread` is a second `clone(CLONE_VM)` of the gterm thread (via
`pthread_create`). It spin-polls `win->events` — the same struct the WM thread
writes into — without any syscall overhead. On a single-CPU QEMU setup the WM
thread and kbd_thread are time-sliced by the scheduler; on SMP they run in
parallel.

> **Learning note:** The three-thread structure here (WM thread, gterm render
> thread, kbd thread) mirrors how real terminal emulators work. alacritty has a
> "renderer thread" and an "event thread"; kitty has a similar split. The
> important invariant is that only one thread writes to the canvas at a time —
> here enforced by structure (only the gterm render thread ever calls
> `gterm_render()`), not by a lock.

---

### 14.4 Simple GUI Apps

Each app is a `wm_app_fn_t` — a plain C function that receives its `window_t *`
and runs until it exits. The WM launches it via `pthread_create` (which uses
`clone(CLONE_VM)` internally), so the app's canvas is directly in WM memory.

**`user/wm/apps.h`** declares all built-in apps:

```c
void gterm_app(window_t *win);   /* graphical terminal  */
void clock_app(window_t *win);   /* digital clock       */
void filebr_app(window_t *win);  /* file browser        */
void textview_app(window_t *win); /* text file viewer   */
```

---

#### Clock

```c
/* user/clock/clock_app.c */
void clock_app(window_t *win)
{
    while (win->active) {
        uint32_t t  = getticks();             /* seconds since boot */
        int h = (t / 3600) % 24, m = (t / 60) % 60, s = t % 60;
        char str[9];
        snprintf(str, sizeof str, "%02d:%02d:%02d", h, m, s);

        gfx_fill(win->canvas, GFX_RGB(10, 10, 10));
        gfx_draw_text(win->canvas, 40, 12, str,
                      GFX_RGB(0, 255, 128), GFX_RGB(10, 10, 10));
        win->dirty = 1;
        sleep(1);

        /* Exit if the WM sent a close event. */
        wm_event_t ev;
        if (wm_evqueue_poll(&win->events, &ev) && ev.type == WM_EV_CLOSE)
            break;
    }
}
```

---

#### File Browser

A scrollable directory listing. Arrow keys and Enter navigate; clicking a file
launches `textview_app` on a new window.

```c
/* user/filebr/filebr_app.c — key structs */
#define LIST_ITEM_H  16
#define MAX_ENTRIES  64

typedef struct {
    char name[256];
    int  is_dir;
} fb_entry_t;

static void fb_render(canvas_t *c, fb_entry_t *entries, int n,
                      int scroll, int selected)
{
    gfx_fill(c, GFX_RGB(20, 20, 20));
    for (int i = scroll; i < n; i++) {
        int y = (i - scroll) * LIST_ITEM_H;
        if (y + LIST_ITEM_H > c->h) break;
        color_t bg = (i == selected) ? GFX_RGB(50,50,150) : GFX_RGB(20,20,20);
        gfx_fill_rect(c, (rect_t){0, y, c->w, LIST_ITEM_H}, bg);
        color_t fg = entries[i].is_dir ? GFX_RGB(100,180,255)
                                       : GFX_RGB(220,220,220);
        gfx_draw_text(c, 4, y + 1, entries[i].name, fg, bg);
    }
}

void filebr_app(window_t *win)
{
    fb_entry_t entries[MAX_ENTRIES];
    int n = 0, scroll = 0, selected = 0;
    char cwd[256] = "/";

    /* Initial directory read. */
    n = fb_read_dir(cwd, entries, MAX_ENTRIES);
    fb_render(win->canvas, entries, n, scroll, selected);
    win->dirty = 1;

    wm_event_t ev;
    while (win->active) {
        if (!wm_evqueue_poll(&win->events, &ev)) continue;
        if (ev.type == WM_EV_CLOSE) break;
        if (ev.type == WM_EV_KEY) {
            if (ev.ascii == '\n' && n > 0) {
                /* Navigate into directory or open file. */
                if (entries[selected].is_dir) {
                    /* chdir + re-read */
                    n = fb_chdir_and_read(cwd, entries[selected].name,
                                          entries, MAX_ENTRIES);
                    scroll = selected = 0;
                }
            } else if (ev.ascii == 'j' || ev.ascii == '\x1b') {
                /* down / up handled via arrow key ascii codes */
                if (selected < n - 1) selected++;
            } else if (ev.ascii == 'k') {
                if (selected > 0) selected--;
            }
            fb_render(win->canvas, entries, n, scroll, selected);
            win->dirty = 1;
        }
    }
}
```

---

#### Text Viewer

```c
/* user/textview/textview_app.c */
void textview_app(window_t *win)
{
    /* Caller passes the path via win->title for simplicity. */
    char *lines[1024];
    int nlines = tv_read_file(win->title, lines, 1024);
    int scroll = 0;
    int rows   = win->canvas->h / GFX_CHAR_H;

    tv_render(win->canvas, lines, nlines, scroll, rows);
    win->dirty = 1;

    wm_event_t ev;
    while (win->active) {
        if (!wm_evqueue_poll(&win->events, &ev)) continue;
        if (ev.type == WM_EV_CLOSE) break;
        if (ev.type == WM_EV_KEY) {
            if ((ev.ascii == 'd') && scroll + rows < nlines) scroll++;
            if ((ev.ascii == 'u') && scroll > 0)             scroll--;
            tv_render(win->canvas, lines, nlines, scroll, rows);
            win->dirty = 1;
        }
    }
    tv_free_lines(lines, nlines);
}
```

---

**Final `user/` layout after section 14:**

```
user/
├── libgfx/          ← 2D graphics library (unchanged from 14.1)
├── wm/
│   ├── wm.h         ← window_t, wm_evqueue_t, wm_state_t (shared types)
│   ├── wm_state.c   ← wm_init, wm_alloc_window, wm_launch_app, wm_focused_window
│   ├── wm_compositor.c ← wm_composite, wm_draw_cursor, wm_draw_taskbar
│   └── main.c       ← event loop; desktop launcher
├── gterm/
│   ├── gterm.h      ← ANSI parser + cell grid (unchanged)
│   └── gterm_app.c  ← gterm_app() — WM app function
├── clock/
│   └── clock_app.c
├── filebr/
│   └── filebr_app.c
├── textview/
│   └── textview_app.c
└── shell/           ← existing ring-3 text shell; gains `startx` command
```

All app `.c` files are compiled into `wm.elf` — there is no separate `gterm.elf`,
`clock.elf`, etc. The WM is a single executable that contains all graphical
apps as statically linked functions. This keeps deployment simple (one ELF on
the FAT16 image) and eliminates exec-time linking.

> **Learning note:** Bundling apps into the compositor is how early graphical
> systems worked — the original Macintosh Finder *was* the shell, and apps were
> separate files only because the hardware had enough storage. For a hobby OS
> with a 1 MB FAT16 image, a single WM+apps ELF is the practical choice. The
> architecture still keeps each app as an isolated function with its own thread
> and its own canvas, so splitting them out later is mechanical.

---


## 15. BSD Socket API for User Space

> **▶ THIS IS THE NEXT MILESTONE (as of 2026-06).** Sections 11–14 are done; the
> desktop is stable. The kernel TCP/IP stack (`net.c`) exists but is not exposed
> to user space — that is exactly what this section adds. Note the syscall
> numbers below (43+) leave room for the recently-added 35 (`SYS_READ_NB`) and
> 36 (`SYS_EXEC_REDIR`).

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
