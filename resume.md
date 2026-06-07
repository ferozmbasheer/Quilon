# Quilon — Resume / Handoff

Read this first if you're picking up work on Quilon after a context reset. It is
the practical "where things stand and how not to waste time" companion to
[ROADMAP3.md](ROADMAP3.md) (the plan) and [ARCHITECTURE.md](ARCHITECTURE.md)
(the system map). Last updated 2026-06.

## What Quilon is

An educational i386 (x86-32) OS: higher-half kernel, per-process paging + demand
paging + CoW fork, preemptive round-robin scheduler, SMP/APIC, FAT16 + VFS +
pipes, a ring-3 libc and shell, a TCP/IP stack (kernel-only so far), a VBE
framebuffer, and a graphical desktop (WM + compositor + terminal/clock/files/
textview apps).

## Current state (done)

Sections 11–14 of ROADMAP3 are complete and the **graphical desktop is stable**.
`startx` from the shell brings up the WM; the taskbar launches Terminal, Clock,
Files; windows drag/raise/focus/close; the terminal runs a nested shell.

Getting here took a long stabilization arc after 14.4. The headline outcome:
**the WM was rearchitected from CLONE_VM threads to a single-threaded event
loop**, and a series of kernel concurrency/memory bugs were fixed. Don't
reintroduce the threaded model.

## Next milestone

**Section 15 — BSD Socket API for user space (DONE: 15.1 sockets, 15.2 DNS,
15.3 wget).** Next up is **ROADMAP3 §16 — more device drivers.** The 15.x
details below are kept as a record of the networking work.

### 15.1 sockets — DONE (2026-06)
The BSD socket syscalls are implemented and wired through the VFS fd table:
- **Syscalls 44–50** (`SYS_SOCKET/BIND/CONNECT/SEND/RECV/LISTEN/ACCEPT`) in
  `syscall.c` (43 was already taken by `SYS_PS`). All user pointers go through
  `uap_ok`.
- **VFS** (`vfs.c`/`vfs.h`): `vfs_node_t` gained `is_socket`/`sock_type`/
  `sock_lport`/`sock_rport`/`sock_rip`; `vfs_socket()` + `vfs_node_for_fd()`;
  `read`/`write`/`close`/`readable` route socket fds to the net layer (so plain
  `read()/write()/close()` work on a socket too).
- **net.c**: added `net_tcp_connect()` (active open: ARP → SYN/SYN-ACK/ACK,
  new `TCP_STATE_SYN_SENT`, RST→refused) and `net_tcp_readable()`. The TCP
  stack is still **single-connection** (global `g_tcp`), so all socket fds alias
  it — fine for the educational demo.
- **libc**: `user/libc/include/sys/socket.h`, asm wrappers in `syscall.S`,
  `inet_aton()` in `stdlib.c`.
- **Demo**: `http <ip> [port] [path]` command in BOTH shells (dual-shell rule).
  User shell uses the socket syscalls; kernel shell calls `net_tcp_*` directly.
  Port is space-separated, NOT `host:port`, because **Quilon's keyboard driver
  has no shift** (`keyboard.c` is a single unshifted map — no `:`/uppercase).

Verified end-to-end (`make test` all suites, `make smoke`, plus a headless
QEMU run driving the shell over the HMP monitor): the ring-3 shell did a full
`http 1.1.1.1 80 /` to the **real internet** via SLIRP NAT — `socket → connect`
(SYN/SYN-ACK/ACK) → `send` → `recv` → `close` — and printed a live
`HTTP/1.1 301` from Cloudflare (381 bytes). Confirmed on the wire with a
`filter-dump` pcap.

That live test exposed **two real bugs** the listen-only/never-against-a-real-
peer stack had hidden (both fixed):
1. **TCP checksum was byte-swapped** — `send_tcp_segment` stored the host-order
   `net_transport_checksum()` result without `net_htons()` (the IP/ICMP paths
   wrap it). Real peers dropped every SYN. The host test only checked the
   checksum *value*, not how the caller stored it.
