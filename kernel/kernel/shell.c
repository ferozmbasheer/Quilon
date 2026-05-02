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
     *  - the global kernel PD is untouched, so a second exec of the same
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
        /* Use the per-process stack mapped by elf_load_into() at
         * USER_STACK_TOP - PAGE_SIZE, not the shared BSS demo stack. */
        usermode_enter_esp((void (*)(void))(uintptr_t)entry, USER_STACK_TOP);
        /* usermode_enter_esp() does iret and never returns to here. */
    }

    /*
     * Reached via exec_longjmp from the SYS_EXIT handler.
     * Restore the kernel's page directory before resuming the shell.
     * STI re-enables hardware interrupts (the longjmp bypassed iret).
     */
    paging_switch((uint32_t)(uintptr_t)paging_get_kernel_pd());
    asm volatile("sti");
    exec_return_active = 0;
    printf("exec: '%s' exited; back in kernel address space\r\n", path);
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

static void shell_cmd_touch(const char *path)
{
    if (!vfs_mounted()) { printf("No filesystem mounted.\r\n"); return; }
    if (path[0] == '\0') { printf("Usage: touch <file>\r\n"); return; }
    if (vfs_create(path) == 0)
        printf("Created '%s'\r\n", path);
    else
        printf("touch: '%s': already exists or disk full\r\n", path);
}

static void shell_cmd_write_file(const char *args)
{
    if (!vfs_mounted()) { printf("No filesystem mounted.\r\n"); return; }

    /* Split "filename content..." at first space. */
    const char *p = args;
    while (*p && *p != ' ') p++;
    if (*p == '\0' || *(p + 1) == '\0') {
        printf("Usage: write <file> <content>\r\n");
        return;
    }

    char fname[VFS_PATH_MAX];
    int flen = (int)(p - args);
    if (flen >= VFS_PATH_MAX) flen = VFS_PATH_MAX - 1;
    int i;
    for (i = 0; i < flen; i++) fname[i] = args[i];
    fname[i] = '\0';

    const char *content = p + 1;
    int clen = 0;
    while (content[clen]) clen++;

    int fd = vfs_open(fname);
    if (fd < 0) { printf("write: '%s': not found\r\n", fname); return; }

    int n = vfs_write(fd, content, (uint32_t)clen);
    vfs_close(fd);

    if (n >= 0)
        printf("Wrote %d bytes to '%s'\r\n", n, fname);
    else
        printf("write: '%s': write failed\r\n", fname);
}

static void shell_cmd_rm(const char *path)
{
    if (!vfs_mounted()) { printf("No filesystem mounted.\r\n"); return; }
    if (path[0] == '\0') { printf("Usage: rm <file>\r\n"); return; }
    if (vfs_remove(path) == 0)
        printf("Removed '%s'\r\n", path);
    else
        printf("rm: '%s': not found\r\n", path);
}

