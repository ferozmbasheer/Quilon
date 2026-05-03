/*
 * Quilon OS  - ring-3 user-space shell (section 7.2)
 *
 * This shell runs entirely at CPL=3, using only int $0x80 syscalls to
 * communicate with the kernel.  It is the Quilon equivalent of Unix's
 * /bin/sh or init  - the first user-space process, PID 1.
 *
 * Commands
 * ────────
 *   help              - list available commands
 *   ls                - list files in the root directory (SYS_READDIR)
 *   cat <file>        - print a file to stdout (SYS_OPEN + SYS_READ)
 *   exec <file.elf>   - launch an ELF program as a child process and wait
 *   pid               - print the shell's own PID (SYS_GETPID)
 *   exit              - exit the shell (SYS_EXIT 0)
 *
 * Differences from the kernel ring-0 shell
 * ─────────────────────────────────────────
 *   • No access to kernel internals (printf → terminal_write, VFS pointers…).
 *     Everything goes through syscalls.
 *   • exec spawns a proper child process (SYS_EXEC returns child PID); the
 *     shell waits for it with SYS_WAIT instead of using exec_setjmp.
 *   • A bug in this shell causes a SIGSEGV  - not a kernel panic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

/* ── Terminal helpers ─────────────────────────────────────────────────── */

/*
 * readline  - read one line of text from stdin, echoing characters back.
 *
 * Reads characters one-at-a-time via SYS_READ(FD_STDIN) until Enter
 * (\r or \n) is received or the buffer is full.  Supports backspace.
 *
 * Returns the number of characters in buf (not counting the NUL terminator).
 */
static int readline(char *buf, int size)
{
    int  i = 0;
    char c;

    while (i < size - 1) {
        if (read(STDIN_FILENO, &c, 1) <= 0)
            continue;

        if (c == '\r' || c == '\n') {
            write(STDOUT_FILENO, "\r\n", 2);
            break;
        }
        if ((c == '\b' || c == 127) && i > 0) {  /* backspace */
            i--;
            write(STDOUT_FILENO, "\b \b", 3);
            continue;
        }
        if (c < 32) continue;  /* ignore other control characters */

        buf[i++] = c;
        write(STDOUT_FILENO, &c, 1);  /* echo */
    }
    buf[i] = '\0';
    return i;
}

/* ── Command implementations ──────────────────────────────────────────── */

static void cmd_help(void)
{
    printf("Quilon ring-3 shell commands:\r\n");
    printf("  help                   - show this message\r\n");
    printf("  ls                     - list files in root directory\r\n");
    printf("  cat <file>             - print file contents\r\n");
    printf("  touch <file>           - create an empty file\r\n");
    printf("  write <file> <data>    - write text to a file\r\n");
    printf("  rm <file>              - delete a file\r\n");
    printf("  fstest                 - FAT16 write/read/remove demo\r\n");
    printf("  initrd                 - list initrd/RAM filesystem contents\r\n");
    printf("  exec <file.elf>        - run an ELF program\r\n");
    printf("  pid                    - print shell PID\r\n");
    printf("  ticks                  - raw PIT tick count\r\n");
    printf("  seconds                - uptime in whole seconds\r\n");
    printf("  clear / cls            - scroll screen\r\n");
    printf("  sbrk                   - demo heap growth via SYS_SBRK\r\n");
    printf("  heaptest               - demand-paging demo: 256 KiB lazy heap\r\n");
    printf("  fork                   - demo SYS_FORK with parent/child\r\n");
    printf("  cow                    - CoW fork: parent/child see own data\r\n");
    printf("  pipe                   - demo anonymous pipe (SYS_PIPE)\r\n");
    printf("  exit                   - exit the shell\r\n");
}

static void cmd_ls(void)
{
    dirent_t     ent;
    unsigned int i = 0;
    int          found = 0;

    while (readdir(i, &ent) == 0) {
        const char *kind = (ent.type == DIRENT_TYPE_DIR) ? "[dir] " : "      ";
        printf("  %s%-14s  %u bytes\r\n", kind, ent.name, ent.size);
        i++;
        found = 1;
    }
    if (!found)
        printf("  (empty)\r\n");
}

static void cmd_cat(const char *path)
{
    if (!path || path[0] == '\0') {
        printf("Usage: cat <file>\r\n");
        return;
    }

    int fd = open(path);
    if (fd < 0) {
        printf("cat: %s: not found\r\n", path);
        return;
    }

    char buf[512];
    int  n;
    while ((n = read(fd, buf, (int)sizeof(buf))) > 0)
        write(STDOUT_FILENO, buf, n);

    close(fd);
    write(STDOUT_FILENO, "\r\n", 2);
}

