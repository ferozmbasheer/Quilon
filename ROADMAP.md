# Quilon OS — Developer Roadmap

A code review, bug fixes, and learning path for building Quilon into a mature hobby OS.
Aimed at a beginner OS developer; every section explains *why*, not just *what*.

---

## Table of Contents

1. [What You Have So Far](#1-what-you-have-so-far)
2. [Bug Fixes (Do These First)](#2-bug-fixes-do-these-first)
3. [Code Improvements](#3-code-improvements)
4. [Next Steps — Ordered Learning Path](#4-next-steps--ordered-learning-path)
   - 4.1 CPU Exception Handlers
   - 4.2 Physical Memory Manager
   - 4.3 Paging
   - 4.4 Heap Allocator
   - 4.5 Serial Port Logging
   - 4.6 Keyboard Input Buffer
   - 4.7 Shell
   - 4.8 Timer / Scheduler
   - 4.9 User Mode
   - 4.10 System Calls
   - 4.11 Filesystem
   - 4.12 ELF Loader
5. [Reference Reading](#5-reference-reading)

---

## 1. What You Have So Far

This is a solid starting point. Here is a plain-English map of every piece:

| Component | File(s) | What it does |
|-----------|---------|--------------|
| Multiboot entry | `arch/i386/boot.S` | GRUB hands control to `_start` here. Sets up a 16 KiB stack and calls `kernel_main`. |
| GDT | `arch/i386/gdt.c` | Tells the CPU what memory segments exist. Required to stay in 32-bit protected mode. |
| IDT + PIC | `arch/i386/interrupts.c` | Tells the CPU which handler to call for each interrupt. Remaps hardware IRQs away from CPU exception vectors. |
| VGA terminal | `arch/i386/tty.c` | Writes characters to the 80×25 text screen at 0xB8000. Handles scrolling and cursor. |
| Keyboard | `arch/i386/interrupts.c` | IRQ1 handler reads a scancode, looks it up in a table, and prints the character. |
| printf | `libc/stdio/printf.c` | Supports `%c`, `%s`, `%d`. Calls `putchar`, which calls `terminal_write`. |
| String lib | `libc/string/` | `memcpy`, `memmove`, `memset`, `memcmp`, `strlen`. |
| C runtime | `arch/i386/crti.S`, `crtn.S` | Runs global constructors/destructors (needed for C++, harmless for C). |
| Linker script | `arch/i386/linker.ld` | Places the kernel at 1 MB and defines section layout. |

**What is NOT yet implemented:**

- CPU exception handling (divide-by-zero, page fault, etc.)
- Physical memory manager (no way to allocate RAM)
- Paging (virtual memory)
- Heap / `malloc`
- Multitasking
- User mode
- System calls
- Filesystem
- Disk I/O

---

## 2. Bug Fixes (Do These First)

These are real bugs that will cause incorrect behaviour right now.

---

### Bug 1 — `printf("%d", 0)` prints nothing

**File:** `libc/stdio/printf.c`  
**Why it matters:** Any counter, status code, or debug value that happens to be 0 disappears silently.

**Root cause:** The integer-to-string conversion loop is `while(copy)`. When `copy == 0` the loop body never runs, so the digit buffer stays empty and nothing is printed.

**The fix:**

```c
// Replace the integer printing block inside printf_impl with:
static void print_int(int value)
{
    if (value < 0) {
        putchar('-');
        value = -value;
    }

    // Special case: zero
    if (value == 0) {
        putchar('0');
        return;
    }

    // Build digits in reverse
    char buf[12]; // enough for INT_MIN
    int len = 0;
    unsigned int uval = (unsigned int)value;
    while (uval > 0) {
        buf[len++] = '0' + (uval % 10);
        uval /= 10;
    }

    // Print in correct order
    for (int i = len - 1; i >= 0; i--)
        putchar(buf[i]);
}
```

> **Learning note:** Always test boundary values (0, -1, INT_MAX, INT_MIN) when writing number formatters. These edges are where most bugs hide.

---

### Bug 2 — Triple fault on any CPU exception

**File:** `arch/i386/interrupts.c`  
**Why it matters:** A divide-by-zero, null pointer dereference, or stack overflow causes an immediate reboot with no error message — the hardest class of bug to diagnose.

**Root cause:** IDT vectors 0–31 are reserved by the CPU for exceptions. They are currently not populated. When an exception fires, the CPU finds a zeroed IDT entry and triple-faults.

**Minimum fix:** Add a catch-all exception handler now (details in Section 4.1).

---

### Bug 3 — Backspace is broken at the left margin

**File:** `arch/i386/tty.c`, the `'\b'` case  
**Why it matters:** Pressing backspace at column 0 wraps to the wrong row.

**Root cause:**
```c
// Current code — wrong
if(--terminal_row == 1)
    terminal_row = 2;
```
The intent is probably "don't go above row 2", but the condition is off-by-one. If `terminal_row` was 3 and you decrement, you get 2 — the check passes and it jumps back to 2, which is correct. But if it was 2 and you decrement, you get 1 — the check fires and it sets to 2, skipping row 1 entirely. Row 0 is never reachable.

**The fix:**
```c
case '\b':
    if (terminal_column > 0) {
        terminal_column--;
    } else if (terminal_row > 0) {
        // Wrap to end of previous row
        terminal_row--;
        terminal_column = VGA_WIDTH - 1;
    }
    // Erase the character at the new position
    terminal_putentryat(' ', terminal_color,
                        terminal_column, terminal_row);
    update_cursor(terminal_column, terminal_row);
    break;
```

---

### Bug 4 — TSS kernel stack pointer is wrong

**File:** `arch/i386/gdt.c`  
**Why it matters:** When the CPU switches from user mode to kernel mode (future work), it loads `esp0` from the TSS to find the kernel stack. If this points to the wrong place, the kernel immediately corrupts memory.

**Root cause:** `esp0` is hardcoded to `0x1FFF0`. The actual kernel stack is the symbol `stack_top` defined in `boot.S`.

**The fix:**
```c
// In boot.S, export the symbol:
.global stack_top

// In gdt.c, use it:
extern uint32_t stack_top;

void gdt_initialize(void) {
    // ...
    tss.esp0 = (uint32_t)&stack_top;
    tss.ss0  = 0x10; // kernel data segment selector
    // ...
}
```

> **Learning note:** The TSS (Task State Segment) is the CPU's way of finding a safe stack during privilege-level transitions. You don't need it until you implement user mode, but setting it correctly now costs nothing.

---

### Bug 5 — Keyboard release events print garbage

**File:** `arch/i386/interrupts.c`, `irq1_handler`  
**Why it matters:** Holding or releasing a key sends a scancode with bit 7 set (value ≥ 0x80). The current code may look these up in `keyboard_map` and print a random character.

**Root cause:** The check `if(keycode < 0) return;` tests the wrong thing. `keycode` is the raw scancode from port 0x60. Key-release events have bit 7 set (value 128–255), which are valid `unsigned char` values but negative when stored in a `signed char`.

**The fix:**
```c
void irq1_handler(void) {
    uint8_t scancode = inb(0x60);

    // Bit 7 set = key release event, ignore it
    if (scancode & 0x80)
        return;

    if (scancode >= sizeof(keyboard_map))
        return;

    char c = keyboard_map[scancode];
    if (c != 0)
        terminal_write(&c, 1);
}
```

> **Learning note:** Always use explicit `uint8_t` / `int8_t` for values coming from hardware ports. The sign of a plain `char` is implementation-defined, which causes exactly this class of bug.

---

## 3. Code Improvements

These are not crashes but they make the code harder to maintain and extend.

---

### Improvement 1 — Remove magic numbers from GDT and IDT setup

Right now `gdt.c` contains raw bytes like `0x9B`, `0x93`, `0xCF`. These are access-right and flag fields packed into a byte. They are impossible to understand without reading the Intel manual alongside.

**Replace with bit-field macros:**

```c
// include/kernel/gdt.h  — add these
#define GDT_ACCESS_PRESENT    (1 << 7)
#define GDT_ACCESS_RING0      (0 << 5)
#define GDT_ACCESS_RING3      (3 << 5)
#define GDT_ACCESS_EXECUTABLE (1 << 3)
#define GDT_ACCESS_READWRITE  (1 << 1)
#define GDT_FLAG_32BIT        (1 << 6)  // in flags nibble
#define GDT_FLAG_GRANULARITY  (1 << 7)  // 4 KiB pages

// Kernel code:  present | ring0 | executable | readable
#define GDT_KERNEL_CODE (GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | \
                         GDT_ACCESS_EXECUTABLE | GDT_ACCESS_READWRITE)
```

---

### Improvement 2 — Collapse the 16 near-identical IRQ stubs

The assembly for IRQ 0–15 is copy-pasted with one number changing. Use a macro:

```asm
.macro IRQ_STUB num, handler
irq\num:
    pusha
    call \handler
    popa
    iret
.endm

IRQ_STUB 0,  irq0_handler
IRQ_STUB 1,  irq1_handler
// ... etc.
```

And the IDT registration loop:

```c
// An array of handler pointers makes this a loop, not 16 lines:
static void (*irq_handlers[])(void) = {
    irq0_handler, irq1_handler, /* ... */ irq15_handler
};

for (int i = 0; i < 16; i++)
    idt_set_gate(32 + i, (uint32_t)irq_stubs[i], 0x08, 0x8E);
```

---

### Improvement 3 — Add `%x` (hex) to printf

Hex output is essential for OS debugging — every address, port, and register value is easiest to read in hex.

```c
case 'x': {
    unsigned int val = va_arg(parameters, unsigned int);
    char buf[9]; // 8 hex digits + null
    int len = 0;
    if (val == 0) { putchar('0'); break; }
    while (val > 0) {
        int digit = val & 0xF;
        buf[len++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
        val >>= 4;
    }
    for (int i = len - 1; i >= 0; i--)
        putchar(buf[i]);
    break;
}
```

> Once you have `%x`, add a helper: `kprintf("irq: vector=0x%x\n", vector)`. This alone will save hours of debugging.

---

### Improvement 4 — Add `VGA_WIDTH` / `VGA_HEIGHT` constants to `vga.h`

`tty.c` hardcodes `80` and `25` in multiple places. Add to `vga.h`:

```c
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
```

This makes it trivial to test with a different resolution later.

---

### Improvement 5 — `terminal_initialize` should clear the whole screen

Right now the initializer only sets `terminal_row = 0`. The screen memory at `0xB8000` may contain leftover data from BIOS/bootloader. Explicitly clear it:

```c
void terminal_initialize(void) {
    terminal_row    = 0;
    terminal_column = 0;
    terminal_color  = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_buffer[y * VGA_WIDTH + x] =
                vga_entry(' ', terminal_color);

    enable_cursor(0, 15);
    update_cursor(0, 0);
}
```

---

## 4. Next Steps — Ordered Learning Path

These are ordered so that each step builds on the previous one. Do not skip ahead — for example, you cannot implement user mode before paging, and you cannot implement paging without a physical memory manager.

---

### 4.1 CPU Exception Handlers

**Do this before anything else.** Without these, any bug in future code causes a silent reboot.

**Background:** The Intel x86 CPU defines 32 reserved interrupt vectors (0–31) for internal faults. Examples:

| Vector | Name | Caused by |
|--------|------|-----------|
| 0 | Divide Error | `x / 0` |
| 6 | Invalid Opcode | Executing a bad instruction |
| 8 | Double Fault | Exception while handling exception |
| 13 | General Protection Fault | Segmentation/privilege violation |
| 14 | Page Fault | Accessing unmapped memory |

**Implementation:**

```c
// arch/i386/exceptions.c
static const char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    // ... fill in all 32
    "Page Fault",
};

// A "registers" struct that mirrors what the CPU pushes on the stack
typedef struct {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; // pusha
    uint32_t int_no, err_code;                        // pushed by stub
    uint32_t eip, cs, eflags, useresp, ss;            // pushed by CPU
} registers_t;

void exception_handler(registers_t *regs) {
    kprintf("\r\n--- KERNEL PANIC ---\r\n");
    kprintf("Exception: %s (vector %d)\r\n",
            exception_messages[regs->int_no], regs->int_no);
    kprintf("EIP=0x%x  CS=0x%x  EFLAGS=0x%x\r\n",
            regs->eip, regs->cs, regs->eflags);
    kprintf("EAX=0x%x  EBX=0x%x  ECX=0x%x  EDX=0x%x\r\n",
            regs->eax, regs->ebx, regs->ecx, regs->edx);
    for(;;) asm("hlt"); // halt
}
```

The assembly stubs for exceptions are slightly different from IRQ stubs because some exceptions push an error code onto the stack and some do not. You need two stub templates:

```asm
// Exception WITHOUT error code (e.g. divide-by-zero)
.macro EXCEPTION_STUB_NOERR num
isr\num:
    push $0          // fake error code for uniform struct
    push $\num       // interrupt number
    jmp exception_common_stub
.endm

// Exception WITH error code (e.g. page fault)
.macro EXCEPTION_STUB_ERR num
isr\num:
    // CPU already pushed error code
    push $\num
    jmp exception_common_stub
.endm

exception_common_stub:
    pusha
    mov  %ds, %ax
    push %eax        // save data segment
    mov  $0x10, %ax  // load kernel data segment
    mov  %ax, %ds
    mov  %ax, %es
    push %esp        // pointer to registers_t
    call exception_handler
    // (we never return from a panic, but if you want to:)
    pop  %eax
    pop  %eax
    mov  %ax, %ds
    mov  %ax, %es
    popa
    add  $8, %esp    // remove int_no and err_code
    iret
```

> **Learning note:** The CPU automatically pushes EIP, CS, EFLAGS (and optionally SS, ESP for ring changes) before calling your handler. Some exceptions also push a 32-bit error code. The `registers_t` struct must exactly match what is on the stack when your handler runs. Getting this wrong means reading corrupted register values.

---

### 4.2 Physical Memory Manager (PMM)

**Background:** When the kernel runs, RAM is just a flat array of bytes. You need to track which pages (4 KiB blocks) are free and which are used. The bootloader's memory map (passed via Multiboot) tells you which regions are safe to use.

**Concept — Bitmap allocator:** Represent every physical 4 KiB page as one bit. 0 = free, 1 = used. For 128 MiB of RAM you need 128 MiB / 4 KiB = 32768 pages = 4096 bytes = 4 KiB for the bitmap itself. This is small enough to store statically.

```c
// include/kernel/pmm.h
#define PAGE_SIZE 4096

void pmm_initialize(uint32_t mem_upper); // called from kernel_main
void *pmm_alloc_page(void);              // returns physical address or NULL
void  pmm_free_page(void *page);
```

```c
// kernel/pmm.c
#define MAX_PAGES (1024 * 1024 / 4)    // 1 GiB / 4 KiB = 262144 pages
static uint32_t bitmap[MAX_PAGES / 32]; // one bit per page

static void pmm_set(uint32_t page)   { bitmap[page/32] |=  (1 << (page%32)); }
static void pmm_clear(uint32_t page) { bitmap[page/32] &= ~(1 << (page%32)); }
static int  pmm_test(uint32_t page)  { return bitmap[page/32] & (1 << (page%32)); }

void *pmm_alloc_page(void) {
    for (int i = 0; i < MAX_PAGES; i++) {
        if (!pmm_test(i)) {
            pmm_set(i);
            return (void*)(uintptr_t)(i * PAGE_SIZE);
        }
    }
    return NULL; // out of memory
}
```

**Reading the Multiboot memory map:**

The Multiboot info struct (passed in `ebx` by GRUB) contains a `mmap_addr`/`mmap_length` pair. Iterate it and call `pmm_clear` for each `MULTIBOOT_MEMORY_AVAILABLE` region, then `pmm_set` the regions occupied by the kernel itself.

> **Learning note:** You must mark kernel pages as *used* in the bitmap before you start allocating, otherwise a future allocation could return a page that the kernel's own code or data lives on.

---

### 4.3 Paging

**Background:** x86 protected mode lets the CPU translate *virtual* addresses to *physical* addresses using a two-level page directory/table structure. Every process gets its own virtual address space, which is fundamental to memory isolation.

**Why you need it:**
- Kernel/user isolation — user programs cannot access kernel memory
- Each process sees the same virtual address range (e.g. 0x00000000–0xBFFFFFFF)
- Guard pages — a page marked "not present" causes a page fault instead of silent corruption

**Structure:**
```
CR3 register → Page Directory (1024 × 4-byte entries)
                  each entry → Page Table (1024 × 4-byte entries)
                                  each entry → 4 KiB physical page
```

**Steps to enable paging:**

1. Allocate a page directory (one physical page).
2. Identity-map the first 4 MiB so the kernel keeps running after `cr0` is set.
3. Set `cr3` to point to the page directory.
4. Set bit 31 (`PG`) in `cr0`.
5. Your page fault handler (vector 14) must now handle faults gracefully.

```c
// arch/i386/paging.c  — skeleton
#define PAGE_PRESENT   (1 << 0)
#define PAGE_WRITABLE  (1 << 1)
#define PAGE_USER      (1 << 2)

static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t first_page_table[1024] __attribute__((aligned(4096)));

void paging_initialize(void) {
    // Identity map first 4 MiB
    for (int i = 0; i < 1024; i++)
        first_page_table[i] = (i * PAGE_SIZE) | PAGE_PRESENT | PAGE_WRITABLE;

    page_directory[0] = ((uint32_t)first_page_table) | PAGE_PRESENT | PAGE_WRITABLE;

    asm volatile(
        "mov %0, %%cr3\n"
        "mov %%cr0, %%eax\n"
        "or  $0x80000000, %%eax\n"
        "mov %%eax, %%cr0\n"
        :: "r"(page_directory) : "eax"
    );
}
```

> **Learning note:** After enabling paging, every address the CPU sees is a *virtual* address. Until you set up higher-half mapping, your kernel must run from physical addresses that are identity-mapped (virtual == physical). This is why the kernel is loaded at 1 MiB — straightforward to identity-map.

---

### 4.4 Heap Allocator (`kmalloc` / `kfree`)

Once you have the PMM and paging, you can build a kernel heap.

**Simplest approach — bump allocator (no free):**

```c
static uint8_t *heap_ptr = (uint8_t *)0x200000; // start after kernel

void *kmalloc(size_t size) {
    void *result = heap_ptr;
    heap_ptr += (size + 7) & ~7; // 8-byte align
    return result;
}
```

This never frees memory, but it is enough to bootstrap more complex structures.

**Next step — linked-list free list:**

Each allocation starts with a small header:
```c
typedef struct block_header {
    size_t size;
    int    is_free;
    struct block_header *next;
} block_header_t;
```

`kmalloc` walks the list for a free block of sufficient size. `kfree` marks a block free and coalesces adjacent free blocks. This is the classic K&R allocator.

> **Learning note:** A real allocator (like `dlmalloc`) is extremely complex. For a hobby OS, a simple first-fit linked list is fine. You will rewrite it several times as you learn its limits — that is normal.

---

### 4.5 Serial Port Logging (UART)

**Why:** QEMU can redirect the serial port to your host terminal (`-serial stdio`). This gives you a log stream that works even before the VGA driver is initialized, and survives kernel panics that corrupt the screen.

```c
// arch/i386/serial.c
#define COM1 0x3F8

void serial_initialize(void) {
    outb(COM1 + 1, 0x00); // disable interrupts
    outb(COM1 + 3, 0x80); // enable DLAB (baud rate divisor)
    outb(COM1 + 0, 0x03); // divisor low  = 3 → 38400 baud
    outb(COM1 + 1, 0x00); // divisor high
    outb(COM1 + 3, 0x03); // 8 bits, no parity, 1 stop bit
    outb(COM1 + 2, 0xC7); // enable FIFO
    outb(COM1 + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

void serial_putchar(char c) {
    while (!(inb(COM1 + 5) & 0x20)); // wait until transmit buffer empty
    outb(COM1, c);
}
```

Add `-serial stdio` to your QEMU command and call `serial_putchar` from your `kprintf`.

---

### 4.6 Keyboard Input Buffer

Right now keystrokes are printed directly inside the IRQ handler. This is fragile — you cannot use the keyboard to drive a shell because there is no way to read typed characters from application code.

**Add a ring buffer:**

```c
// include/kernel/keyboard.h
#define KEYBOARD_BUFFER_SIZE 256

void keyboard_initialize(void);
char keyboard_getchar(void);    // blocks until a key is pressed
int  keyboard_available(void);  // returns number of chars in buffer
```

```c
// kernel/keyboard.c
static char    kb_buffer[KEYBOARD_BUFFER_SIZE];
static uint8_t kb_read_pos  = 0;
static uint8_t kb_write_pos = 0;

// Called from IRQ1 handler instead of printing directly
void keyboard_handle_scancode(uint8_t scancode) {
    if (scancode & 0x80) return; // key release
    char c = keyboard_map[scancode];
    if (c == 0) return;
    kb_buffer[kb_write_pos++ % KEYBOARD_BUFFER_SIZE] = c;
}

char keyboard_getchar(void) {
    while (kb_read_pos == kb_write_pos); // spin wait
    return kb_buffer[kb_read_pos++ % KEYBOARD_BUFFER_SIZE];
}
```

> **Learning note:** Spin-waiting is inefficient (wastes CPU cycles). Later, replace it with a blocking wait that puts the current task to sleep until data arrives. For now it is fine.

---

### 4.7 A Simple Shell

With a keyboard buffer and a terminal, you can build a read-eval-print loop:

```c
void shell_run(void) {
    char line[256];
    int  pos = 0;

    kprintf("quilon> ");

    while (1) {
        char c = keyboard_getchar();

        if (c == '\n') {
            line[pos] = '\0';
            shell_execute(line);
            pos = 0;
            kprintf("\r\nquilon> ");
        } else if (c == '\b' && pos > 0) {
            pos--;
            kprintf("\b \b");
        } else if (pos < 255) {
            line[pos++] = c;
            terminal_write(&c, 1);
        }
    }
}

void shell_execute(const char *cmd) {
    if (strcmp(cmd, "help") == 0)
        kprintf("Commands: help, clear, halt\n");
    else if (strcmp(cmd, "clear") == 0)
        terminal_initialize();
    else if (strcmp(cmd, "halt") == 0)
        asm("hlt");
    else if (cmd[0] != '\0')
        kprintf("Unknown command: %s\n", cmd);
}
```

> **Learning note:** This also means you need `strcmp` in your string library. Add it — it is three lines.

---

### 4.8 Programmable Interval Timer (PIT) and Scheduler

**Background:** The PIT (Intel 8253/8254) fires IRQ0 at a configurable rate. This is the heartbeat of any preemptive multitasking OS.

**Configure the PIT:**
```c
// arch/i386/pit.c
#define PIT_FREQUENCY 1193180  // Hz, hardware constant

void pit_initialize(uint32_t frequency) {
    uint16_t divisor = PIT_FREQUENCY / frequency;
    outb(0x43, 0x36);                  // command: channel 0, square wave
    outb(0x40, divisor & 0xFF);        // low byte
    outb(0x40, (divisor >> 8) & 0xFF); // high byte
}
```

Call `pit_initialize(100)` for 100 Hz (10 ms tick).

**Round-robin scheduler (concept):**

```c
typedef struct task {
    uint32_t esp;       // saved stack pointer
    uint32_t cr3;       // page directory (future)
    int      state;     // RUNNING, READY, BLOCKED
} task_t;

// IRQ0 handler calls:
void schedule(void) {
    // 1. Save current task's ESP (from registers pushed on stack)
    // 2. Pick next READY task (round robin)
    // 3. Restore next task's ESP
    // 4. If different CR3, reload CR3
    // 5. Return — iret pops the new task's EIP
}
```

> **Learning note:** Context switching is one of the most conceptually tricky parts of OS development. The key insight: `iret` pops EIP, CS, and EFLAGS from the stack. If you arrange the stack to look like an interrupted task, `iret` will "return" to a completely different function. This is how you switch between tasks.

---

### 4.9 User Mode (Ring 3)

**Background:** x86 has four privilege levels (rings). Ring 0 is kernel mode; ring 3 is user mode. User-mode code cannot execute privileged instructions or access kernel memory. This is the fundamental security boundary.

**Steps to jump to ring 3:**

1. Set up TSS correctly (esp0 and ss0 must point to the kernel stack).
2. Use `iret` with user-mode CS/SS selectors (RPL field = 3) and a user-mode EIP.

```asm
; Jump to user mode — artificial iret
push $0x23        ; SS  = user data selector (GDT index 3, RPL 3)
push $USER_STACK  ; ESP = user stack
push $0x202       ; EFLAGS (IF set)
push $0x1B        ; CS  = user code selector (GDT index 1, RPL 3)
push $user_func   ; EIP
iret
```

> **Learning note:** The GDT selectors encode both the descriptor index and the requested privilege level (RPL) in the low 2 bits. `0x1B = 0b00011011` = index 3, TI=0 (GDT), RPL=3.

---

### 4.10 System Calls

Once you have user mode, user-mode code needs a controlled way to ask the kernel to do things (write to screen, allocate memory, etc.). This is the system call interface.

**Simplest approach — software interrupt:**

```c
// User side (in user space):
void sys_write(const char *msg) {
    asm volatile("int $0x80" :: "a"(1), "b"(msg));
    //                           eax=syscall#  ebx=arg
}

// Kernel side:
void syscall_handler(registers_t *regs) {
    switch (regs->eax) {
        case 1: terminal_write((char*)regs->ebx, strlen((char*)regs->ebx)); break;
        case 2: /* exit */ break;
        default: kprintf("Unknown syscall %d\n", regs->eax);
    }
}
```

Register `int 0x80` in the IDT with DPL=3 so user-mode code can call it.

---

### 4.11 Filesystem

The simplest filesystem to implement is a RAM disk with a flat file layout. A more realistic first step is reading from an ATA hard disk.

**Suggested order:**
1. Implement an ATA PIO driver (reads 512-byte sectors from port I/O).
2. Parse a FAT16 partition (widely documented, simple structure).
3. Implement VFS (Virtual File System) — an abstraction layer so code above doesn't care whether it's FAT, ext2, or a RAM disk.

The OSDev Wiki has a full ATA PIO tutorial. This is a significant milestone — reading a real file off disk is a major achievement for a hobby OS.

---

### 4.12 ELF Loader

Once you have a filesystem and user mode, you can load and run programs:

1. Read an ELF binary from disk.
2. Parse the ELF header to find `LOAD` segments.
3. Allocate virtual memory pages for each segment.
4. Copy segment data from the file.
5. Jump to the ELF entry point in user mode.

This is the point where Quilon becomes a real OS — running programs that were compiled independently of the kernel.

---

## 5. Reference Reading

These are the canonical resources for x86 OS development.

| Resource | What it covers |
|----------|----------------|
| **OSDev Wiki** (wiki.osdev.org) | Everything. Start with the "Bare Bones" tutorial, then the category pages for each subsystem. |
| **Intel 64 and IA-32 Architectures Software Developer's Manual** | The authoritative reference for x86 instructions, GDT, IDT, paging, TSS. Vol 3A covers protected-mode architecture. Free PDF from Intel. |
| **"Writing a Simple Operating System from Scratch"** by Nick Blundell | Excellent beginner-oriented PDF. Covers bootloader, protected mode, C kernel. |
| **The Little Book About OS Development** | Thorough, hands-on. Covers paging, interrupts, filesystem. Free online. |
| **"Operating Systems: Three Easy Pieces"** by Arpaci-Dusseau | Teaches OS concepts (processes, memory, storage) at a high level. Free PDF. Great companion to hands-on work. |
| **James Molloy's kernel tutorial** (archived) | Walks through a very similar kernel in depth. Some bugs in the original, but the explanations are superb. |
| **QEMU monitor** (`Ctrl-Alt-2`) | `info registers`, `x/10x 0xb8000`, `info mem` — invaluable for debugging without a physical machine. |

---

*Document generated for Quilon OS — April 2026.*
