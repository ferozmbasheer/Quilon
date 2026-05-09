---
name: Quilon OS project state
description: Current completion status of roadmap sections, next steps, and key architectural decisions
type: project
---

Completed roadmap sections 4.1–4.12 (first roadmap) plus sections 5–8.1 of ROADMAP2.md.

**Section 8.1 — FAT16 Write Support (completed 2026-05-01)**

All write operations implemented and tested:

- `fat16_write()` — writes to existing file clusters via read-modify-write, updates dir entry size
- `fat16_create()` — allocates cluster, writes new root-dir entry (8.3 name, attr=0x20)
- `fat16_remove()` — frees FAT cluster chain, marks dir entry deleted (0xE5)
- `fat16_write_fat_entry()` / `fat16_alloc_cluster()` / `fat16_free_chain()` — helpers
- `ata_sector_write()` wrapper added to kernel.c; `fs_ctx.sector_write` now wired to ATA
- VFS layer (`vfs_write`, `vfs_create`, `vfs_remove`) was already plumbed in
- Syscalls `SYS_CREATE` (13) and `SYS_REMOVE` (14) were already in syscall.c
- Shell commands added: `touch <file>`, `write <file> <data>`, `rm <file>`, `fstest`
- 57 fat16 unit tests + 47 VFS unit tests, all passing

Bug fixes in fat16.c:
- Forward declaration for `fat16_next_cluster` (used before its definition in `fat16_free_chain`)
- `find_free:` label in `fat16_create` followed by declaration — fixed with `; ` null statement

**Section 5 — Process Isolation (completed 2026-04-22)**

All four subsections implemented:

5.1 Per-Process Page Directories
- `paging_create_address_space()` in paging.c — allocates new PD, copies kernel PDE[0]
- `paging_switch(pd_phys)` — writes CR3 to switch address space
- `paging_map_page_alloc_into(pd, virt, phys, flags)` — maps page into arbitrary PD
- `page_directory[]` now non-static; exposed via `paging_get_kernel_pd()`

5.2 Process Control Block and Process Table
- `kernel/include/kernel/process.h` — `process_t` PCB, `proc_state_t` enum, API
- `kernel/kernel/process.c` — `process_init`, `process_create`, `process_find`, `process_pick_next`, `process_launch`
- `PROCESS_MAX = 16` slots, each with 4096-byte per-process `kernel_stack[]`

5.3 Preemptive Context Switching
- `context_switch(prev_esp_ptr, next_esp_val)` assembly in boot.S — saves/restores callee-saved regs + ESP
- `process_first_run` assembly trampoline in boot.S — lands on first scheduling of new process
- `scheduler_tick()` updated to use `process_pick_next()` + `context_switch()`
- `scheduler_yield()` added for voluntary CPU release (SYS_EXIT, SYS_WAIT)
- TSS `esp0` updated on every context switch via `gdt_set_kernel_stack()`

5.4 Process Lifecycle: exit and wait
- `SYS_EXIT` marks process ZOMBIE, wakes blocked parent, calls `scheduler_yield()`
- `SYS_WAIT` (syscall 7) blocks caller until child becomes ZOMBIE, then reaps
- `SYS_EXEC` (syscall 8) creates child address space + PCB via `elf_load_into()`

**Section 8.2 — Pipes (completed 2026-05-02)**

Anonymous in-kernel ring-buffer pipes implemented:

- `kernel/include/kernel/pipe.h` — `pipe_t` struct, `PIPE_MAX=8`, `PIPE_BUF_SIZE=4096`, full API
- `kernel/kernel/pipe.c` — ring-buffer, blocking read/write (yield loop), `wake_blocked()` wake-all helper
- `vfs_node_t` extended: `is_pipe`, `pipe_write_end`, `pipe_idx` fields
- `vfs_read`/`vfs_write`/`vfs_close` dispatch to pipe functions when `is_pipe=1`
- `vfs_pipe(fds[2])` allocates a pipe slot and two VFS FDs (read/write ends)
- `SYS_PIPE = 17` — `pipe(int fds[2])` syscall added to syscall.h + syscall.c
- `pipe()` asm stub added to user/libc/syscall.S; declared in unistd.h
- Kernel shell: `pipetest` command; User shell: `pipe` command
- `tests/test_pipe.c` — 55 tests covering alloc, ring-buffer, wrap-around, ref counting, error handling
- All 14 test suites pass (645 total assertions)

