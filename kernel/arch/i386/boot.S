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

# Storage for the Multiboot info pointer passed by GRUB in %ebx.
# Saved before _init so the stack stays 16-byte aligned for kernel_main.
.section .data
.global multiboot_info_ptr
multiboot_info_ptr:
    .long 0

# The kernel entry point.
.section .text
.global _start
.type _start, @function
_start:
	movl $stack_top, %esp

	# Save the Multiboot info pointer before calling _init.
	movl %ebx, multiboot_info_ptr

	# Call the global constructors.
	call _init

	# Transfer control to the main kernel.
	call kernel_main

	# Hang if kernel_main unexpectedly returns.
	cli
1:	hlt
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
    pop  %eax        /* pop saved ds value into eax (clobbers, then popa fix) */
    mov  %ax, %ds   /* restore caller's data segment                          */
    mov  %ax, %es
    popa             /* restores all regs including eax = syscall return value */
    add  $8, %esp    /* remove int_no and err_code                             */
    iret             /* restores eip, cs, eflags, useresp, ss (ring-3 return)  */
