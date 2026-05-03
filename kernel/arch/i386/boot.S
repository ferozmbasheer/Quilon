.set KERNEL_OFFSET, 0xC0000000
.set KERNEL_PD_IDX, 768           /* 0xC0000000 >> 22 */

# Declare constants for the multiboot header.
.set ALIGN,    1<<0             # align loaded modules on page boundaries
.set MEMINFO,  1<<1             # provide memory map
.set FLAGS,    ALIGN | MEMINFO  # this is the Multiboot 'flag' field
.set MAGIC,    0x1BADB002       # 'magic number' lets bootloader find the header
.set CHECKSUM, -(MAGIC + FLAGS) # checksum of above, to prove we are multiboot

# Declare a header as in the Multiboot Standard.
.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

# Reserve a stack for the initial thread.
.section .bss
.align 16
stack_bottom:
.skip 16384 # 16 KiB
.global stack_top
stack_top:

# Temporary boot page directory (4-MiB PSE entries, zeroed in _start).
.align 4096
.global boot_pd
boot_pd:
.skip 4096

# Storage for the Multiboot info pointer passed by GRUB in %ebx.
.section .data
.global multiboot_info_ptr
multiboot_info_ptr:
    .long 0

# The kernel entry point.
.section .text
.global _start
.type _start, @function
_start:
    /* Paging is OFF here; all addresses must be physical. */

    /* 1. Temporary stack at physical address (stack_top is linked at high VA). */
    movl $(stack_top - KERNEL_OFFSET), %esp

    /* 2. Save GRUB's Multiboot info pointer to its physical address. */
    movl %ebx, (multiboot_info_ptr - KERNEL_OFFSET)

    /* 3. Enable PSE (4-MiB pages) in CR4 bit 4. */
    movl %cr4, %eax
    orl  $0x10, %eax
    movl %eax, %cr4

    /* 4. Zero the boot page directory (4096 bytes = 1024 dwords). */
    movl $(boot_pd - KERNEL_OFFSET), %edi
    movl $1024, %ecx
    xorl %eax, %eax
    rep  stosl

    /* 5. PD[0]: identity-map 0x00000000-0x003FFFFF (PS=1, W=1, P=1). */
    movl $0x00000083, (boot_pd - KERNEL_OFFSET)

    /* 6. PD[768]: map 0xC0000000-0xC03FFFFF -> phys 0x00000000 (kernel high). */
    movl $0x00000083, (boot_pd - KERNEL_OFFSET + KERNEL_PD_IDX * 4)

    /* 7. Load physical address of boot_pd into CR3. */
    movl $(boot_pd - KERNEL_OFFSET), %eax
    movl %eax, %cr3

    /* 8. Enable paging: set CR0 bit 31 (PG). */
    movl %cr0, %eax
    orl  $0x80000000, %eax
    movl %eax, %cr0

    /* 9. Jump to high virtual address.
     *    EIP is still in the identity-mapped low region; the 'lea' gives the
     *    high linked address of _start_high, then jmp *%eax crosses to it.  */
    lea  _start_high, %eax
    jmp  *%eax

.global _start_high
_start_high:
    /* Now running at high virtual address ~0xC0100000. */

    /* 10. Switch stack pointer to the high virtual address. */
    addl $KERNEL_OFFSET, %esp

    /* 11. Call the global constructors. */
    call _init

    /* 12. Transfer control to the main kernel. */
    call kernel_main

    /* Hang if kernel_main unexpectedly returns. */
    cli
1:  hlt
    jmp 1b
.size _start, . - _start

.global inb

inb:
	mov 4(%esp), %edx
	mov 8(%esp), %al
	in %dx, %al
	ret

.global outb

outb:
	mov 4(%esp), %edx
	mov 8(%esp), %al
	out %al, %dx
	ret

/* ── CPU Exception stubs (vectors 0-31) ─────────────────────────────────── */
/* Exceptions without a CPU-pushed error code push a fake zero so every stub
 * leaves the stack in the same layout for exception_common_stub.         */

.macro EXCEPTION_NOERR num
.global isr\num
isr\num:
    push $0
    push $\num
    jmp exception_common_stub
.endm

.macro EXCEPTION_ERR num
.global isr\num
isr\num:
    push $\num
    jmp exception_common_stub
.endm