static void cmd_exec(const char *path)
{
    if (!path || path[0] == '\0') {
        printf("Usage: exec <file.elf>\r\n");
        return;
    }

    printf("exec: launching '%s'...\r\n", path);
    int child_pid = exec(path);
    if (child_pid < 0) {
        printf("exec: failed to launch '%s'\r\n", path);
        return;
    }
    printf("exec: child PID=%d, waiting...\r\n", child_pid);

    int exit_code = 0;
    wait(child_pid, &exit_code);
    printf("exec: '%s' exited (code %d)\r\n", path, exit_code);
}

static void cmd_touch(const char *path)
{
    if (!path || path[0] == '\0') { printf("Usage: touch <file>\r\n"); return; }
    if (create(path) == 0)
        printf("Created '%s'\r\n", path);
    else
        printf("touch: '%s': already exists or disk full\r\n", path);
}

static void cmd_write(const char *args)
{
    /* Split "filename content..." at first space. */
    if (!args || args[0] == '\0') {
        printf("Usage: write <file> <data>\r\n");
        return;
    }
    const char *p = args;
    while (*p && *p != ' ') p++;
    if (*p == '\0' || *(p + 1) == '\0') {
        printf("Usage: write <file> <data>\r\n");
        return;
    }

    /* Copy filename */
    char fname[64];
    int flen = (int)(p - args);
    if (flen >= (int)sizeof(fname)) flen = (int)sizeof(fname) - 1;
    int i;
    for (i = 0; i < flen; i++) fname[i] = args[i];
    fname[i] = '\0';

    const char *content = p + 1;
    int clen = strlen(content);

    int fd = open(fname);
    if (fd < 0) { printf("write: '%s': not found\r\n", fname); return; }

    int n = write(fd, content, clen);
    close(fd);

    if (n >= 0)
        printf("Wrote %d bytes to '%s'\r\n", n, fname);
    else
        printf("write: '%s': write failed\r\n", fname);
}

static void cmd_rm(const char *path)
{
    if (!path || path[0] == '\0') { printf("Usage: rm <file>\r\n"); return; }
    if (fremove(path) == 0)
        printf("Removed '%s'\r\n", path);
    else
        printf("rm: '%s': not found\r\n", path);
}

static void cmd_fstest(void)
{
    const char *fname   = "TEST.TXT";
    const char *message = "Hello from FAT16 write!";

    printf("=== FAT16 write demo ===\r\n");

    printf("1. touch %s ... ", fname);
    if (create(fname) != 0) { printf("FAILED\r\n"); return; }
    printf("OK\r\n");

    int fd = open(fname);
    if (fd < 0) { printf("2. open failed\r\n"); goto cleanup; }
    {
        int n = write(fd, message, strlen(message));
        close(fd);
        printf("2. wrote %d bytes\r\n", n);
    }

    fd = open(fname);
    if (fd < 0) { printf("3. re-open failed\r\n"); goto cleanup; }
    {
        char buf[64];
        int n = read(fd, buf, (int)sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            printf("3. read back: \"%s\"\r\n", buf);
            printf("   %s\r\n", strcmp(buf, message) == 0
                                 ? "content matches" : "MISMATCH");
        } else {
            printf("3. read failed\r\n");
        }
    }

    printf("4. directory:\r\n");
    {
        dirent_t     ent;
        unsigned int i = 0;
        while (readdir(i, &ent) == 0) {
            printf("   %-14s %u bytes\r\n", ent.name, ent.size);
            i++;
        }
    }

cleanup:
    printf("5. rm %s ... ", fname);
    printf("%s\r\n", fremove(fname) == 0 ? "OK" : "FAILED");
    printf("=== done ===\r\n");
}

static void cmd_initrd(void)
{
    /* List all files in the currently mounted filesystem and, if MOTD.TXT
     * exists, print its contents.  When the kernel mounted an initrd before
     * FAT16, this shows the RAM filesystem's files.                        */
    printf("=== initrd demo (section 8.3) ===\r\n");
    printf("(reads from whatever filesystem is currently mounted)\r\n");

    dirent_t     ent;
    unsigned int i     = 0;
    int          count = 0;
    while (readdir(i, &ent) == 0) {
        printf("  [%u] %-16s  %u bytes\r\n", i, ent.name, ent.size);
        i++;
        count++;
    }
    if (count == 0)
        printf("  (empty)\r\n");

    /* Try to read MOTD.TXT — present in demo initrd images. */
    int fd = open("MOTD.TXT");
    if (fd >= 0) {
        char buf[64];
        int n = read(fd, buf, (int)sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            printf("MOTD.TXT: \"%s\"\r\n", buf);
        }
    }
    printf("=== done ===\r\n");
}

