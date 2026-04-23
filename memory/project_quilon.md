---
name: Quilon OS project state
description: Current completion status of roadmap sections, next steps, and key architectural decisions
type: project
---

Completed roadmap sections 4.1–4.12 (first roadmap) plus section 5 of ROADMAP2.md.

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

Additional changes:
- `elf_load_into(path, target_pd)` in elf.c — loads ELF into arbitrary PD without touching kernel PD
- `shell_cmd_exec` updated: creates per-process PD, uses `elf_load_into`, switches PD, restores on longjmp
- Shell `ps` command added — shows process table
- `kernel_main` initialises process table with `process_init()` and shows isolation demo
- `gdt_set_kernel_stack()` added to gdt.c/gdt.h
- 113 host-side unit tests pass (test_process.c + all prior suites)

**Why:** Fixes the critical bug where every ELF was mapped into the global PD at 0x400000, meaning running two programs or re-running one would overwrite each other's memory.

**Next section:** 6 — System Call Expansion (exec as proper syscall, fork, brk/sbrk, signals)