2. **`net_tcp_recv` slept with no waker** — it `waitq_sleep()`-ed between polls,
   but networking is *polled, not IRQ-driven*, so the one-RTT-away response was
   missed (0 bytes). Now busy-polls (bounded) like `net_tcp_connect`/`net_ping`.

Pre-existing single-connection limits remain (one `g_tcp`, one 512-byte rx
segment buffered at a time, PSH required to buffer data) — fine for the demo.
(Headless `ping`/ICMP to public IPs is rate-limited and may time out; TCP works
regardless.)

### 15.2 DNS resolver — DONE (2026-06)
Stub resolver over UDP/53, all wire logic in pure-C host-testable helpers in
`net.h` (`dns_encode_qname`, `dns_build_query`, `dns_skip_name`,
`dns_parse_response` — handles name compression + CNAME-before-A).
- **net.c**: `net_dns_lookup(host, server, *out_ip)` (busy-polls like the TCP
  paths — networking is polled, not IRQ-driven) and `net_dns_server()`. The
  DNS server is captured from **DHCP option 6** (new `case 6` in
  `parse_dhcp_options`, stored in `g_dns`); `net_dns_lookup` falls back to that,
  then to `8.8.8.8`. Reuses the existing `g_udp_rx` slot + `net_udp_send`.
- **syscall**: `SYS_DNS_RESOLVE 51` (`dns_resolve(host, uint32_t *out_ip)`),
  validated with `uap_str_ok` + `copyout`. libc: `dns_resolve()` stub in
  `syscall.S`, declared in `sys/socket.h`.
- **tests**: `tests/test_dns.c` (32 assertions, wired into `tests/Makefile`).
- **Demo**: `nslookup <host>` in BOTH shells; `http`/`http`-builtin now accept a
  **hostname** (DNS fallback when `inet_aton`/`parse_ipv4` fails).

### 15.3 wget — DONE (2026-06)
`user/wget/main.c` → `WGET.ELF`. **Quilon has no argv** (crt0 pushes none, exec
takes only a path), so wget reads the target **interactively from stdin**
(`host:` / `path:` prompts with a tiny echoing readline) rather than argv. A
`wget` command in BOTH shells execs `/wget.elf`.

Verified live (headless QEMU over the HMP monitor, SLIRP NAT): `dhcp` →
`nslookup example.com` returned `104.20.23.154`, and `wget` (host
`example.com`, path `/`) resolved via DNS, connected, and printed a live
`HTTP/1.1 200 OK` + the real Example Domain HTML from Cloudflare. Driver:
`/tmp/dns_live_test.py` (sendkey + serial capture; transient, not committed).

**Section 15 is complete.** Next is ROADMAP3 §16 (more device drivers).

## Build & test (and the #1 gotcha)

```sh
make        # build kernel + libc
make test   # host unit tests (host gcc only) — run on every change
make run    # boot under QEMU (GUI)
make smoke  # headless boot smoke test
```

**GOTCHA that wasted hours:** `make disk` (and `./create_disk.sh`) rebuild only
the **userspace** `disk.img` — NOT the kernel ISO. To test a **kernel** change
you MUST run `./iso.sh` (top-level `make`/`make run` chain it). After a bare
`make disk` you are booting a stale kernel. If a kernel change "has no effect,"
this is almost certainly why.

Toolchain: `i686-elf` cross gcc, GRUB, QEMU, python3 (see BUILD.md).

## Architecture you must know before changing things

### WM = single-threaded event loop (`user/wm/`)
- One process, one loop. **Only the loop touches `wm_state_t` and the
  framebuffer** — that's what makes it race-free. No locks, no event queue, no
  `pthread_join`.
- Apps are non-blocking callbacks on each `window_t`, NOT threads:
  `on_event(win,ev)`, `on_tick(win)`, `on_destroy(win)`; per-window heap state in
  `win->app_state`; closing sets `win->want_close` and the loop destroys it.
- Launch entry points: `gterm_open` / `clock_open` / `filebr_open` /
  `textview_open` (each installs callbacks + state + draws first frame).