EXCEPTION_NOERR 0
EXCEPTION_NOERR 1
EXCEPTION_NOERR 2
EXCEPTION_NOERR 3
EXCEPTION_NOERR 4
EXCEPTION_NOERR 5
EXCEPTION_NOERR 6
EXCEPTION_NOERR 7
EXCEPTION_ERR   8
EXCEPTION_NOERR 9
EXCEPTION_ERR   10
EXCEPTION_ERR   11
EXCEPTION_ERR   12
EXCEPTION_ERR   13
EXCEPTION_ERR   14
EXCEPTION_NOERR 15
EXCEPTION_NOERR 16
EXCEPTION_ERR   17
EXCEPTION_NOERR 18
EXCEPTION_NOERR 19
EXCEPTION_NOERR 20
EXCEPTION_NOERR 21
EXCEPTION_NOERR 22
EXCEPTION_NOERR 23
EXCEPTION_NOERR 24
EXCEPTION_NOERR 25
EXCEPTION_NOERR 26
EXCEPTION_NOERR 27
EXCEPTION_NOERR 28
EXCEPTION_NOERR 29
EXCEPTION_ERR   30
EXCEPTION_NOERR 31

exception_common_stub:
    pusha
    mov  %ds, %ax
    push %eax
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    push %esp
    call exception_handler
    pop  %eax
    pop  %eax
    mov  %ax, %ds
    mov  %ax, %es
    popa
    add  $8, %esp
    iret

/* ── IRQ stubs (hardware interrupts, vectors 32-47) ────────────────────── */
/* Each stub saves all registers, calls the C handler, restores, and irets. */

.macro IRQ_STUB num
.global irq\num
irq\num:
    pusha
    call irq\num\()_handler
    popa
    iret
.endm

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

.global load_idt

load_idt:
	mov 4(%esp), %edx
	lidt (%edx)
	sti
	ret

/* ── exec_setjmp / exec_longjmp ─────────────────────────────────────────── */
/*
 * Minimal setjmp/longjmp used by shell_cmd_exec to resume the shell after a
 * user program calls SYS_EXIT.
 *
 * exec_jmp_buf layout (6 × uint32_t, 24 bytes):
 *   [0]  EBX
 *   [4]  ESI
 *   [8]  EDI
 *   [12] EBP
 *   [16] ESP  (caller's stack pointer, i.e. ESP+4 on entry = return address slot)
 *   [20] EIP  (return address, i.e. the word at (%esp) on entry)
 *
 * Only callee-saved registers need to be preserved; caller-saved registers
 * (EAX, ECX, EDX) are clobbered across any call boundary anyway.
 */

.global exec_setjmp
exec_setjmp:
    movl  4(%esp), %eax        /* buf = first argument                         */
    movl  (%esp),  %ecx        /* return address (EIP to restore)              */
    movl  %ecx,  20(%eax)      /* save EIP                                     */
    leal  4(%esp), %ecx        /* caller's ESP (after this call returns)       */
    movl  %ecx,  16(%eax)      /* save ESP                                     */
    movl  %ebx,   0(%eax)      /* save EBX                                     */
    movl  %esi,   4(%eax)      /* save ESI                                     */
    movl  %edi,   8(%eax)      /* save EDI                                     */
    movl  %ebp,  12(%eax)      /* save EBP                                     */
    xorl  %eax,  %eax          /* return 0 from exec_setjmp                    */
    ret

.global exec_longjmp
exec_longjmp:
    movl  4(%esp), %edx        /* buf                                          */
    movl  8(%esp), %eax        /* val                                          */
    testl %eax, %eax
    jnz   .Lexec_longjmp_nonzero
    movl  $1, %eax             /* longjmp(buf,0) returns 1, never 0            */
.Lexec_longjmp_nonzero:
    movl   0(%edx), %ebx       /* restore EBX                                  */
    movl   4(%edx), %esi       /* restore ESI                                  */
    movl   8(%edx), %edi       /* restore EDI                                  */
    movl  12(%edx), %ebp       /* restore EBP                                  */
    movl  16(%edx), %esp       /* restore ESP (jumps back to caller's frame)   */
    jmp  *20(%edx)             /* jump to saved EIP — exec_setjmp "returns" 1  */

/* ── Context switch (section 5.3) ───────────────────────────────────────── */
/*
 * context_switch(prev_esp_ptr, next_esp_val)
 *
 * Saves callee-saved registers (edi, esi, ebx, ebp) plus the kernel stack
 * pointer into *prev_esp_ptr, then restores those registers from the frame
 * that starts at next_esp_val and returns into the new process's context.
 *
 * For an EXISTING process:  next_esp_val points to the frame saved the last
 * time that process was suspended here.  The ret returns to wherever that
 * process was inside scheduler_tick / scheduler_yield.
 *
 * For a NEW process: next_esp_val points to the initial frame built by
 * process_create().  The ret jumps to process_first_run (below), which
 * calls process_launch() → usermode_enter() → iret into ring 3.
 *
 * Calling convention (cdecl):
 *   [esp+ 0]  return address
 *   [esp+ 4]  prev_esp_ptr   — pointer to prev->kernel_esp
 *   [esp+ 8]  next_esp_val   — value of next->kernel_esp
 */
