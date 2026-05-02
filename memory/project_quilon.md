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

**Next section:** 8.2 — Pipes