- Loop order each iteration: poll mouse → poll keyboard (`read_nonblock`) → tick
  windows → destroy `want_close` windows → composite + taskbar + **cursor last**
  + `gfx_flush`. (Cursor is drawn after the taskbar so it stays on top of it.)
- **Apps must never block.** The terminal drains its shell's stdout pipe with
  `read_nonblock` inside `on_tick`.
- `tests/test_wm.c` exercises the pure-logic helpers in `wm.h` on the host —
  keep it green when touching WM logic.

### Kernel memory: >4 MiB access
Only the first 4 MiB is identity-mapped. Page tables/dirs stay in low memory;
**data pages** (heap/ELF/stack/CoW) come from `pmm_alloc_data_page()` (above
4 MiB) and are touched via `kmap()`/`kunmap()` (paging.c). Never dereference an
arbitrary physical address directly.

### Kernel concurrency rules (learned the hard way)
- A lock/state shared between **thread and interrupt context** must disable
  interrupts while held: use `spinlock_acquire_irqsave` (spinlock.h). Applied to
  `pmm_lock`, `tty_lock`, `kmap_lock`, and the waitq.
- IRQ stubs (`boot.S`) load the kernel data segment (a missing `mov $0x10,%ds`
  caused clone-thread `iret` GPFs).
- ATA PIO is serialized with a **plain** spinlock (slow, never called from IRQ —
  irqsave there would starve the timer).
- fork/clone wrap PCB-publish + frame-build in `irq_save`/`irq_restore`.

### Syscalls (`kernel/kernel/syscall.c`)
Single dispatch switch. Current high numbers: 26–34 (stat/mkdir/chdir/getcwd/
lseek/rename/clone/mouse_read/dup2), **35 `SYS_READ_NB`** (non-blocking read),
**36 `SYS_EXEC_REDIR`** (exec with explicit child stdin/stdout, used by the
terminal so the WM keeps its own keyboard), 40–42 gfx. Section 15 should use 43+.
- **Validate user pointers** with `uap_ok` / `uap_str_ok` / `copyout` (they check
  against the process VMA table). Do not deref raw user pointers.
- `gfx_owner_pid` tracks the single graphics owner: refuses a second WM, and
  while set the kernel suppresses its VBE text console + mouse cursor
  (`vbe_set_graphics_mode`) so kernel output doesn't flash on the desktop.
- `owns_std_fds` (PCB): only a process *handed* pipe fds via `exec_redir` closes
  them on exit; plain-exec children that merely inherited them must not (else a
  grandchild's exit closes the parent shell's pipes — that bug hung the
  terminal).

### Dual shell
Two shells exist: the in-kernel debug shell (`kernel/kernel/shell.c`) and the
ring-3 shell (`user/shell/main.c`). **Shell command changes must be made in
both.**

## How to verify GUI changes without a human

Mouse injection via the QEMU monitor (relative PS/2) is imprecise — clicks often
miss. What works:
- Drive `startx` and keystrokes via the HMP `sendkey` over a `-monitor unix:`
  socket; capture `-serial file:`.
- `screendump file.ppm` then analyze pixels in Python (count colors / regions).
  Desktop is `(96,138,175)`, taskbar `(191,191,191)`.
- For deterministic app open/close, temporarily add a small `#ifdef`-guarded
  autolaunch/autoclose hook in `user/wm/main.c` (and remember to remove it).
- `make smoke` with `SMOKE_SENTINEL='launching SHELL.ELF'` confirms boot.
There is no in-VM integration test framework; host unit tests + screendump
analysis are the tools.

## Working agreement

- The maintainer **commits manually** — make changes, build, verify, then pause
  and summarize for review. Do not run `git commit` yourself.
- Keep `make test` green and the build warning-free.
- Project memory lives in `.claude/projects/.../memory/` (indexed by `MEMORY.md`):
  see `project_wm_architecture.md` and `project_thread_concurrency.md` for the
  full design rationale and the specific bugs already fixed — read them before
  touching the WM or kernel locking.