.global context_switch
context_switch:
    push %ebp
    push %ebx
    push %esi
    push %edi
    /*
     * Stack layout after 4 pushes (offsets from current ESP):
     *   + 0  edi
     *   + 4  esi
     *   + 8  ebx
     *   +12  ebp
     *   +16  return address (back into scheduler_tick / scheduler_yield)
     *   +20  prev_esp_ptr
     *   +24  next_esp_val
     */
    mov  20(%esp), %eax          /* eax = prev_esp_ptr                   */
    mov  %esp,    (%eax)         /* *prev_esp_ptr = current ESP (save)   */
    mov  24(%esp), %esp          /* ESP = next_esp_val   (stack switch)  */
    /*
     * We are now on the new process's kernel stack.
     * Its layout must mirror what we just pushed above.
     */
    pop  %edi
    pop  %esi
    pop  %ebx
    pop  %ebp
    ret                          /* jump into the new process's context  */

/*
 * process_first_run — trampoline for a brand-new process's first scheduling.
 *
 * process_create() plants the address of this label as the "return address"
 * in the initial kernel stack frame.  When context_switch's ret fires for
 * that process the very first time, execution arrives here.
 *
 * We simply tail-call process_launch(), which updates the TSS kernel stack
 * pointer, calls usermode_initialize(), and does iret into ring 3.
 * It is declared noreturn, so the hlt below is a safety net only.
 */
.global process_first_run
process_first_run:
    call process_launch
1:  hlt
    jmp  1b

/* ── fork() child return trampoline (section 6.2) ───────────────────────── */
/*
 * fork_child_return — entered by context_switch when a fork() child is first
 * scheduled.  At this point, the child's kernel stack contains the full
 * int80_stub return frame (copied from the parent by SYS_FORK), with EAX
 * already patched to 0 (fork() returns 0 in the child).
 *
 * Stack layout on entry (ESP points at the saved DS word):
 *   [esp+ 0]  ds          (saved caller segment)
 *   [esp+ 4]  edi         \
 *   [esp+ 8]  esi          |
 *   [esp+12]  ebp          |  pusha save area
 *   [esp+16]  esp_snap     |  (popa discards this slot)
 *   [esp+20]  ebx          |
 *   [esp+24]  edx          |
 *   [esp+28]  ecx          |
 *   [esp+32]  eax = 0     /  ← fork() returns 0 to child
 *   [esp+36]  int_no (0x80)
 *   [esp+40]  err_code (0)
 *   [esp+44]  eip          \
 *   [esp+48]  cs            |  CPU iret frame
 *   [esp+52]  eflags        |
 *   [esp+56]  useresp      /
 *   [esp+60]  ss
 *
 * This is identical to the tail of int80_stub, so we expose it as a global
 * label there rather than duplicating the code.
 */
.global fork_child_return

/* ── int $0x80 system call stub ─────────────────────────────────────────── */
/*
 * Entry point for `int $0x80` system calls.  Registered in the IDT with
 * DPL=3 by syscall_initialize() so ring-3 code may invoke it.
 *
 * Stack on entry (ring-3 → ring-0 transition, CPU pushes 5 items):
 *   ss       ← user stack segment
 *   useresp  ← user stack pointer
 *   eflags
 *   cs       ← user code segment (RPL=3 tells iret to restore ss/esp)
 *   eip      ← return address in user code
 *
 * This stub:
 *   1. Pushes a fake err_code (0) and int_no (0x80) for a uniform layout.
 *   2. Saves all general-purpose registers with pusha.
 *   3. Saves DS and switches to the kernel data segment (0x10).
 *   4. Passes a pointer to the save area to syscall_handler.
 *   5. Restores DS, all registers, and irets back to ring 3.
 *
 * Return value: syscall_handler writes the result into regs->eax (the pusha
 * slot) before returning.  The subsequent popa restores EAX from that slot,
 * so the caller's EAX holds the syscall return value after `int $0x80`.
 */
.global int80_stub
int80_stub:
    push $0          /* fake err_code — keeps layout identical to exceptions */
    push $0x80       /* int_no                                                */
    pusha
    mov  %ds, %ax
    push %eax        /* save caller's data segment                            */
    mov  $0x10, %ax  /* switch to kernel data segment                         */
    mov  %ax, %ds
    mov  %ax, %es
    push %esp        /* argument: pointer to syscall_regs_t                   */
    call syscall_handler
    add  $4, %esp    /* remove the esp-pointer argument                       */
fork_child_return:   /* ← fork() child's first scheduling resumes here        */
    pop  %eax        /* pop saved ds value into eax (clobbers, then popa fix) */
    mov  %ax, %ds   /* restore caller's data segment                          */
    mov  %ax, %es
    popa             /* restores all regs including eax = syscall return value */
    add  $8, %esp    /* remove int_no and err_code                             */
    iret             /* restores eip, cs, eflags, useresp, ss (ring-3 return)  */
