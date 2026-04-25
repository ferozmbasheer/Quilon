/*
 * Quilon user-space libc — crt0.S
 *
 * C runtime startup stub.  The ELF loader jumps here at e_entry (0x400000).
 *
 * Responsibility:
 *   1. Call main().
 *   2. Pass main's return value to exit().
 *
 * Stack state on entry (set up by usermode_enter / process_launch):
 *   ESP points to the top of the user stack (USER_STACK_SIZE = 4 KiB).
 *   The stack is empty — no argc, argv, or envp has been pushed.
 *
 * We declare main() with `int main(void)` — no argument passing needed.
 * If a program declares `int main(int argc, char **argv)`, argc and argv
 * will be garbage, but that is acceptable for this demo OS.
 */

.section .text
.global _start

_start:
    call  main          /* call C main(); return value in %eax */
    pushl %eax          /* push return value as exit() argument */
    call  exit          /* exit(main_retval) — never returns   */

    /* Safety net: exit() is noreturn, but the assembler doesn't know that. */
.Lhang:
    pause
    jmp   .Lhang
