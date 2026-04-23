#include <string.h>
#include <stdio.h>
#include <kernel/shell.h>
#include <kernel/keyboard.h>
#include <kernel/tty.h>
#include <kernel/pit.h>
#include <kernel/usermode.h>
#include <kernel/vfs.h>
#include <kernel/elf.h>
#include <kernel/paging.h>
#include <kernel/pmm.h>
#include <kernel/process.h>

static void shell_cmd_ls(void)
{
    if (!vfs_mounted()) {
        printf("No filesystem mounted.\r\n");
        return;
    }
    vfs_dirent_t ent;
    uint32_t i = 0;
    while (vfs_readdir(i, &ent) == 0) {
        printf("  %s  (%d bytes)\r\n", ent.name, (int)ent.size);
        i++;
    }
    if (i == 0)
        printf("  (empty)\r\n");
}

static void shell_cmd_exec(const char *path)
{
    if (!vfs_mounted()) {
        printf("No filesystem mounted.\r\n");
        return;
    }
    if (path[0] == '\0') {
        printf("Usage: exec <file.elf>\r\n");
        return;
    }

    printf("Loading ELF '%s' into isolated address space...\r\n", path);

    /*
     * Section 5.1: allocate a fresh page directory for this process.
     * The kernel's first-4-MiB entry is copied in so the kernel remains
     * reachable after we switch to this PD.
     */
    uint32_t *proc_pd = paging_create_address_space();
    if (!proc_pd) {
        printf("exec: out of memory (cannot allocate page directory)\r\n");
        return;
    }

    /*
     * Load the ELF segments into the process's own page directory.
     * Physical pages are allocated from the PMM and mapped only in proc_pd
     * — the global kernel PD is untouched, so a second exec of the same
     * binary will not collide with the first.
     */
    uint32_t entry = elf_load_into(path, proc_pd);
    if (entry == 0) {
        printf("exec: failed to load '%s'\r\n", path);
        pmm_free_page(proc_pd);
        return;
    }

    printf("exec: entry=0x%x  switching to process address space\r\n",
           (unsigned)entry);

    /*
     * Save a return point so SYS_EXIT can longjmp back here instead of
     * halting the CPU.  exec_setjmp returns 0 the first time (direct
     * call) and ≥1 after an exec_longjmp from the SYS_EXIT handler.
     */
    if (exec_setjmp(&exec_return_buf) == 0) {
        exec_return_active = 1;

        /* Switch to the process's isolated address space. */
        paging_switch((uint32_t)(uintptr_t)proc_pd);

        usermode_initialize();
        usermode_enter((void (*)(void))(uintptr_t)entry);
        /* usermode_enter() does iret and never returns to here. */
    }

    /*
     * Reached via exec_longjmp from the SYS_EXIT handler.
     * Restore the kernel's page directory before resuming the shell.
     * STI re-enables hardware interrupts (the longjmp bypassed iret).
     */
    paging_switch((uint32_t)(uintptr_t)paging_get_kernel_pd());
    asm volatile("sti");
    exec_return_active = 0;
    printf("exec: '%s' exited — back in kernel address space\r\n", path);
}

static void shell_cmd_cat(const char *path)
{
    if (!vfs_mounted()) {
        printf("No filesystem mounted.\r\n");
        return;
    }
    int fd = vfs_open(path);
    if (fd < 0) {
        printf("cat: %s: not found\r\n", path);
        return;
    }
    char buf[64];
    int n;
    while ((n = vfs_read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    printf("\r\n");
    vfs_close(fd);
}

static void shell_cmd_ps(void)
{
    static const char *state_names[] = {
        "unused ", "running", "ready  ", "blocked", "zombie "
    };
    printf("  PID  PARENT  STATE    NAME\r\n");
    for (int i = 0; i < PROCESS_MAX; i++) {
        const process_t *p = &process_table[i];
        if (p->state == PROC_UNUSED) continue;
        const char *sname = (p->state < 5) ? state_names[p->state] : "?";
        printf("  %3d  %6d  %s  %s\r\n",
               (int)p->pid, (int)p->parent_pid, sname, p->name);
    }
}

static void shell_execute(const char *cmd) {
    if (strcmp(cmd, "help") == 0) {
        printf("Commands: help, clear, cls, halt, ticks, seconds,\r\n");
        printf("          ring3, syscall, ps, ls, cat <file>, exec <file.elf>\r\n");
    } else if (strcmp(cmd, "clear") == 0) {
        terminal_initialize();
    } else if (strcmp(cmd, "cls") == 0) {
        terminal_initialize();
    } else if (strcmp(cmd, "halt") == 0) {
        printf("Halting.\r\n");
        asm volatile("cli; hlt");
    } else if (strcmp(cmd, "ticks") == 0) {
        printf("%d\r\n", pit_get_ticks());
    } else if (strcmp(cmd, "seconds") == 0) {
        uint32_t hz = pit_get_hz();
        uint32_t secs = (hz > 0) ? pit_get_ticks() / hz : 0;
        printf("%d\r\n", (int)secs);
    } else if (strcmp(cmd, "ring3") == 0) {
        printf("Entering ring 3 (user mode)...\r\n");
        printf("Expect: white-on-green banner on line 2, then a GPF.\r\n");
        usermode_initialize();
        usermode_enter(user_task_demo);
        /* usermode_enter() never returns; the GPF handler halts the CPU. */
    } else if (strcmp(cmd, "syscall") == 0) {
        printf("Entering ring 3 to demo system calls via int $0x80...\r\n");
        printf("Expect: SYS_WRITE output, SYS_GETPID result, then SYS_EXIT halt.\r\n");
        usermode_initialize();
        usermode_enter(user_task_syscall);
        /* usermode_enter() never returns; SYS_EXIT halts the CPU. */
    } else if (strcmp(cmd, "ps") == 0) {
        shell_cmd_ps();
    } else if (strcmp(cmd, "ls") == 0) {
        shell_cmd_ls();
    } else if (strncmp(cmd, "cat ", 4) == 0) {
        shell_cmd_cat(cmd + 4);
    } else if (strncmp(cmd, "exec ", 5) == 0) {
        shell_cmd_exec(cmd + 5);
    } else if (cmd[0] != '\0') {
        printf("Unknown command: %s\r\n", cmd);
    }
}

void shell_run(void) {
    char line[256];
    int  pos = 0;

    printf("quilon> ");

    while (1) {
        char c = keyboard_getchar();

        if (c == '\n') {
            line[pos] = '\0';
            printf("\r\n");
            shell_execute(line);
            pos = 0;
            printf("quilon> ");
        } else if (c == '\b' && pos > 0) {
            pos--;
            printf("\b \b");
        } else if (pos < 255) {
            line[pos++] = c;
            printf("%c", c);
        }
    }
}
