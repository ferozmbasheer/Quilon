/*
 * Quilon OS — hello_c user-space demo (section 7.3)
 *
 * A minimal C program demonstrating the full user-space workflow:
 *   1. Compiled with i686-elf-gcc against the Quilon user libc.
 *   2. Linked with crt0.o so that _start calls main() then exit().
 *   3. Placed on the FAT16 disk image as HELLOC.ELF.
 *   4. Launched from the ring-3 shell:  quilon> exec HELLOC.ELF
 *
 * Features exercised
 * ──────────────────
 *   • printf / puts  — formatted output via SYS_WRITE
 *   • malloc / free  — heap allocator via SYS_SBRK
 *   • strlen / strcmp — user libc string functions
 *   • getpid          — SYS_GETPID syscall
 *   • exit(0)         — SYS_EXIT via crt0 after main() returns
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    printf("\r\n");
    printf("+--------------------------------------------+\r\n");
    printf("|  Hello from C on Quilon! (section 7.3)     |\r\n");
    printf("+--------------------------------------------+\r\n");

    /* SYS_GETPID */
    printf("PID: %d\r\n", getpid());

    /* malloc / free round-trip */
    char *buf = (char *)malloc(64);
    if (buf) {
        /* Use string functions from user libc */
        const char *msg = "Quilon libc malloc works!";
        memcpy(buf, msg, strlen(msg) + 1);
        printf("malloc: \"%s\"\r\n", buf);
        free(buf);
        printf("free: OK\r\n");
    } else {
        printf("malloc: failed\r\n");
    }

    /* String comparison demo */
    const char *a = "hello";
    const char *b = "world";
    printf("strcmp(\"%s\",\"%s\") = %d  (expect <0)\r\n",
           a, b, strcmp(a, b));

    puts("Done — returning to shell.");
    return 0;
}
