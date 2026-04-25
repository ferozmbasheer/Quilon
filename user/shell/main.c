/*
 * Quilon OS — ring-3 user-space shell (section 7.2)
 *
 * This shell runs entirely at CPL=3, using only int $0x80 syscalls to
 * communicate with the kernel.  It is the Quilon equivalent of Unix's
 * /bin/sh or init — the first user-space process, PID 1.
 *
 * Commands
 * ────────
 *   help             — list available commands
 *   ls               — list files in the root directory (SYS_READDIR)
 *   cat <file>       — print a file to stdout (SYS_OPEN + SYS_READ)
 *   exec <file.elf>  — launch an ELF program as a child process and wait
 *   pid              — print the shell's own PID (SYS_GETPID)
 *   exit             — exit the shell (SYS_EXIT 0)
 *
 * Differences from the kernel ring-0 shell
 * ─────────────────────────────────────────
 *   • No access to kernel internals (printf → terminal_write, VFS pointers…).
 *     Everything goes through syscalls.
 *   • exec spawns a proper child process (SYS_EXEC returns child PID); the
 *     shell waits for it with SYS_WAIT instead of using exec_setjmp.
 *   • A bug in this shell causes a SIGSEGV — not a kernel panic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

/* ── Terminal helpers ─────────────────────────────────────────────────── */

/*
 * readline — read one line of text from stdin, echoing characters back.
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
    printf("  help             — show this message\r\n");
    printf("  ls               — list files in root directory\r\n");
    printf("  cat <file>       — print file contents\r\n");
    printf("  exec <file.elf>  — run an ELF program\r\n");
    printf("  pid              — print shell PID\r\n");
    printf("  exit             — exit the shell\r\n");
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

    if      (strcmp(cmd, "help") == 0) cmd_help();
    else if (strcmp(cmd, "ls")   == 0) cmd_ls();
    else if (strcmp(cmd, "cat")  == 0) cmd_cat(arg);
    else if (strcmp(cmd, "exec") == 0) cmd_exec(arg);
    else if (strcmp(cmd, "pid")  == 0) cmd_pid();
    else if (strcmp(cmd, "exit") == 0) {
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
