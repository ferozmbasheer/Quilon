# Quilon — Resume / Handoff

Read this first if you're picking up work on Quilon after a context reset. It is
the practical "where things stand and how not to waste time" companion to
[ROADMAP3.md](ROADMAP3.md) (the plan) and [ARCHITECTURE.md](ARCHITECTURE.md)
(the system map). Last updated 2026-08.

## What Quilon is

An educational i386 (x86-32) OS: higher-half kernel, per-process paging + demand
paging + CoW fork, preemptive round-robin scheduler, SMP/APIC, a VFS with
**ext2 (read-only) and FAT16** drivers + pipes, a ring-3 libc and shell, a
TCP/IP stack **exposed to user space through BSD sockets + DNS**, a VBE
framebuffer, and a graphical desktop (WM + compositor + terminal/clock/files/
textview apps).

## Current state (done)

Sections 11–16 of ROADMAP3 are complete and the **graphical desktop is stable**.
`startx` from the shell brings up the WM; the taskbar launches Terminal, Clock,
Files; windows drag/raise/focus/close; the terminal runs a nested shell.
The ring-3 shell can fetch a real web page (`wget`, `http`, `nslookup`), and the
OS boots and runs off either an ext2 or a FAT16 disk image.

Getting here took a long stabilization arc after 14.4. The headline outcome:
**the WM was rearchitected from CLONE_VM threads to a single-threaded event
loop**, and a series of kernel concurrency/memory bugs were fixed. Don't
reintroduce the threaded model.

## Stability review (2026-11) — read REVIEW.md

Before starting §17, a review ([REVIEW.md](REVIEW.md)) found and fixed real
kernel bugs: ring-0 page faults on user buffers panicked, CR0.WP was off,
exited processes leaked their address space, every exec'd process could
read/write kernel memory, the VFS had no lock, wait() could hang, and closed
terminals left shells spinning. Each fix has host tests plus an in-OS check:
**`make ktest`** (user/ktest, typed into the VM via the QEMU monitor) and
**`make wmtest`** (mouse-driven desktop test). Run both on kernel changes, on
both images (`SMOKE_DISK=ext2.img make ktest`). Still open in REVIEW.md:
per-process fd tables + cwd, errno, boot-demo gating, single TCP connection.

## Next milestone

**Section 16 — More device drivers: 16.1 ATA Bus Master DMA and 16.2 ext2 are
both DONE (ext2 read-only).** Next up is **ROADMAP3 §17 — porting** (17.1 Lua,
17.2 a text editor, 17.3 TinyCC), or ext2 **write** support if the disk should
become the primary read-write filesystem. The 15.x and 16.x details below are
kept as a record.

### 16.2 ext2 filesystem — DONE, read-only (2026-08)
Quilon boots from and runs off an ext2 disk. The kernel probes ext2 first and
falls back to FAT16, so both images work unchanged.
- **`kernel/kernel/ext2.c` + `include/kernel/ext2.h`**: superblock parse
  (magic 0xEF53 at byte 1024), block group descriptors, `ext2_read_inode`,
  `ext2_bmap` (direct + singly + **doubly** indirect; a zero pointer is a hole
  and reads as zeroes), `ext2_read_data`, and `ext2_lookup_path` — **real
  directory traversal**, which FAT16 never had. Rev 0 and rev 1 (128- or
  256-byte inodes); block sizes 1/2/4 KiB. Triply indirect is rejected.
- **Case-insensitive fallback** in `ext2_dir_find`: ext2 is case-sensitive and
  an exact match always wins, but if a directory has no exact match a
  case-insensitive one is accepted. This is *required*, not a convenience —
  Quilon's keyboard has no shift, so `cat readme.txt` is the only way to reach
  README.TXT. See the `ci_name_matches` comment.
- **Read-only**: write/create/mkdir/remove/rename are NULL in `ext2_vfs_ops`,
  so the VFS reports them unsupported rather than silently failing.
- **VFS path resolution (new, affects every driver)**: `vfs_normalize()` in
  vfs.c now resolves each path against the CWD and folds `.`/`..`/duplicate
  slashes, and `vfs_chdir` stores a normalised absolute CWD. Without it
  `cd sub` + `cat file.txt` could not work, because vfs_open passed the raw
  path straight through. All drivers already stripped a leading `/`, so FAT16
  and the initrd were unaffected. `pwd` now prints `/sub`, not `sub`.
- **`VFS_NAME_MAX` 12 → 63** (and `DIRENT_NAME_MAX` to match) so long ext2
  names survive readdir. **This is a user-space ABI change**: the two must stay
  equal or `ls` prints garbage sizes. Nothing stores arrays of dirents, so the
  cost is a bigger stack local.
