# Quilon OS — Roadmap 2

The first roadmap took Quilon from a bare kernel stub to a working OS that can:
load a FAT16 disk, parse and run ELF32 user programs, handle system calls from
ring 3, and return control to the shell after a program exits.

This roadmap covers what comes next. The theme is **turning a demo into a real OS**:
proper process isolation, a working user space, filesystem writes, and the
foundations of multi-process execution.

---

## Table of Contents

5. [Process Isolation (Do This First)](#5-process-isolation-do-this-first)
   - 5.1 Per-Process Page Directories
   - 5.2 Process Control Block and Process Table
   - 5.3 Preemptive Context Switching
   - 5.4 Process Lifecycle: exit and wait

6. [System Call Expansion](#6-system-call-expansion)
   - 6.1 exec() as a Proper Syscall
   - 6.2 fork()
   - 6.3 User Heap: brk / sbrk
   - 6.4 Signals

7. [User Space](#7-user-space)
   - 7.1 User-Mode libc
   - 7.2 Shell as a Ring-3 Program
   - 7.3 Writing C Programs for Quilon

8. [Filesystem Maturity](#8-filesystem-maturity)
   - 8.1 FAT16 Write Support
   - 8.2 Pipes
   - 8.3 initrd — RAM-Based Initial Filesystem

9. [Memory Management](#9-memory-management)
   - 9.1 Higher-Half Kernel
   - 9.2 Demand Paging
   - 9.3 Copy-on-Write fork

10. [Towards a Real OS](#10-towards-a-real-os)
    - 10.1 PCI Bus Enumeration
    - 10.2 Network Card Driver (RTL8139)
    - 10.3 Minimal TCP/IP Stack
    - 10.4 VGA Graphics Mode (VESA/VBE)
    - 10.5 Symmetric Multiprocessing (SMP)

---

## Where We Are Now

| Component | File(s) | Status |
|-----------|---------|--------|
| GDT, IDT, exceptions | `arch/i386/gdt.c`, `interrupts.c` | ✓ Complete |
| VGA terminal + serial | `arch/i386/tty.c`, `serial.c` | ✓ Complete |
| Physical memory manager | `arch/i386/pmm.c` | ✓ Complete |
| Paging (single shared PD) | `arch/i386/paging.c` | ✓ Partial — no per-process PDs |
| Heap allocator | `kernel/kmalloc.c` | ✓ Complete |
| Keyboard + shell | `kernel/keyboard.c`, `shell.c` | ✓ Complete |
| PIT + scheduler data structures | `arch/i386/pit.c`, `kernel/scheduler.c` | ✓ Partial — no actual context switch |
| Ring-3 user mode | `kernel/usermode.c` | ✓ Complete |
| System calls (write/read/open/close/exit/getpid) | `kernel/syscall.c` | ✓ Partial — no fork/exec/brk |
| ATA PIO driver | `arch/i386/ata.c` | ✓ Complete |
| FAT16 read-only | `kernel/fat16.c` | ✓ Partial — no write |
| VFS layer | `kernel/vfs.c` | ✓ Partial — no write ops |
| ELF32 loader | `kernel/elf.c` | ✓ Complete |
| User programs | `user/hello.S` | ✓ Assembly demo only |

**The single most important missing piece:** every ELF program is mapped at
virtual address `0x400000` in the *global* page directory. If you ever run two
programs simultaneously (or re-run one without rebooting), they overwrite each
other's memory. Section 5 fixes this.

---

## 5. Process Isolation (Do This First)

### 5.1 Per-Process Page Directories

**Why it matters right now:** `elf_load()` calls `paging_map_page_alloc()` which
writes into the single kernel page directory. The second ELF you load steps on
the first one's pages at `0x400000`. Before anything else, each process needs its
own page directory.

**Background:** The x86 MMU uses the value in register CR3 as the base address of
the active page directory. When the CPU reads CR3, it finds the current process's
mapping. Switching CR3 to a different page directory instantly changes the entire
virtual address space — that is how processes are isolated from each other.

Every process needs its own page directory that:
1. Maps the kernel (the same for all processes).
2. Maps that process's own user pages (unique per process).

```
Physical RAM
┌──────────────────────────────────────┐
│ Kernel code + data  (shared)         │ ← same physical pages, mapped into
│                                      │   every PD at the same virtual address
├──────────────────────────────────────┤
│ Process A pages at 0x400000          │ ← only in Process A's PD
├──────────────────────────────────────┤
│ Process B pages at 0x400000          │ ← only in Process B's PD
└──────────────────────────────────────┘
```

**Implementation:**

```c
// arch/i386/paging.c — add these two functions

/* Allocate a fresh page directory and copy the kernel half into it.
 * Returns the physical address of the new PD, or NULL on OOM.
 *
 * "Kernel half" means PD entries 0..KERNEL_PD_ENTRIES-1 (the first
 * 4 MiB identity map + any kernel mappings above 0xC0000000 if you
 * later move to a higher-half design).  For now, copy entry 0 only
 * (the identity-mapped first 4 MiB page table). */
uint32_t *paging_create_address_space(void);

/* Switch the active page directory to pd_phys and flush the TLB. */
void paging_switch(uint32_t pd_phys);
```

```c
uint32_t *paging_create_address_space(void)
{
    uint32_t *new_pd = pmm_alloc_page();
    if (!new_pd) return NULL;
    memset(new_pd, 0, PAGE_SIZE);

    /* Copy the kernel's PD entry 0 (first 4 MiB identity map) so the
     * kernel remains accessible after the CR3 switch.  Once you implement
     * a higher-half kernel, copy the upper quarter instead.              */
    new_pd[0] = page_directory[0];

    return new_pd;
}

void paging_switch(uint32_t pd_phys)
{
    asm volatile("mov %0, %%cr3" :: "r"(pd_phys) : "memory");
}
```

Then in `elf_load()`, allocate into the *process's own* PD, not the global one.
The natural way to do this is to pass the target PD into `elf_load` (or have the
loader create a new PD and return it alongside the entry point).

> **Learning note:** Notice that the kernel page table (`entry 0`) is *shared*:
> the same physical page table is referenced from every process's PD. So a write
> to kernel memory by any process is visible everywhere — which is what you want
> (one copy of the kernel). User pages are not shared, so they are private.

---

### 5.2 Process Control Block and Process Table

**Background:** A Process Control Block (PCB) is the kernel's record of one
process. Everything the kernel needs to suspend a process and resume it later
lives here: saved registers, page directory, file descriptors, state, PID.

Your scheduler already has a `task_t` with `esp`, `cr3`, and `state`. Extend it
into a real PCB:

```c
// include/kernel/process.h
#define PROCESS_MAX         16
#define PROCESS_NAME_LEN    16

typedef enum {
    PROC_UNUSED  = 0,
    PROC_RUNNING = 1,
    PROC_READY   = 2,
    PROC_BLOCKED = 3,   /* waiting in wait() for a child */
    PROC_ZOMBIE  = 4,   /* exited, waiting to be reaped by parent */
} proc_state_t;

typedef struct process {
    uint32_t     pid;
    uint32_t     parent_pid;
    proc_state_t state;

    /* Saved kernel-mode stack pointer — restored on context switch. */
    uint32_t     kernel_esp;

    /* Physical address of this process's page directory. */
    uint32_t     cr3;

    /* Per-process file descriptor table.
     * Extends the current global VFS fd table to be per-process.   */
    int          fds[VFS_MAX_FDS];

    /* Kernel stack — one page per process.                         */
    uint8_t      kernel_stack[4096] __attribute__((aligned(16)));

    /* Exit status, written by SYS_EXIT, read by wait().            */
    int          exit_code;

    char         name[PROCESS_NAME_LEN];
} process_t;

extern process_t process_table[PROCESS_MAX];
extern process_t *current_process;

process_t *process_create(const char *name);
void       process_exit(int code);
int        process_wait(uint32_t pid, int *exit_code);
```

> **Learning note:** The `kernel_esp` field is the cornerstone of context
> switching. When the scheduler suspends a process, it saves the current stack
> pointer here. When it resumes the process, it restores this value into ESP.
> The return address on that stack points back into wherever the process was
> interrupted. The CPU "resumes" it simply by executing `ret` or `iret`.

---

### 5.3 Preemptive Context Switching

**Background:** Your PIT fires IRQ0 at 100 Hz. The IRQ0 handler currently calls
`scheduler_tick()`, which exists but does nothing beyond incrementing a counter.
This section makes that tick do a real context switch.

**The key insight:** When IRQ0 fires, the CPU pushes EFLAGS, CS, EIP onto the
kernel stack and jumps to your handler. If you also `pusha` to save the rest of
the registers, the stack contains a complete CPU snapshot. If you then change ESP
to point to a *different* process's saved stack and execute `popa` + `iret`, the
CPU resumes that other process as if nothing happened.

```asm
/* arch/i386/boot.S — revised IRQ0 stub */
irq0:
    pusha
    call scheduler_tick     /* may modify current_process */
    popa
    iret
```

```c
/* kernel/scheduler.c */
void scheduler_tick(void)
{
    /* 1. Acknowledge the PIC so future IRQs are not blocked. */
    outb(0x20, 0x20);   /* EOI to master PIC */

    /* 2. Find the next READY process (round-robin). */
    process_t *next = scheduler_pick_next();
    if (!next || next == current_process) return;

    /* 3. Switch address space if it changed. */
    if (next->cr3 != current_process->cr3)
        paging_switch(next->cr3);

    /* 4. Switch kernel stacks.
     *    current_process->kernel_esp now holds the stack pointer
     *    as it was just before this function was called — the full
     *    pusha frame sits above it.  We save it, then restore the
     *    other process's saved ESP.                                */
    process_t *prev = current_process;
    current_process = next;
    next->state = PROC_RUNNING;
    prev->state = PROC_READY;

    /* This is the context switch itself.  After the inline asm,
     * execution continues in `next`, not `prev`.                  */
    asm volatile(
        "mov %%esp, %0 \n\t"   /* save current ESP into prev->kernel_esp */
        "mov %1, %%esp \n\t"   /* load next->kernel_esp                  */
        : "=m"(prev->kernel_esp)
        : "m"(next->kernel_esp)
        : "memory"
    );
    /* When `next` was last suspended here, it also executed this asm.
     * Returning from scheduler_tick() in `next`'s stack frame now
     * causes `irq0` to execute `popa` + `iret` with `next`'s saved
     * registers, completing the switch.                            */
}
```

> **Learning note:** The hardest part of writing a scheduler is not the
> scheduling algorithm — it is setting up the initial stack frame for a brand-new
> process so that it looks like it was "interrupted" in the right place. When you
> create a process, you must manually build a fake pusha + iret frame on its
> kernel stack and set `kernel_esp` to point at it.

---

### 5.4 Process Lifecycle: exit and wait

`SYS_EXIT` currently longjmps back to the shell. With real processes that is
wrong — the process should be destroyed, its resources freed, and the parent
notified.

**Minimal implementation:**

```c
/* In syscall.c, SYS_EXIT handler: */
case SYS_EXIT:
    current_process->exit_code = (int)regs->ebx;
    current_process->state     = PROC_ZOMBIE;
    /* Wake the parent if it is blocked in wait(). */
    process_t *parent = process_find(current_process->parent_pid);
    if (parent && parent->state == PROC_BLOCKED)
        parent->state = PROC_READY;
    /* Yield — the scheduler will never pick a ZOMBIE again. */
    scheduler_yield();
    __builtin_unreachable();
```

```c
/* SYS_WAIT: block until child exits, then reap it. */
case SYS_WAIT: {
    uint32_t child_pid  = regs->ebx;
    uint32_t *exit_code = (uint32_t *)(uintptr_t)regs->ecx;
    process_t *child    = process_find(child_pid);
    if (!child) { regs->eax = (uint32_t)-1; break; }

    current_process->state = PROC_BLOCKED;
    scheduler_yield();   /* sleep until child sets us READY */

    /* We're back — child must be ZOMBIE now. */
    if (exit_code) *exit_code = (uint32_t)child->exit_code;
    child->state = PROC_UNUSED;   /* reap */
    regs->eax = 0;
    break;
}
```

> **Learning note:** A ZOMBIE process is one that has exited but whose entry in
> the process table has not yet been freed. The parent holds the exit code until
> it calls `wait()`. Without `wait()` you would never be able to retrieve the exit
> status, and the slot would leak — this is the classic "zombie process" of Unix.

---

## 6. System Call Expansion

### 6.1 exec() as a Proper Syscall

Right now `exec` is a shell command that calls `elf_load()` directly from ring 0.
A proper `SYS_EXEC` syscall allows any user program to launch another program.

```c
/* syscall.h — add: */
#define SYS_EXEC   7   /* exec(path) → does not return on success, -1 on failure */

/* syscall.c — add case: */
case SYS_EXEC: {
    const char *path = (const char *)(uintptr_t)regs->ebx;

    /* Create a new process with its own address space. */
    process_t *child = process_create(path);
    if (!child) { regs->eax = (uint32_t)-1; break; }

    /* Load the ELF into the child's address space. */
    uint32_t entry = elf_load_into(path, child->cr3);
    if (!entry) { child->state = PROC_UNUSED; regs->eax = (uint32_t)-1; break; }

    child->entry = entry;
    child->state = PROC_READY;
    regs->eax    = child->pid;
    break;
}
```

This also means refactoring `elf_load()` to take a target `cr3` parameter so it
maps pages into the child's address space rather than the current one.

---

### 6.2 fork()

`fork()` creates an exact copy of the calling process. The child starts executing
immediately after the `fork()` syscall, with one difference: `fork()` returns the
child's PID in the parent and 0 in the child.

This is conceptually simple but implementation-heavy:

1. Allocate a new process slot and a new page directory.
2. For every user page in the parent's address space, allocate a new physical
   page, copy the contents, and map it at the same virtual address in the child's
   PD.
3. Copy the parent's kernel stack into the child's kernel stack.
4. Set the child's `kernel_esp` to point at the copied stack with the return value
   (EAX) patched to 0.
5. Mark the child READY.
6. Return the child PID to the parent.

> **Learning note:** A naive fork copies every page immediately — this is correct
> but slow. Modern kernels use copy-on-write (see section 9.3): mark both the
> parent's and child's pages read-only, and only copy a page when one of them
> tries to write it. This makes `fork()` nearly free for programs that immediately
> call `exec()`.

---

### 6.3 User Heap: brk / sbrk

User programs need dynamic memory. The standard Unix mechanism is `brk()`/`sbrk()`:
the kernel tracks a "program break" — the top of the process's heap — and lets
the program extend it.

```c
/* syscall.h */
#define SYS_SBRK   8   /* sbrk(increment) → old break (void*), or -1 on OOM */

/* In process_t, add: */
uint32_t heap_start;   /* bottom of heap — set to end of last ELF segment    */
uint32_t heap_end;     /* current break — top of heap                        */
```

```c
case SYS_SBRK: {
    int32_t  increment = (int32_t)regs->ebx;
    uint32_t old_break = current_process->heap_end;
    uint32_t new_break = old_break + (uint32_t)increment;

    /* Allocate physical pages to cover [old_break, new_break). */
    uint32_t page = old_break & ~(PAGE_SIZE - 1);
    while (page < new_break) {
        void *phys = pmm_alloc_page();
        if (!phys) { regs->eax = (uint32_t)-1; goto sbrk_done; }
        paging_map_page_alloc(page, (uint32_t)phys,
                              PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
        page += PAGE_SIZE;
    }
    current_process->heap_end = new_break;
    regs->eax = old_break;
    sbrk_done: break;
}
```

With `sbrk` working, a user-space `malloc` built on top of it (like the classic
K&R allocator) will work without any further kernel changes.

---

### 6.4 Signals

Signals are asynchronous notifications sent to a process: `SIGKILL` terminates it,
`SIGSEGV` fires on a bad memory access, `SIGCHLD` fires when a child exits.

A minimal signal implementation for Quilon:

1. Add a `pending_signals` bitmask to `process_t`.
2. Add a `signal_handlers[NSIG]` array (function pointers or `SIG_DFL`/`SIG_IGN`).
3. After every syscall and every context switch, check `pending_signals`. If any
   are set, and the process is in user mode, redirect EIP to the handler.
4. The page fault handler (`exception 14`) already fires when ring-3 code touches
   an unmapped page — make it send `SIGSEGV` to the current process instead of
   panicking the kernel.

```c
/* include/kernel/signal.h */
#define NSIG       32
#define SIGKILL     9
#define SIGSEGV    11
#define SIGCHLD    17

void signal_send(process_t *proc, int signum);
void signal_dispatch(void);  /* called on return from every syscall */
```

> **Learning note:** Signal delivery is tricky because the handler runs in user
> mode at ring 3. The kernel must build a special stack frame that pushes the
> signal number and a return address pointing to a small trampoline (called a
> "sigreturn stub") that issues a `SYS_SIGRETURN` syscall to restore the original
> registers. Getting this right is a good test of how well you understand the
> ring-3/ring-0 boundary.

---

## 7. User Space

### 7.1 User-Mode libc

Right now the only user-space program is `user/hello.S`, written in assembly.
To write real programs in C you need a libc — a thin layer that translates C
library calls into Quilon syscalls.

Unlike the kernel's `libk.a` (which uses `printf` → `terminal_write` directly),
the user-space libc must use `int $0x80` to reach the kernel.

**Suggested layout:** `user/libc/`

```
user/libc/
├── include/
│   ├── stdio.h      — printf, puts, fwrite, fopen, fclose
│   ├── stdlib.h     — malloc, free, exit, atoi
│   ├── string.h     — memcpy, memset, strlen, strcmp
│   └── unistd.h     — write, read, open, close, getpid, sbrk
├── syscall.S        — int $0x80 wrappers for each SYS_* number
├── stdio.c          — printf built on top of write()
├── stdlib.c         — malloc/free built on top of sbrk()
└── string.c         — same memcpy/strlen/... as kernel libk
```

The key file is `syscall.S` — the raw `int $0x80` wrappers:

```asm
/* user/libc/syscall.S */
.global write
write:                    /* int write(int fd, const void *buf, int len) */
    pushl %ebx
    movl  8(%esp),  %eax  /* load return addr offset — watch the push above */
    movl  $1, %eax        /* SYS_WRITE */
    movl  8(%esp),  %ebx  /* fd */
    movl  12(%esp), %ecx  /* buf */
    movl  16(%esp), %edx  /* len */
    int   $0x80
    popl  %ebx
    ret

.global exit
exit:
    movl  $3, %eax        /* SYS_EXIT */
    movl  4(%esp), %ebx   /* exit code */
    int   $0x80
    /* does not return */
```

Once this libc is built as `user/libc/libc.a`, user programs can be compiled with:

```bash
i686-elf-gcc -ffreestanding -nostdlib -Iuser/libc/include \
    -o my_program.elf my_program.c \
    -Luser/libc -lc -lgcc
```

---

### 7.2 Shell as a Ring-3 Program

Today's shell (`kernel/shell.c`) runs in ring 0 — it is part of the kernel. This
means a bug in the shell crashes the entire OS. The shell should run as an ordinary
ring-3 process.

**Steps:**

1. Move the shell source to `user/shell/shell.c`.
2. Build it against the user libc (`user/libc/libc.a`).
3. Include `shell.elf` in the FAT16 disk image.
4. In `kernel_main`, after the filesystem is mounted, call `process_create` to
   load and launch `shell.elf` as the first user process.
5. Remove `shell_run()` from `kernel_main`.

The kernel's job after boot becomes just: initialize hardware, mount the disk,
spawn `shell.elf`, and start the scheduler. Everything else happens in user space.

This is the architecture of every real Unix: the kernel is minimal, `init` (PID 1)
is the first user process.

> **Learning note:** Once the shell is a user program, a bug in it causes a
> SIGSEGV or a failed assertion — not a kernel panic. The kernel can kill the
> crashed shell and spawn a fresh one. This separation is why operating systems
> are called "operating systems" rather than "operating programs" — the OS
> manages and isolates the programs running on top of it.

---

### 7.3 Writing C Programs for Quilon

With a libc in place, the workflow for writing a Quilon program becomes:

```c
/* user/hello_c/main.c */
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    printf("Hello from C on Quilon!\n");
    return 0;
}
```

```bash
i686-elf-gcc -ffreestanding -nostdlib \
    -Iuser/libc/include -Luser/libc \
    -T user/link.ld -o hello_c.elf main.c -lc -lgcc
```

To make this a first-class workflow, add a `user/Makefile` rule that compiles
all `user/*/main.c` programs and embeds the resulting ELF files in the disk image
automatically.

---

## 8. Filesystem Maturity

### 8.1 FAT16 Write Support

The VFS ops table (`vfs_ops_t`) currently has no `write` function. Adding writes
to FAT16 requires:

1. **Add `write` to the VFS ops table:**
   ```c
   typedef struct {
       int  (*open)   (...);
       int  (*read)   (...);
       int  (*write)  (void *ctx, vfs_node_t *node, uint32_t offset,
                       uint32_t size, const uint8_t *buf);  /* NEW */
       int  (*readdir)(...);
       void (*close)  (...);
       int  (*create) (void *ctx, const char *path);         /* NEW */
       int  (*remove) (void *ctx, const char *path);         /* NEW */
   } vfs_ops_t;
   ```

2. **Implement `fat16_write()`:** Find the file's FAT chain. If the write extends
   past the last cluster, allocate new clusters and append them to the chain. Write
   the sector(s) back via ATA. Update the directory entry's file size.

3. **Implement `fat16_create()`:** Find a free root directory entry (one whose
   first byte is `0x00` or `0xE5`). Write the 8.3 name, attribute (`0x20`), and
   allocate the first cluster.

4. **Add `SYS_WRITE` for file descriptors:** Currently `SYS_WRITE` only handles
   stdout/stderr. It should forward fd ≥ `VFS_FD_BASE` to `vfs_write()`.

> **Learning note:** FAT16 write is significantly harder than read because you must
> maintain consistency between the FAT table and the directory entries. Always
> write the FAT first, then the data, then the directory entry — if the system
> crashes mid-write you want the old directory entry to still point to valid data.

---

### 8.2 Pipes

A pipe is an anonymous in-memory channel between two processes. One process writes
into the write end; another reads from the read end.

```c
/* syscall.h */
#define SYS_PIPE  9   /* pipe(int fds[2]) → 0 or -1 */

/* kernel/pipe.c — ring buffer in kernel memory */
#define PIPE_BUF_SIZE 4096

typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    int      readers;   /* reference counts — close when both reach 0 */
    int      writers;
} pipe_t;
```

The pipe appears as two synthetic file descriptors. `vfs_read(read_fd)` blocks
the reading process if the buffer is empty; `vfs_write(write_fd)` blocks if the
buffer is full. This is the first form of inter-process communication (IPC) and
enables shell pipelines like:

```
quilon> cat README.TXT | hexdump
```

---

### 8.3 initrd — RAM-Based Initial Filesystem

The FAT16 disk requires ATA hardware, which may not be available (e.g. inside a
virtual machine configured without a disk, or on real hardware where the drive
takes time to spin up). An initrd is a small filesystem embedded directly in the
kernel image (or loaded by GRUB as a Multiboot module) that is available
immediately at boot, before any drivers are initialized.

A minimal initrd format:

```
[ 4 bytes: file count N ]
[ N × file header:
    [ 16 bytes: name ]
    [ 4 bytes:  size ]
    [ size bytes: data ]
]
```

GRUB passes the initrd address and size to the kernel in the Multiboot info
struct (`mods_addr` / `mods_count`). Read those fields in `kernel_main` and mount
the initrd VFS driver before trying to initialize ATA. This lets the kernel boot
to a shell even without a disk.

---

## 9. Memory Management

### 9.1 Higher-Half Kernel

**Background:** Today the kernel lives at virtual address `0x00100000` (1 MiB).
This is the same region user programs use. A "higher-half" kernel remaps the
kernel to `0xC0000000` (3 GiB), giving the bottom 3 GiB entirely to user
processes and reserving the top 1 GiB for the kernel.

This is how Linux and most production kernels are structured. Benefits:
- User programs can use the full range `0x00000000–0xBFFFFFFF`.
- The kernel is invisible to user-mode code (it is not mapped into user
  page tables, or if it is, the pages are supervisor-only).

**Implementation overview:**

1. In `boot.S`, set up a temporary page directory that identity-maps the first
   4 MiB AND maps `0xC0000000–0xC0400000` → `0x00000000–0x00400000`. Enable
   paging with this temporary PD.
2. Jump to a high virtual address using a `lea` / `add` trick on EIP.
3. Remove the identity map (PD entry 0), keeping only the high mapping.
4. Update `linker.ld` so all kernel symbols are linked at `0xC0100000`.
5. Update every place that uses a physical address directly to add (or subtract)
   the kernel offset `0xC0000000`.

> **Learning note:** The higher-half switch happens in assembly before any C code
> runs, because C variables already use the link-time addresses. The tricky
> moment is the instruction immediately after `mov eax, cr0; or PG_BIT; mov cr0,
> eax` — the next instruction fetch uses the new mapping, so it must exist.

---

### 9.2 Demand Paging

**Background:** Today `elf_load()` allocates all pages for a program immediately
when it is loaded. With demand paging, pages are not allocated until the program
actually touches them. The page fault handler allocates and maps the missing page
on demand.

```c
/* arch/i386/exceptions.c — page fault handler (vector 14) */
void page_fault_handler(registers_t *regs)
{
    uint32_t fault_addr;
    asm volatile("mov %%cr2, %0" : "=r"(fault_addr));

    /* Is this a valid but not-yet-mapped user address?
     * Check the process's VMA (Virtual Memory Area) list. */
    vma_t *vma = vma_find(current_process, fault_addr);
    if (!vma) {
        /* No VMA covers this address — genuine segfault. */
        signal_send(current_process, SIGSEGV);
        return;
    }

    /* Allocate a physical page and map it. */
    void *page = pmm_alloc_page();
    memset(page, 0, PAGE_SIZE);
    paging_map_page_alloc(fault_addr & ~(PAGE_SIZE - 1),
                          (uint32_t)page,
                          PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    /* Return — the faulting instruction is re-executed automatically. */
}
```

A VMA (Virtual Memory Area) is a description of a contiguous virtual address
range: `[start, end)`, its flags (readable, writable, executable), and where its
data comes from (anonymous zero page, file-backed, etc.). Store a small array of
VMAs in each `process_t`.

---

### 9.3 Copy-on-Write fork()

In section 6.2, `fork()` was described as copying every page immediately. With
copy-on-write (CoW):

1. On `fork()`, map all writable user pages in both parent and child as
   **read-only** (clear `PAGE_WRITABLE`). The parent and child share the same
   physical pages.
2. When either process writes to a shared page, the CPU raises a page fault
   (because `PAGE_WRITABLE` is clear).
3. The page fault handler sees that this is a CoW fault (the VMA says the page
   should be writable, but the PTE says read-only). It:
   a. Allocates a new physical page.
   b. Copies the old page's content.
   c. Maps the new page read-write in the faulting process.
   d. Decrements the reference count on the original page.

With CoW, `fork()` + immediate `exec()` touches almost no pages — the typical
shell usage pattern (`fork`, then `exec` the new program) becomes nearly free.

---

## 10. Towards a Real OS

These sections describe longer-range work. They are independent of each other
and can be tackled in any order after section 9.

---

### 10.1 PCI Bus Enumeration

Before writing any device driver, you need to find what devices are present.
The PCI configuration space is accessed via two I/O ports: `0xCF8` (address) and
`0xCFC` (data).

```c
/* arch/i386/pci.c */
uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func <<  8)
                  | (offset & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

void pci_enumerate(void)
{
    for (int bus = 0; bus < 256; bus++)
    for (int slot = 0; slot < 32; slot++) {
        uint32_t word = pci_read(bus, slot, 0, 0);
        if ((word & 0xFFFF) == 0xFFFF) continue;   /* no device */
        uint16_t vendor = word & 0xFFFF;
        uint16_t device = word >> 16;
        printf("pci: bus %d slot %d — vendor=0x%x device=0x%x\n",
               bus, slot, vendor, device);
    }
}
```

PCI enumeration is a prerequisite for the network card driver in 10.2.

---

### 10.2 Network Card Driver (RTL8139)

The Realtek RTL8139 is the classic learning network card. QEMU emulates it
(`-device rtl8139`), it has a simple register interface, and it is
thoroughly documented on the OSDev Wiki.

**Steps:**
1. Detect the RTL8139 via PCI enumeration (vendor `0x10EC`, device `0x8139`).
2. Read the Base Address Register (BAR0) to find the I/O port base.
3. Initialize: software reset, enable TX/RX, set the receive buffer address.
4. Transmit: write the packet address and size to a TX descriptor register.
5. Receive: poll or interrupt — the card writes received packets to a ring buffer
   in kernel memory pointed to by `RBSTART`.

```c
/* arch/i386/rtl8139.c — transmit one raw Ethernet frame */
void rtl8139_send(const void *data, uint16_t len)
{
    /* RTL8139 has 4 TX descriptors (TSD0–TSD3), used round-robin. */
    static int tx_slot = 0;

    outl(io_base + 0x20 + tx_slot * 4, (uint32_t)(uintptr_t)data);
    outl(io_base + 0x10 + tx_slot * 4, len & 0x1FFF);

    tx_slot = (tx_slot + 1) % 4;
}
```

---

### 10.3 Minimal TCP/IP Stack

With raw Ethernet frames flowing, a minimal network stack follows:

| Layer | Protocol | Notes |
|-------|----------|-------|
| Layer 2 | Ethernet II | Src/dst MAC, EtherType |
| Layer 3 | IPv4 | Header checksum, TTL, src/dst IP |
| Layer 3 | ARP | Map IP → MAC before sending |
| Layer 4 | UDP | Simple, connectionless — a good first target |
| Layer 4 | TCP | Add sequence numbers, ACK, retransmit |
| Application | DHCP (UDP) | Get an IP address automatically |

A UDP echo server fits in about 300 lines of C and is a satisfying milestone:
you can `ping` Quilon from your host machine.

---

### 10.4 VGA Graphics Mode (VESA/VBE)

The VBE (VESA BIOS Extensions) interface lets you switch from 80×25 text mode to
a linear framebuffer at any resolution (e.g. 1024×768 × 32bpp). GRUB can perform
this switch for you with a single line in `grub.cfg`:

```
set gfxmode=1024x768x32
set gfxpayload=keep
```

The Multiboot info struct then contains the framebuffer address, dimensions, and
pixel format. Your VGA terminal driver needs a full replacement that draws glyphs
from a bitmap font into the framebuffer.

Graphics mode unlocks:
- A proper terminal emulator (ANSI escape codes, colours, bold)
- Window manager concepts
- Bitmap and vector rendering

---

### 10.5 Symmetric Multiprocessing (SMP)

Modern machines have multiple CPU cores. Getting them all running requires:

1. **APIC:** Replace the legacy PIC with the Advanced Programmable Interrupt
   Controller. The APIC supports per-CPU local timers and inter-processor
   interrupts (IPIs).
2. **ACPI/MP tables:** Parse the ACPI MADT or Intel MP table to discover how
   many cores ("Application Processors", APs) are present.
3. **AP startup:** Send a SIPI (Startup Inter-Processor Interrupt) to each AP.
   The AP starts in real mode at a trampoline page, switches to protected mode,
   enables paging, and joins the scheduler.
4. **Spinlocks:** Every shared kernel data structure needs a spinlock. Without
   locking, two cores modifying the PMM bitmap simultaneously will corrupt it.
5. **Per-CPU data:** Each core needs its own TSS, GDT, and interrupt stack.

SMP is the most complex topic on this list. It is typically the last major
milestone for a hobby OS before it starts resembling a real kernel.

---

## Reference Reading

| Resource | What it covers |
|----------|----------------|
| **OSDev Wiki** (wiki.osdev.org) | Process management, SMP, PCI, RTL8139, VESA/VBE — all covered in depth with working code examples. |
| **"Operating Systems: Three Easy Pieces"** | Processes, virtual memory, file systems explained conceptually. Excellent companion to hands-on work. Free PDF. |
| **Intel IA-32 SDM Vol 3A** | Paging, task management, TSS, APIC. The authoritative source for everything hardware-level. |
| **"The Design of the UNIX Operating System"** by Bach | How Unix implements processes, files, and the VFS. Very readable and directly applicable to Quilon's design. |
| **Linux 0.01 source** | Linus's first release. Small enough (~10k lines) to read in full. Illuminates how fork/exec/wait actually work. |
| **QEMU `-monitor stdio`** | `info mem` dumps the live page table. `info registers` shows all CPU state. `x/10x 0x400000` reads user-space memory. Invaluable for debugging ring-3 issues. |
| **GDB + QEMU** | Run QEMU with `-s -S` to halt at startup, then `gdb -ex "target remote :1234" quilon.kernel`. Full source-level debugging of the kernel. |

---

## Suggested Milestones

| Milestone | Sections | What it proves |
|-----------|----------|----------------|
| **Milestone 1 — Multi-process** | 5.1 + 5.2 + 5.3 | Two ELF programs run simultaneously without corrupting each other |
| **Milestone 2 — Unix basics** | 5.4 + 6.1 + 6.2 | Shell can `exec` a program, `fork` a child, and `wait` for it |
| **Milestone 3 — C programs** | 6.3 + 7.1 + 7.3 | A program written in C (not assembly) runs on Quilon |
| **Milestone 4 — User shell** | 7.2 | The shell itself runs at ring 3; kernel_main exits after spawn |
| **Milestone 5 — Read/write FS** | 8.1 | User programs can create and write files on FAT16 |
| **Milestone 6 — Networked** | 10.1 + 10.2 + 10.3 | Quilon responds to a ping from the host |

---

*Document written for Quilon OS — April 2026.*
