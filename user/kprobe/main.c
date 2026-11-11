/*
 * kprobe -- helper for ktest (REVIEW.md H).  Must be exec'd, not forked:
 * only exec'd processes went through usermode_initialize(), which used to
 * give them PAGE_USER on the identity map and the kernel-high window.
 *
 * Reads one byte from stdin to pick the target -- 'k': kernel text at
 * 0xC0100000, 'i': the identity map at 0x00100000 -- then loads from it.
 * Exits 0 if the load succeeded (isolation broken), 2 on bad input; the
 * expected outcome is death by SIGSEGV (exit code -11).
 */
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) exit(2);
    uint32_t addr = (c == 'k') ? 0xC0100000u : (c == 'i') ? 0x00100000u : 0;
    if (!addr) exit(2);
    volatile uint32_t v = *(volatile uint32_t *)addr;
    (void)v;
    exit(0);
}