static void shell_cmd_fstest(void)
{
    if (!vfs_mounted()) { printf("No filesystem mounted.\r\n"); return; }

    const char *fname   = "TEST.TXT";
    const char *message = "Hello from FAT16 write!";

    printf("=== FAT16 write demo ===\r\n");

    /* 1. Create */
    printf("1. touch %s ... ", fname);
    if (vfs_create(fname) != 0) { printf("FAILED\r\n"); return; }
    printf("OK\r\n");

    /* 2. Write */
    int fd = vfs_open(fname);
    if (fd < 0) { printf("2. open failed\r\n"); goto cleanup; }
    {
        int msglen = 0;
        while (message[msglen]) msglen++;
        int n = vfs_write(fd, message, (uint32_t)msglen);
        vfs_close(fd);
        printf("2. wrote %d bytes\r\n", n);
        if (n != msglen) goto cleanup;
    }

    /* 3. Read back */
    fd = vfs_open(fname);
    if (fd < 0) { printf("3. re-open failed\r\n"); goto cleanup; }
    {
        char buf[64];
        int n = vfs_read(fd, buf, sizeof(buf) - 1);
        vfs_close(fd);
        if (n > 0) {
            buf[n] = '\0';
            printf("3. read \"%s\"\r\n", buf);
            if (strcmp(buf, message) == 0)
                printf("   content matches\r\n");
            else
                printf("   content MISMATCH\r\n");
        } else {
            printf("3. read failed\r\n");
        }
    }

    /* 4. ls */
    printf("4. directory:\r\n");
    {
        vfs_dirent_t ent;
        uint32_t idx = 0;
        while (vfs_readdir(idx, &ent) == 0) {
            printf("   %s (%d bytes)\r\n", ent.name, (int)ent.size);
            idx++;
        }
    }

cleanup:
    /* 5. Remove */
    printf("5. rm %s ... ", fname);
    printf("%s\r\n", vfs_remove(fname) == 0 ? "OK" : "FAILED");
    printf("=== done ===\r\n");
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

/* Run a ring-3 task via exec_setjmp so the shell regains control when the
 * task calls SYS_EXIT.  Used by the `sbrk` and `fork` shell commands.     */
static void shell_run_ring3_task(void (*task)(void), const char *label)
{
    printf("Running ring-3 task '%s' via exec_setjmp...\r\n", label);

    /*
     * Call usermode_initialize() BEFORE paging_create_address_space() so
     * that page_directory[0] already has PAGE_USER set when it is copied
     * into proc_pd[0].
     *
     * Why this order matters:
     *   paging_create_address_space() copies page_directory[0] verbatim.
     *   usermode_initialize() sets PAGE_USER on page_directory[0]'s PDE
     *   AND on the PTEs in the shared first_page_table[].
     *   If paging_create_address_space() runs first, proc_pd[0] gets a
     *   stale PDE without PAGE_USER  - ring-3 code in the first 4 MiB
     *   triggers a protection-violation page fault (err_code 0x5).
     *
     * usermode_initialize() is idempotent (static initialized guard), so
     * calling it here every time is safe.
     */
    usermode_initialize();

    uint32_t *proc_pd = paging_create_address_space();
    if (!proc_pd) {
        printf("%s: out of memory (cannot allocate page directory)\r\n", label);
        return;
    }

    if (exec_setjmp(&exec_return_buf) == 0) {
        exec_return_active = 1;
        paging_switch((uint32_t)(uintptr_t)proc_pd);
        usermode_enter(task);
    }

    paging_switch((uint32_t)(uintptr_t)paging_get_kernel_pd());
    asm volatile("sti");
    exec_return_active = 0;
    pmm_free_page(proc_pd);
    printf("%s: task exited; back in kernel\r\n", label);
}

static void shell_execute(const char *cmd) {
    if (strcmp(cmd, "help") == 0) {
        printf("Commands: help, clear, cls, halt, ticks, seconds,\r\n");
        printf("          ring3, syscall, sbrk, fork, ps, ls, cat <file>,\r\n");
        printf("          touch <file>, write <file> <data>, rm <file>,\r\n");
        printf("          fstest, exec <file.elf>\r\n");
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
    } else if (strcmp(cmd, "sbrk") == 0) {
        printf("Demoing SYS_SBRK from ring 3...\r\n");
        shell_run_ring3_task(user_task_sbrk, "sbrk");
    } else if (strcmp(cmd, "fork") == 0) {
        printf("Demoing SYS_FORK from ring 3...\r\n");
        printf("(fork requires a scheduler-managed process; "
               "exec_setjmp path returns -1  - expected)\r\n");
        shell_run_ring3_task(user_task_fork, "fork");
    } else if (strcmp(cmd, "ps") == 0) {
        shell_cmd_ps();
    } else if (strcmp(cmd, "ls") == 0) {
        shell_cmd_ls();
    } else if (strncmp(cmd, "cat ", 4) == 0) {
        shell_cmd_cat(cmd + 4);
    } else if (strncmp(cmd, "exec ", 5) == 0) {
        shell_cmd_exec(cmd + 5);
    } else if (strncmp(cmd, "touch ", 6) == 0) {
        shell_cmd_touch(cmd + 6);
    } else if (strncmp(cmd, "write ", 6) == 0) {
        shell_cmd_write_file(cmd + 6);
    } else if (strncmp(cmd, "rm ", 3) == 0) {
        shell_cmd_rm(cmd + 3);
    } else if (strcmp(cmd, "fstest") == 0) {
        shell_cmd_fstest();
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