static void cmd_clear(void)
{
    int i;
    for (i = 0; i < 25; i++)
        write(STDOUT_FILENO, "\r\n", 2);
}

/*
 * cmd_heaptest — demand paging demo (section 9.2).
 *
 * Allocates a 256 KiB buffer via malloc (which calls sbrk internally).
 * With demand paging, sbrk only creates a VMA — no physical pages are
 * allocated yet.  Each write to a new page triggers a page-fault, and the
 * kernel prints "[demand] pid X: mapped page 0xXXXXX".  This makes the
 * lazy page-by-page allocation visible on the console.
 *
 * After writing, every page is verified to confirm the kernel delivered the
 * correct data at each demand-paged address.
 */
static void cmd_heaptest(void)
{
    const int BUF_SIZE  = 256 * 1024;   /* 256 KiB = 64 pages */
    const int PAGE_SIZE = 4096;

    printf("=== Demand-paging heap test (section 9.2) ===\r\n");
    printf("Allocating %d KiB via malloc (sbrk → VMA, no pages yet)...\r\n",
           BUF_SIZE / 1024);

    char *buf = (char *)malloc(BUF_SIZE);
    if (!buf) {
        printf("heaptest: malloc failed\r\n");
        return;
    }
    printf("malloc returned %p\r\n", (void *)buf);
    printf("Touching each 4-KiB page (each touch triggers a demand-page fault):\r\n");

    int i;
    for (i = 0; i < BUF_SIZE; i += PAGE_SIZE) {
        buf[i] = (char)((i / PAGE_SIZE) & 0xFF);
    }

    printf("Verifying page contents...\r\n");
    int ok = 1;
    for (i = 0; i < BUF_SIZE; i += PAGE_SIZE) {
        if (buf[i] != (char)((i / PAGE_SIZE) & 0xFF)) {
            printf("  MISMATCH at offset %d\r\n", i);
            ok = 0;
            break;
        }
    }

    printf("Result: %s\r\n", ok ? "PASS — all 64 pages demand-paged and verified"
                                 : "FAIL — data mismatch");
    free(buf);
    printf("=== done ===\r\n");
}

static void cmd_sbrk(void)
{
    char *p = (char *)sbrk(4096);
    if ((int)(long)p == -1) {
        printf("sbrk: failed\r\n");
        return;
    }
    *p = 0x42;
    printf("sbrk: got %p, sentinel=0x%x\r\n", (void *)p, (unsigned char)*p);
}

static void cmd_fork(void)
{
    int pid = fork();
    if (pid < 0) {
        printf("fork: failed\r\n");
    } else if (pid == 0) {
        printf("fork: child (PID=%d)\r\n", getpid());
        exit(0);
    } else {
        int code = 0;
        printf("fork: parent (PID=%d), child=%d\r\n", getpid(), pid);
        wait(pid, &code);
        printf("fork: child exited (code=%d)\r\n", code);
    }
}

/* cmd_cow — demonstrate copy-on-write fork (section 9.3).
 *
 * Allocates a buffer, writes "parent" into it, then forks.
 * The child overwrites it with "child" and exits.  Because CoW is in
 * effect, the parent still sees its original value — the write in the
 * child triggered a CoW fault, a new physical page was allocated for the
 * child, and the parent's page was left untouched.               */
static void cmd_cow(void)
{
    static char shared[32];

    /* Write initial data before fork. */
    int i;
    const char *init = "parent-data";
    for (i = 0; init[i]; i++) shared[i] = init[i];
    shared[i] = '\0';

    printf("cow: before fork, shared=\"%s\"\r\n", shared);

    int pid = fork();
    if (pid < 0) {
        printf("cow: fork failed\r\n");
        return;
    }

    if (pid == 0) {
        /* Child: overwrite the buffer.  This write triggers a CoW fault;
         * the kernel copies the page and maps a private copy here. */
        const char *cdata = "child-data";
        for (i = 0; cdata[i]; i++) shared[i] = cdata[i];
        shared[i] = '\0';
        printf("cow: child  (PID=%d) shared=\"%s\"\r\n", getpid(), shared);
        exit(0);
    } else {
        int code = 0;
        wait(pid, &code);
        /* Parent's copy must be untouched — CoW preserved isolation. */
        printf("cow: parent (PID=%d) shared=\"%s\"  "
               "(expect \"parent-data\")\r\n", getpid(), shared);
        printf("cow: %s\r\n",
               (shared[0] == 'p') ? "PASS: CoW preserved parent page"
                                  : "FAIL: parent page was corrupted");
    }
}