- **Demo (§16 convention)**: a `=== Section 16.2: ext2 Filesystem ===` boot
  block reports the mounted fs + geometry, then proves the two ext2-only
  capabilities (nested `/SUB/NESTED.TXT` lookup, a >8.3 filename). Both probes
  are skipped silently on a FAT16 boot, which just prints its own geometry.
- **`fs` command in BOTH shells** (dual-shell rule) backed by new
  **`SYS_FSINFO 52`** + an optional `fsinfo` op in `vfs_ops_t`, implemented for
  ext2, FAT16 and the initrd. `vfs_fsinfo_t`/`fsinfo_t` are another
  kernel↔user ABI pair that must stay in sync. FAT16 gained a `total_sectors`
  field (from the BPB) so it can report a volume size too.
- **Build**: `make ext2disk` writes `ext2.img` via `tools/make_ext2_disk.py`
  (needs host `mke2fs`); `make disk` still writes the FAT16 `disk.img`.
  `SMOKE_DISK=ext2.img make smoke` boots the ext2 one.
- **user/Makefile, libc, libgfx now use `-MMD -MP`** header dependencies. They
  had none: the `VFS_NAME_MAX` change did not rebuild the user programs, and
  the stale `dirent_t` produced garbage `ls` output from a clean-looking build.
- **tests**: `tests/test_ext2.c` (183 assertions, hand-built in-memory image,
  wired into `tests/Makefile`) plus new path-normalisation cases in
  `test_vfs.c` and fsinfo cases in `test_vfs.c`/`test_fat16.c`. Also validated
  against a **real `mke2fs` image** on the host
  (256-byte inodes, a 300 KiB file read through doubly indirect blocks,
  byte-for-byte).
- **Verified in QEMU**: booted from ext2.img → `fs: ext2 mounted on primary
  slave` → SHELL.ELF loaded and launched; then in the ring-3 shell `ls`,
  `cat readme.txt`, `cat longfilename.txt` (a name FAT16 cannot store),
  `cd sub` + `cat nested.txt`, `pwd`, `fs`; and `startx` brought up the desktop
  (screendump: 96.5% desktop + 2.6% taskbar). The kernel ring-0 shell's `fs`
  was checked by booting with no disk (initrd only). FAT16 boot + desktop
  re-checked for regressions; `make test` and both smoke tests green,
  warning-free.

### 16.1 ATA Bus Master DMA — DONE (2026-06)
Disk I/O now uses Bus Master DMA instead of PIO, with PIO as the fallback.
- **`arch/i386/ata_dma.c` + `include/kernel/ata_dma.h`**: finds the IDE
  controller via PCI (class 0x01 / subclass 0x01), reads the Bus Master base
  from **BAR4** (I/O BAR), enables PCI bus mastering (command reg bit 2), and
  programs a single-entry **PRDT**. Read = cmd `0xC8`, write = `0xCA`,
  direction bit `0x08` in the BM command register.
- **Bounce buffer**, not arbitrary caller buffers: a page-aligned 4 KiB BSS
  buffer (+ PRDT) whose physical address is just `virt - KERNEL_OFFSET` (it's
  in the first few MiB). DMA goes through it (reads copy out after, writes copy
  in before); 4 KiB page alignment means it never crosses a 64 KiB PRD
  boundary. One DMA op = ≤8 sectors; larger transfers loop. (FAT16 reads one
  sector at a time, so this is ample.)
- **Completion is polled, not blocking** (like the net stack, and so it works
  before the scheduler exists): the IRQ14 hook `ata_dma_irq()` (called from
  `irq14_handler` before EOI) clears the controller's interrupt + sets a flag;
  the issuing thread busy-polls that flag OR the BM "active" bit with a timeout.
  Both paths clear controller state, so it's correct with or without the IRQ.
- **Integration**: `ata_read_sectors`/`ata_write_sectors` (ata.c) try DMA when
  `ata_dma_available()`, else PIO — under the **same plain `ata_lock`** (DMA
  busy-polls with interrupts on so IRQ14 can fire, exactly like the PIO loop).
  `ata_dma_init()` runs in kernel.c **after `pci_enumerate()`** (early-boot FAT
  mount already ran in PIO, which is fine).
- **The pure PRD logic `ata_dma_build_prd()` is a static inline in the header**
  (size/boundary/64-KiB-wrap rules), host-tested in `tests/test_ata_dma.c`
  (wired into `tests/Makefile`).
- **Verified** (headless QEMU, slave disk on `if=ide,index=1`): boot logged
  `ata: Bus Master DMA enabled`, then SHELL.ELF (31 KB) was read via DMA, parsed
  and executed. `make test` (all suites) + `make smoke` green, warning-free.

### Older record: Section 15 — BSD Socket API for user space (DONE)

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

