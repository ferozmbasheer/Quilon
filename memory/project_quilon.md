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

**Next section:** Section 9 — Memory Management (Higher-Half Kernel, Demand Paging, CoW fork)