**Blocking design:** `pipe_read` yields when empty (writers > 0); `pipe_write` yields when full. `wake_blocked()` wakes all PROC_BLOCKED processes after writes/close (safe: callers recheck predicate on resume). Works with preemptive PIT timer because int 0x80 is a trap gate (IF not cleared).

**Known limitation:** Global VFS FD table (not per-process) means fork+pipe requires care — close in one process affects all. Shell `pipe` demo uses single-process write/read to show the mechanism cleanly.

**Section 8.3 — initrd RAM-Based Initial Filesystem (completed 2026-05-02)**

Read-only VFS driver backed by an in-memory image, available before any disk driver initialises:

- `kernel/include/kernel/initrd.h` — `initrd_ctx_t`, `initrd_entry_t`, `initrd_vfs_ops`, `initrd_mount()`, `initrd_build_demo()`
- `kernel/kernel/initrd.c` — `initrd_mount()` parses the flat image format; VFS ops (open/read/readdir/close); write/create/remove are NULL (read-only); `initrd_build_demo()` writes a 3-file demo image (MOTD.TXT, VERSION.TXT, INIT.SH)
- `kernel/include/kernel/multiboot.h` — added `MULTIBOOT_FLAG_MODS` and `multiboot_module_t` struct
- `kernel/kernel/kernel.c` — Section 8.3 block before ATA init: checks for GRUB module → mounts if valid; always builds synthetic demo image and reads it; mounts synthetic as VFS fallback if no disk/GRUB module
- Shell commands added: `initrd` in both kernel shell.c and user/shell/main.c
- `tests/test_initrd.c` — 68 tests: mount validation, open, read, readdir, read-only enforcement, `initrd_build_demo`, VFS integration
- All 15 test suites pass (713+ total assertions)

**Image format:** `[uint32_t N] [N × {char name[16]; uint32_t size; uint8_t data[size]}]`
**Boot order:** initrd mounted first → ATA/FAT16 mounts and replaces it if a disk is found.

**Section 9.3 — Copy-on-Write fork (completed 2026-05-03)**

CoW fork implemented across all layers:

- `kernel/arch/i386/pmm.c` — added `uint8_t refcount[MAX_PAGES]`; `pmm_alloc_page` sets refcount=1; `pmm_free_page` decrements and only frees when refcount reaches 0; new `pmm_ref_page()` increments refcount; `pmm_page_refcount()` returns count; `pmm_init_range()` zeroes refcount array
- `kernel/include/kernel/pmm.h` — declared `pmm_ref_page`, `pmm_page_refcount`
- `kernel/include/kernel/paging.h` — added `PAGE_COW = (1u << 9)` (OS-reserved PTE bit 9); updated `paging_fork_address_space` docs; declared `paging_cow_handle(pd, fault_addr)`
- `kernel/arch/i386/paging.c` — rewrote `paging_fork_address_space` to CoW: shares physical pages, marks writable PTEs read-only+PAGE_COW in both parent and child, calls `pmm_ref_page`; added `paging_cow_handle`: single-owner → restore writability in place; multi-owner → copy page, `pmm_free_page` old ref, map new page writable; uses `invlpg` for TLB invalidation
- `kernel/arch/i386/exceptions.c` — CoW check before SIGSEGV: write fault (err_code bit 1) + VMA_W → calls `paging_cow_handle`; returns normally to re-execute faulting instruction
- `kernel/kernel/syscall.c` — SYS_FORK: after `paging_fork_address_space`, flushes parent TLB via `paging_switch(current_process->cr3)`; copies VMAs from parent to child
- `kernel/kernel/kernel.c` — Section 9.3 boot demo: PMM refcount lifecycle, PAGE_COW flags, free-count invariant after CoW fork
- `kernel/kernel/shell.c` + `user/shell/main.c` — `cow` command added to both shells; user shell `cmd_cow` does actual fork+write isolation demo