static void cmd_pipe(void)
{
    const char *msg = "Hello through the pipe!";
    int         msglen = strlen(msg);

    printf("=== Pipe demo (section 8.2) ===\r\n");

    int fds[2];
    if (pipe(fds) != 0) {
        printf("pipe: syscall failed\r\n");
        return;
    }
    printf("1. pipe created: read_fd=%d  write_fd=%d\r\n", fds[0], fds[1]);

    /* Write into the write end. */
    int n = write(fds[1], msg, msglen);
    printf("2. wrote %d bytes to write_fd\r\n", n);

    /* Close the write end so the read end sees EOF after draining. */
    close(fds[1]);
    printf("3. write end closed\r\n");

    /* Read from the read end. */
    char buf[64];
    int r = read(fds[0], buf, (int)sizeof(buf) - 1);
    if (r > 0) {
        buf[r] = '\0';
        printf("4. read %d bytes: \"%s\"\r\n", r, buf);
        printf("   %s\r\n",
               strcmp(buf, msg) == 0 ? "content matches" : "MISMATCH");
    } else {
        printf("4. read returned %d\r\n", r);
    }

    /* Second read should return 0 (EOF). */
    int eof = read(fds[0], buf, (int)sizeof(buf));
    printf("5. second read (EOF expected): %d\r\n", eof);

    close(fds[0]);
    printf("=== done ===\r\n");
}

static void cmd_ticks(void)
{
    printf("%u\r\n", getticks());
}

static void cmd_seconds(void)
{
    unsigned int hz = gethz();
    unsigned int secs = hz ? getticks() / hz : 0;
    printf("%u\r\n", secs);
}

static void cmd_pid(void)
{
    printf("shell PID: %d\r\n", getpid());
}

/* ── Command dispatch ─────────────────────────────────────────────────── */

static void dispatch(char *line)
{
    /* Trim leading spaces. */
    while (*line == ' ') line++;
    if (*line == '\0') return;

    /* Split command from argument at the first space. */
    char *cmd = line;
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg == ' ') {
        *arg = '\0';  /* NUL-terminate the command */
        arg++;
        while (*arg == ' ') arg++;  /* skip spaces before argument */
    }

    if      (strcmp(cmd, "help")   == 0) cmd_help();
    else if (strcmp(cmd, "ls")     == 0) cmd_ls();
    else if (strcmp(cmd, "cat")    == 0) cmd_cat(arg);
    else if (strcmp(cmd, "touch")  == 0) cmd_touch(arg);
    else if (strcmp(cmd, "write")  == 0) cmd_write(arg);
    else if (strcmp(cmd, "rm")     == 0) cmd_rm(arg);
    else if (strcmp(cmd, "fstest") == 0) cmd_fstest();
    else if (strcmp(cmd, "exec")   == 0) cmd_exec(arg);
    else if (strcmp(cmd, "ticks")   == 0) cmd_ticks();
    else if (strcmp(cmd, "seconds") == 0) cmd_seconds();
    else if (strcmp(cmd, "pid")    == 0) cmd_pid();
    else if (strcmp(cmd, "clear")  == 0) cmd_clear();
    else if (strcmp(cmd, "cls")    == 0) cmd_clear();
    else if (strcmp(cmd, "sbrk")     == 0) cmd_sbrk();
    else if (strcmp(cmd, "heaptest") == 0) cmd_heaptest();
    else if (strcmp(cmd, "fork")   == 0) cmd_fork();
    else if (strcmp(cmd, "cow")    == 0) cmd_cow();
    else if (strcmp(cmd, "pipe")   == 0) cmd_pipe();
    else if (strcmp(cmd, "initrd") == 0) cmd_initrd();
    else if (strcmp(cmd, "exit")   == 0) {
        printf("Bye.\r\n");
        exit(0);
    }
    else {
        printf("Unknown command: %s  (try 'help')\r\n", cmd);
    }
}

/* ── Entry point ──────────────────────────────────────────────────────── */

int main(void)
{
    printf("\r\n");
    printf("+----------------------------------------------+\r\n");
    printf("|  Quilon ring-3 shell  (section 7.2)          |\r\n");
    printf("|  Running at CPL=3 via SYS_EXEC / process_t   |\r\n");
    printf("+----------------------------------------------+\r\n");
    printf("Type 'help' for a list of commands.\r\n\r\n");

    char line[128];

    for (;;) {
        write(STDOUT_FILENO, "quilon> ", 8);
        readline(line, (int)sizeof(line));
        dispatch(line);
    }

    return 0;  /* unreachable */
}