That completes Section 15. (Section 16 is covered above; the forward plan is in
"Next milestone" at the top.)

## Build & test (and the #1 gotcha)

```sh
make                    # build kernel + libc
make test               # host unit tests (host gcc only) — run on every change
make run                # boot under QEMU (GUI)
make smoke              # headless boot smoke test (FAT16 disk.img)
make disk               # rebuild the FAT16 disk.img only
make ext2disk           # rebuild ext2.img (needs host mke2fs)

SMOKE_DISK=ext2.img SMOKE_SENTINEL='Section 16.2 ready' make smoke   # ext2 boot
```

**GOTCHA #1 that wasted hours:** `make disk` (and `./create_disk.sh`) rebuild
only the **userspace** `disk.img` — NOT the kernel ISO. To test a **kernel**
change you MUST run `./iso.sh` (top-level `make`/`make run` chain it). After a
bare `make disk` you are booting a stale kernel. If a kernel change "has no
effect," this is almost certainly why.

**GOTCHA #2 (fixed, but know the shape of it):** user-space objects used to have
**no header dependencies**, so editing a libc header did not rebuild the
programs that included it. Changing `DIRENT_NAME_MAX` then left a stale
`dirent_t` in SHELL.ELF while the kernel wrote the new layout — `ls` printed
garbage sizes from a build that looked perfectly clean. `user/Makefile`, `libc`
and `libgfx` now pass `-MMD -MP` and `-include` the `.d` files. If you add a new
user-space build rule, keep that.

Toolchain: `i686-elf` cross gcc, GRUB, QEMU, python3, `mke2fs` (e2fsprogs, only
for `ext2disk`) — see BUILD.md.

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

### Filesystems: two drivers behind one VFS
- **Mount is a probe, not a config**: `mount_disk()` in kernel.c tries **ext2
  first** (superblock magic at byte 1024 — a much stronger signal than FAT16's
  0x55AA) and falls back to FAT16, across primary master then slave. Whichever
  image you attach just works. The initrd is mounted earlier and a real disk
  mounts over it — do **not** guard the disk probe with `vfs_mounted()`, which
  is already true by then (that mistake cost a debugging round).
- **ext2 is read-only**: write/create/mkdir/remove/rename are NULL in
  `ext2_vfs_ops` so the VFS reports "unsupported" instead of silently doing
  nothing. FAT16 is still the read-write one.
- **Paths are normalised in the VFS, not the drivers**: `vfs_normalize()`
  resolves against the CWD and folds `.`/`..`/duplicate slashes, so drivers
  always receive an absolute path (they each strip the leading `/`). `vfs_cwd`
  is always absolute and normalised — that invariant is what makes the function
  cheap, so don't write a raw string into it.
- **Two kernel↔user ABI pairs must stay equal**, or user space silently reads
  garbage: `VFS_NAME_MAX` (vfs.h) ↔ `DIRENT_NAME_MAX` (user dirent.h), and
  `vfs_fsinfo_t` (vfs.h) ↔ `fsinfo_t` (user unistd.h). Both are commented as
  such on both sides.
- **ext2 lookup has a case-insensitive fallback** (exact match always wins).
  It is required, not a nicety: the keyboard has no shift, so `cat readme.txt`
  is the only way to reach README.TXT. See the `project_keyboard_no_shift.md`
  memory and the `ci_name_matches` comment in ext2.c.
- Adding a driver means implementing `vfs_ops_t`; `fsinfo` is optional but
  cheap, and without it `fs` reports the filesystem as unidentified.

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
terminal so the WM keeps its own keyboard), 40–42 gfx, **43 `SYS_PS`**,
**44–50 sockets**, **51 `SYS_DNS_RESOLVE`**, **52 `SYS_FSINFO`**.
**The next free number is 53.**
- **Validate user pointers** with `uap_ok` / `uap_str_ok` / `copyout` (they check
  against the process VMA table). Do not deref raw user pointers.
- A syscall that copies out a **struct** adds a kernel↔user ABI pair that has to
  be kept in sync by hand (see the filesystem section above for the two current
  ones). Comment both sides when you add another.
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
- `make smoke` with `SMOKE_SENTINEL='launching SHELL.ELF'` confirms boot;
  `SMOKE_DISK=ext2.img` boots the ext2 image instead of `disk.img`.
- To drive the **ring-3 shell** non-interactively, wait for `launching
  SHELL.ELF` in the serial log, then `sendkey` one character at a time (no
  shift: lowercase only, and use `spc`/`dot`/`slash` for punctuation).
- To reach the **ring-0 kernel shell**, boot with **no disk attached** — the
  SHELL.ELF load fails and the kernel falls back to it, with only the initrd
  mounted. That is the practical way to test kernel-shell commands.
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