**Tests:** `tests/test_cow.c` — 67 tests: PMM refcount lifecycle, PAGE_COW flag properties, CoW fork PTE flag logic (simulated), CoW handle shared/sole-owner cases, full parent/child isolation lifecycle. Makefile rule added.

**All 17 test suites pass.**

**Section 10.1 — PCI Bus Enumeration (completed 2026-05-03)**

PCI configuration space enumeration via I/O ports 0xCF8/0xCFC:

- `kernel/include/kernel/pci.h` — `pci_device_t`, `PCI_MAX_DEVICES=64`, `pci_config_addr()` static inline, vendor/device constants (REALTEK, RTL8139, INTEL, QEMU), config register offsets; all hardware-independent declarations usable by tests
- `kernel/arch/i386/pci.c` — `pci_read`/`pci_write` (inline `outl`/`inl` asm); `pci_enumerate` (256 buses × 32 slots, multi-function aware via header type bit 7); `pci_find_device`, `pci_get_device`, `pci_class_name`; hardware functions guarded by `#ifdef __is_kernel`
- `kernel/kernel/kernel.c` — Section 10.1 boot demo: calls `pci_enumerate()`, lists all devices with class name, highlights RTL8139 if present
- `kernel/kernel/shell.c` — `pci` command: re-enumerates and prints full device table
- `user/shell/main.c` — `pci` command: scans all buses via `pci_read_u()` syscall, inline class name lookup
- `SYS_PCI_READ = 18` — EBX=bus, ECX=(slot<<8)|func, EDX=offset → 32-bit dword; kernel delegates to `pci_read()`; host build returns -1
- `user/libc/syscall.S` — `pci_read_u` stub (packs slot/func into ECX)
- `user/libc/include/unistd.h` — `pci_read_u()` declaration + SYS_PCI_READ constant
- `tests/test_pci.c` — 37 tests: address construction (enable bit, bus/slot/func/offset fields, known values), class name lookup, device table index/find, table capacity limit; all pass
- All 18 test suites pass.

**Section 10.2 — RTL8139 Network Card Driver (completed 2026-05-03)**

RTL8139 NIC driver over PCI: DMA ring-buffer RX, 4-slot TX, IRQ, MAC read. SYS_NET_SEND/RECV/STATUS syscalls. Shell: `net`, `netsend` commands.

**Section 10.3 — Minimal TCP/IP Stack (completed 2026-05-03)**

Full protocol stack: Ethernet II, ARP, IPv4, ICMP, UDP, TCP, DHCP.

- `kernel/include/kernel/net.h` — all protocol structs, constants, pure-C static-inline helpers (checksum, byte-order, frame builders); testable on host without x86 I/O
- `kernel/kernel/net.c` — hardware-dependent implementation (guarded by `#ifdef __is_kernel`): ARP cache (4 slots), DHCP discover→offer→request→ACK, ICMP ping, single TCP connection state machine (LISTEN→SYN_RCVD→ESTABLISHED→CLOSED)
- **Checksum storage bug fixed:** `ipv4_fill`, `icmp_fill_echo_request`, `icmp_fill_echo_reply` must call `net_htons(net_checksum16(...))` — storing `checksum = net_checksum16(...)` directly was little-endian-wrong (bytes swapped vs. network order)
- New syscalls: `SYS_NET_PING=22`, `SYS_NET_DHCP=23`, `SYS_NET_GETIP=24`
- Kernel shell: `ping <ip>`, `dhcp`, `arp`, `tcpip` commands
- User shell: `ping <ip>`, `dhcp`, `ip` commands
- `tests/test_net.c` — 107 tests: byte-order helpers, checksum (RFC 1071 + TCP/UDP pseudo-header), Ethernet/ARP/IPv4/ICMP/UDP/TCP frame builders, full Eth+IP+ICMP frame assembly; all pass
- All 20 test suites pass.

**Next section:** Section 10.4 (check ROADMAP2.md for the next item)
