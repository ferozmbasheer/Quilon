/*
 * Quilon OS  - ring-3 user-space shell (section 7.2)
 *
 * This shell runs entirely at CPL=3, using only int $0x80 syscalls to
 * communicate with the kernel.  It is the Quilon equivalent of Unix's
 * /bin/sh or init  - the first user-space process, PID 1.
 *
 * Commands
 * --------
 *   help              - list available commands
 *   ls                - list files in the root directory (SYS_READDIR)
 *   cat <file>        - print a file to stdout (SYS_OPEN + SYS_READ)
 *   exec <file.elf>   - launch an ELF program as a child process and wait
 *   pid               - print the shell's own PID (SYS_GETPID)
 *   exit              - exit the shell (SYS_EXIT 0)
 *
 * Differences from the kernel ring-0 shell
 * -----------------------------------------
 *   • No access to kernel internals (printf -> terminal_write, VFS pointers…).
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
#include <pthread.h>
#include <gfx.h>

/* -- Terminal helpers --------------------------------------------------- */

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

/* -- Command implementations -------------------------------------------- */

static void cmd_help(void)
{
    printf("Quilon ring-3 shell commands:\r\n");
    printf("  help                   - show this message\r\n");
    printf("  ls                     - list files in root directory\r\n");
    printf("  cat <file>             - print file contents\r\n");
    printf("  touch <file>           - create an empty file\r\n");
    printf("  write <file> <data>    - write text to a file\r\n");
    printf("  rm <file|dir>          - delete a file or empty directory\r\n");
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
    printf("  pci                    - list PCI bus devices (section 10.1)\r\n");
    printf("  net                    - show RTL8139 NIC status (section 10.2)\r\n");
    printf("  netsend                - send ARP request and poll for reply\r\n");
    printf("  dhcp                   - obtain IP address via DHCP\r\n");
    printf("  ip                     - show current IPv4 address\r\n");
    printf("  ping <a.b.c.d>         - ICMP echo request\r\n");
    printf("  vga                    - show VBE framebuffer info (section 10.4)\r\n");
    printf("  smp                    - show CPU/SMP info via CPUID (section 10.5)\r\n");
    printf("  ansitest               - ANSI colour/cursor demo (section 11.1)\r\n");
    printf("  psf                    - PSF2 font loader info    (section 11.2)\r\n");
    printf("  stat <file>            - show file/dir metadata   (section 12.1)\r\n");
    printf("  pwd                    - print working directory   (section 12.1)\r\n");
    printf("  cd <path>              - change working directory  (section 12.1)\r\n");
    printf("  mkdir <dir>            - create directory          (section 12.1)\r\n");
    printf("  rename <old> <new>     - rename file or directory  (section 12.1)\r\n");
    printf("  thread                 - kernel thread demo (SYS_CLONE, section 12.3)\r\n");
    printf("  mouse                  - PS/2 mouse position and buttons (section 13)\r\n");
    printf("  gfx                    - 2D graphics lib demo (section 14.1)\r\n");
    printf("  wm-demo                - WM compositor demo   (section 14.2)\r\n");
    printf("  startx                 - launch the graphical desktop (section 14.3)\r\n");
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
    if (!path || path[0] == '\0') { printf("Usage: rm <file|dir>\r\n"); return; }
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

    /* Try to read MOTD.TXT -- present in demo initrd images. */
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
    write(STDOUT_FILENO, "\033[2J\033[H", 7);
}

/*
 * cmd_heaptest -- demand paging demo (section 9.2).
 *
 * Allocates a 256 KiB buffer via malloc (which calls sbrk internally).
 * With demand paging, sbrk only creates a VMA -- no physical pages are
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
    printf("Allocating %d KiB via malloc (sbrk -> VMA, no pages yet)...\r\n",
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

    printf("Result: %s\r\n", ok ? "PASS -- all 64 pages demand-paged and verified"
                                 : "FAIL -- data mismatch");
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

/* cmd_cow -- demonstrate copy-on-write fork (section 9.3).
 *
 * Allocates a buffer, writes "parent" into it, then forks.
 * The child overwrites it with "child" and exits.  Because CoW is in
 * effect, the parent still sees its original value -- the write in the
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
        /* Parent's copy must be untouched -- CoW preserved isolation. */
        printf("cow: parent (PID=%d) shared=\"%s\"  "
               "(expect \"parent-data\")\r\n", getpid(), shared);
        printf("cow: %s\r\n",
               (shared[0] == 'p') ? "PASS: CoW preserved parent page"
                                  : "FAIL: parent page was corrupted");
    }
}

/* Thread function used by cmd_thread. */
static void *thread_worker(void *arg)
{
    int n = (int)(unsigned)arg;
    printf("thread: worker (tid=%d, arg=%d) running\r\n", getpid(), n);
    return (void *)0;
}

static void cmd_thread(void)
{
    printf("=== Kernel Thread demo (section 12.3 -- SYS_CLONE) ===\r\n");
    printf("thread: SYS_CLONE=%d  CLONE_VM=0x%x\r\n",
           SYS_CLONE, CLONE_VM);
    printf("thread: creating two threads via pthread_create...\r\n");

    pthread_t t1, t2;
    int r1 = pthread_create(&t1, NULL, thread_worker, (void *)1);
    int r2 = pthread_create(&t2, NULL, thread_worker, (void *)2);

    if (r1 != 0 || r2 != 0) {
        printf("thread: pthread_create failed\r\n");
        return;
    }

    printf("thread: parent (pid=%d) waiting for t1=%d t2=%d\r\n",
           getpid(), (int)t1, (int)t2);

    pthread_join(t1, NULL);
    printf("thread: t1 joined\r\n");
    pthread_join(t2, NULL);
    printf("thread: t2 joined\r\n");

    printf("thread: PASS -- both threads ran and returned\r\n");
    printf("=== Thread demo done ===\r\n");
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

/* -- PCI class code name -- minimal inline table (no kernel headers needed) -- */
static const char *pci_cls(unsigned int code)
{
    switch (code) {
    case 0x00: return "Unclassified";
    case 0x01: return "Mass Storage";
    case 0x02: return "Network";
    case 0x03: return "Display";
    case 0x04: return "Multimedia";
    case 0x05: return "Memory";
    case 0x06: return "Bridge";
    case 0x07: return "Communication";
    case 0x08: return "System Peripheral";
    case 0x09: return "Input Device";
    case 0x0C: return "Serial Bus";
    case 0x0D: return "Wireless";
    case 0xFF: return "Unassigned";
    default:   return "Unknown";
    }
}

static void cmd_pci(void)
{
    int found = 0;
    printf("=== PCI Bus Enumeration (section 10.1) ===\r\n");

    for (unsigned int bus = 0; bus < 256; bus++) {
        for (unsigned int slot = 0; slot < 32; slot++) {
            unsigned int w = pci_read_u(bus, slot, 0, 0x00);
            unsigned int vendor = w & 0xFFFF;
            if (vendor == 0xFFFF) continue;   /* no device */

            unsigned int device = w >> 16;
            /* Read class/subclass from offset 0x08. */
            unsigned int cw  = pci_read_u(bus, slot, 0, 0x08);
            unsigned int cls = (cw >> 24) & 0xFF;
            unsigned int sub = (cw >> 16) & 0xFF;

            printf("  %u:%u.0  vendor=0x%x  device=0x%x"
                   "  class=0x%x/0x%x (%s)\r\n",
                   bus, slot,
                   vendor, device,
                   cls, sub,
                   pci_cls(cls));
            found++;

            /* Check for multi-function device (header type bit 7). */
            unsigned int hw = pci_read_u(bus, slot, 0, 0x0C);
            unsigned int htype = (hw >> 16) & 0xFF;
            if (htype & 0x80) {
                for (unsigned int func = 1; func < 8; func++) {
                    unsigned int fw = pci_read_u(bus, slot, func, 0x00);
                    if ((fw & 0xFFFF) == 0xFFFF) continue;
                    unsigned int fdev = fw >> 16;
                    unsigned int fcw  = pci_read_u(bus, slot, func, 0x08);
                    unsigned int fcls = (fcw >> 24) & 0xFF;
                    unsigned int fsub = (fcw >> 16) & 0xFF;
                    printf("  %u:%u.%u  vendor=0x%x  device=0x%x"
                           "  class=0x%x/0x%x (%s)\r\n",
                           bus, slot, func,
                           fw & 0xFFFF, fdev,
                           fcls, fsub,
                           pci_cls(fcls));
                    found++;
                }
            }
        }
    }

    if (found == 0)
        printf("  (no PCI devices found)\r\n");
    else
        printf("Total: %d device(s)\r\n", found);
    printf("=== done ===\r\n");
}

static void cmd_net(void)
{
    unsigned char mac[6];
    printf("=== RTL8139 Network Card (section 10.2) ===\r\n");

    int ready = net_status(mac);
    if (!ready) {
        printf("rtl8139: not ready (not found or init failed)\r\n");
        printf("  make sure QEMU is started with:"
               " -device rtl8139,netdev=net0 -netdev user,id=net0\r\n");
    } else {
        printf("rtl8139: ready\r\n");
        printf("mac: %x:%x:%x:%x:%x:%x\r\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    printf("=== done ===\r\n");
}

static void cmd_netsend(void)
{
    unsigned char mac[6];
    printf("=== RTL8139 TX/RX demo (section 10.2) ===\r\n");

    int ready = net_status(mac);
    if (!ready) {
        printf("rtl8139: NIC not ready\r\n");
        return;
    }
    printf("mac: %x:%x:%x:%x:%x:%x\r\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Build a minimal ARP "Who has 10.0.2.2?" broadcast frame. */
    unsigned char frame[42];
    int i;

    /* Ethernet header: dst=broadcast, src=our MAC, EtherType=0x0806 (ARP) */
    for (i = 0; i < 6; i++) frame[i] = 0xFF;       /* dst: broadcast */
    for (i = 0; i < 6; i++) frame[6 + i] = mac[i]; /* src: our MAC */
    frame[12] = 0x08; frame[13] = 0x06;             /* EtherType: ARP */

    /* ARP payload */
    frame[14] = 0x00; frame[15] = 0x01; /* HTYPE: Ethernet */
    frame[16] = 0x08; frame[17] = 0x00; /* PTYPE: IPv4 */
    frame[18] = 6;                       /* HLEN */
    frame[19] = 4;                       /* PLEN */
    frame[20] = 0x00; frame[21] = 0x01; /* OPER: request */
    for (i = 0; i < 6; i++) frame[22 + i] = mac[i]; /* sender MAC */
    frame[28] = 10; frame[29] = 0; frame[30] = 2; frame[31] = 15; /* sender IP 10.0.2.15 (Standard guest IP in QEMU)*/
    for (i = 0; i < 6; i++) frame[32 + i] = 0x00;   /* target MAC: unknown */
    frame[38] = 10; frame[39] = 0; frame[40] = 2; frame[41] = 2;  /* target IP 10.0.2.2 (QEMU virtual gateway/DNS)*/

    printf("sending ARP request (42 bytes)...\r\n");
    int r = net_send(frame, 42);
    if (r != 0) {
        printf("net_send: failed (%d)\r\n", r);
        return;
    }
    printf("TX: ok\r\n");

    /* Poll for a response (up to ~1M iterations). */
    printf("polling for RX...\r\n");
    unsigned char rxbuf[1520];
    int got = 0;
    int iter;
    for (iter = 0; iter < 1000000 && !got; iter++) {
        int n = net_recv(rxbuf, (int)sizeof(rxbuf));
        if (n <= 0) continue;
        got = 1;
        printf("RX: %d bytes\r\n", n);
        printf("  dst: %x:%x:%x:%x:%x:%x\r\n",
               rxbuf[0], rxbuf[1], rxbuf[2], rxbuf[3], rxbuf[4], rxbuf[5]);
        printf("  src: %x:%x:%x:%x:%x:%x\r\n",
               rxbuf[6], rxbuf[7], rxbuf[8], rxbuf[9], rxbuf[10], rxbuf[11]);
        unsigned int etype = ((unsigned int)rxbuf[12] << 8) | rxbuf[13];
        printf("  ethertype: 0x%x\r\n", etype);
    }
    if (!got)
        printf("no reply received (timeout)\r\n");
    printf("=== done ===\r\n");
}

static unsigned int shell_parse_ipv4(const char *s)
{
    unsigned int ip = 0;
    unsigned int octet = 0;
    int dots = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            octet = octet * 10u + (unsigned int)(*s - '0');
            if (octet > 255u) return 0u;
        } else if (*s == '.') {
            ip = (ip << 8) | octet;
            octet = 0u;
            dots++;
            if (dots > 3) return 0u;
        } else {
            return 0u;
        }
        s++;
    }
    if (dots != 3) return 0u;
    return (ip << 8) | octet;
}

static void cmd_dhcp(void)
{
    printf("=== DHCP (section 10.3) ===\r\n");
    printf("dhcp: sending DISCOVER...\r\n");
    int r = net_dhcp();
    if (r == 0) {
        unsigned int ip = net_getip();
        printf("dhcp: ACK -- IP = %u.%u.%u.%u\r\n",
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
               (ip >>  8) & 0xFF,  ip & 0xFF);
    } else {
        printf("dhcp: failed (timeout or no DHCP server)\r\n");
    }
    printf("=== done ===\r\n");
}

static void cmd_ip(void)
{
    unsigned int ip = net_getip();
    if (ip == 0)
        printf("IP: not configured (run 'dhcp')\r\n");
    else
        printf("IP: %u.%u.%u.%u\r\n",
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
               (ip >>  8) & 0xFF,  ip & 0xFF);
}

static void cmd_ping(const char *arg)
{
    if (!arg || arg[0] == '\0') {
        printf("Usage: ping <a.b.c.d>\r\n");
        return;
    }
    unsigned int dst = shell_parse_ipv4(arg);
    if (dst == 0u) {
        printf("ping: invalid address '%s'\r\n", arg);
        return;
    }
    printf("PING %u.%u.%u.%u ...\r\n",
           (dst >> 24) & 0xFF, (dst >> 16) & 0xFF,
           (dst >>  8) & 0xFF,  dst & 0xFF);
    int r = net_ping(dst);
    if (r > 0)
        printf("reply received\r\n");
    else if (r == 0)
        printf("timeout -- no reply\r\n");
    else
        printf("error (NIC not ready or no IP configured)\r\n");
}

static void cmd_vga(void)
{
    unsigned int info[3];
    int active = vbe_info_u(info);
    if (!active) {
        printf("vga: VBE framebuffer not active (text mode)\r\n");
        printf("  (add set gfxmode=800x600x32 to grub.cfg)\r\n");
        return;
    }
    printf("vga: framebuffer %ux%u  bpp=%u\r\n", info[0], info[1], info[2]);
}

static void cmd_ansitest(void)
{
    printf("=== ANSI Escape Code Demo (section 11.1) ===\r\n\r\n");

    printf("Standard colors:\r\n");
    printf("  \033[30mBlack\033[0m   \033[31mRed\033[0m     "
           "\033[32mGreen\033[0m   \033[33mYellow\033[0m\r\n");
    printf("  \033[34mBlue\033[0m    \033[35mMagenta\033[0m "
           "\033[36mCyan\033[0m    \033[37mWhite\033[0m\r\n\r\n");

    printf("Bright colors:\r\n");
    printf("  \033[90mDk Grey\033[0m \033[91mBr Red\033[0m  "
           "\033[92mBr Green\033[0m \033[93mBr Yellow\033[0m\r\n");
    printf("  \033[94mBr Blue\033[0m \033[95mBr Magenta\033[0m "
           "\033[96mBr Cyan\033[0m \033[97mBr White\033[0m\r\n\r\n");

    printf("Attributes:\r\n");
    printf("  \033[1mBold\033[0m  "
           "\033[1;31mBold Red\033[0m  "
           "\033[1;32mBold Green\033[0m  "
           "\033[1;33mBold Yellow\033[0m\r\n\r\n");

    printf("Background colors:\r\n");
    printf("  \033[41m Red \033[0m "
           "\033[42m Green \033[0m "
           "\033[44m Blue \033[0m "
           "\033[45m Magenta \033[0m "
           "\033[46m Cyan \033[0m\r\n\r\n");

    printf("Erase-to-EOL (text after '|' erased):\r\n");
    printf("  visible text | ERASED_TEXT");
    printf("\033[12D\033[K");
    printf("\r\n\r\n");

    printf("Cursor save/restore:\r\n");
    printf("  before ");
    printf("\033[s");
    printf("OVERWRITTEN");
    printf("\033[u");
    printf("after\r\n\r\n");

    printf("=== done ===\r\n");
}

static void cmd_psf(void)
{
    /* The PSF2 font loader lives entirely in the kernel (ring-0).
     * From ring-3 we can only query VBE framebuffer state via vbe_info_u().
     * If VBE is active the kernel-side psf2_load() will have been called
     * from the 'psf' shell command or at boot when a font is found on initrd. */
    unsigned int info[3];
    int active = vbe_info_u(info);

    printf("=== PSF2 Bitmap Font Loader (section 11.2) ===\r\n");
    if (!active) {
        printf("VBE framebuffer: not active (text mode)\r\n");
        printf("PSF2 font rendering requires VBE graphical mode.\r\n");
        printf("(add 'set gfxmode=800x600x32' to grub.cfg)\r\n");
    } else {
        printf("VBE framebuffer: %ux%u bpp=%u\r\n",
               info[0], info[1], info[2]);
        printf("PSF2 font loader: kernel-side (use 'psf' in the ring-0 shell\r\n");
        printf("  to load a synthetic font from the built-in 8x8 bitmaps).\r\n");
        printf("\r\nFormat overview:\r\n");
        printf("  32-byte header: magic=0x864AB572, version=0,\r\n");
        printf("                  glyph_count, bytes_per_glyph, height, width\r\n");
        printf("  Glyph data: glyph[c] at offset c * bytes_per_glyph\r\n");
        printf("  Each row: ceil(width/8) bytes, MSB = leftmost pixel\r\n");
    }
    printf("=== done ===\r\n");
}

static void cmd_smp(void)
{
    /* Read CPUID leaf 1 for APIC ID and feature flags (ring-3 safe).
     * EBX[31:24] = Initial APIC ID of the logical processor running this code.
     * EDX bit 9  = APIC on-chip present.                                   */
    unsigned int eax, ebx, ecx, edx;
    asm volatile(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1)
    );
    unsigned int apic_id    = (ebx >> 24) & 0xFF;
    unsigned int has_apic   = (edx >> 9)  & 1;
    unsigned int has_htt    = (edx >> 28) & 1;   /* Hyper-Threading */
    unsigned int logical_cnt = has_htt ? ((ebx >> 16) & 0xFF) : 1;

    printf("=== SMP info (section 10.5) ===\r\n");
    printf("CPUID.1: EAX=0x%x\r\n", eax);
    printf("Initial APIC ID (this CPU): %u\r\n", apic_id);
    printf("On-chip APIC present:       %s\r\n", has_apic ? "yes" : "no");
    printf("Max logical CPUs (HTT):     %u\r\n", logical_cnt);

    /* CPUID leaf 4 / leaf 0xB report topology on newer CPUs.
     * For QEMU -smp 2, leaf 1 EBX[23:16] typically reports 2.            */
    printf("(kernel SMP state visible via 'smp' in the ring-0 shell)\r\n");
}

/* -- Section 12.1: File Metadata & Directory Operations -------------------- */

static void cmd_stat(const char *path)
{
    if (!path || path[0] == '\0') { printf("Usage: stat <file>\r\n"); return; }
    stat_t st;
    if (stat(path, &st) != 0) {
        printf("stat: '%s': not found\r\n", path);
        return;
    }
    printf("  name:  %s\r\n", path);
    printf("  type:  %s\r\n", S_ISDIR(st.st_type) ? "directory" : "regular file");
    printf("  size:  %u bytes\r\n", st.st_size);
}

static void cmd_pwd(void)
{
    char buf[128];
    if (getcwd(buf, sizeof(buf)))
        printf("%s\r\n", buf);
    else
        printf("pwd: failed\r\n");
}

static void cmd_cd(const char *path)
{
    if (!path || path[0] == '\0') { printf("Usage: cd <path>\r\n"); return; }
    if (chdir(path) != 0)
        printf("cd: '%s': not found or not a directory\r\n", path);
}

static void cmd_mkdir_dir(const char *path)
{
    if (!path || path[0] == '\0') { printf("Usage: mkdir <dir>\r\n"); return; }
    if (mkdir(path) == 0)
        printf("mkdir: created '%s'\r\n", path);
    else
        printf("mkdir: '%s': failed (exists or disk full)\r\n", path);
}

static void cmd_rename_file(const char *args)
{
    if (!args || args[0] == '\0') { printf("Usage: rename <old> <new>\r\n"); return; }
    const char *p = args;
    while (*p && *p != ' ') p++;
    if (*p == '\0' || *(p + 1) == '\0') { printf("Usage: rename <old> <new>\r\n"); return; }

    char oldname[64];
    int len = (int)(p - args);
    if (len >= (int)sizeof(oldname)) len = (int)sizeof(oldname) - 1;
    int i;
    for (i = 0; i < len; i++) oldname[i] = args[i];
    oldname[i] = '\0';

    const char *newname = p + 1;
    while (*newname == ' ') newname++;

    if (rename(oldname, newname) == 0)
        printf("rename: '%s' -> '%s'\r\n", oldname, newname);
    else
        printf("rename: failed (src not found or dst exists)\r\n");
}

static void cmd_mouse(void)
{
    mouse_event_t ev;
    printf("=== PS/2 Mouse Driver (section 13) ===\r\n");
    if (mouse_read(&ev) <= 0) {
        printf("mouse: not available\r\n");
        return;
    }
    printf("mouse: x=%d  y=%d  buttons=0x%02x\r\n",
           ev.x, ev.y, (unsigned)ev.buttons);
    printf("mouse: left=%s  right=%s  middle=%s\r\n",
           (ev.buttons & MOUSE_BTN_LEFT)   ? "down" : "up",
           (ev.buttons & MOUSE_BTN_RIGHT)  ? "down" : "up",
           (ev.buttons & MOUSE_BTN_MIDDLE) ? "down" : "up");
    printf("=== done ===\r\n");
}

static void cmd_gfx(void)
{
    printf("=== 2D Graphics Library Demo (section 14.1) ===\r\n");
    printf("syscalls: GFX_INFO=%d  GFX_MAP=%d  GFX_FLUSH=%d\r\n",
           SYS_GFX_INFO, SYS_GFX_MAP, SYS_GFX_FLUSH);

    gfx_info_t info;
    canvas_t  *scr = gfx_screen_init(&info);
    if (!scr) {
        printf("gfx: not available (VBE not active or map failed)\r\n");
        return;
    }

    int W = (int)info.width;
    int H = (int)info.height;

    printf("screen: %ux%u  bpp=%u  pitch=%u bytes\r\n",
           info.width, info.height, info.bpp, info.pitch);

    /* Dark desktop background */
    gfx_fill(scr, GFX_RGB(0x1E, 0x1E, 0x3C));

    /* Title bar */
    gfx_fill_rect(scr, (rect_t){0, 0, W, 24}, GFX_RGB(0x50, 0x50, 0xC8));
    gfx_draw_text(scr, 4, 4, "Quilon 2D Graphics -- section 14.1",
                  GFX_RGB(0xFF, 0xFF, 0xFF), GFX_RGB(0x50, 0x50, 0xC8));

    /* Close button (top-right corner) */
    gfx_fill_rect(scr, (rect_t){W - 22, 2, 20, 20}, GFX_RGB(0xC8, 0x28, 0x28));
    gfx_draw_text(scr, W - 18, 4, "X",
                  GFX_RGB(0xFF, 0xFF, 0xFF), GFX_RGB(0xC8, 0x28, 0x28));

    /* Content area */
    gfx_fill_rect(scr, (rect_t){4, 28, W - 8, H - 52}, GFX_RGB(0xF0, 0xF0, 0xF0));
    gfx_draw_rect(scr, (rect_t){4, 28, W - 8, H - 52}, GFX_RGB(0x80, 0x80, 0x80));

    /* Section label inside content area */
    gfx_draw_text(scr, 12, 36, "libgfx canvas primitives:",
                  GFX_RGB(0x20, 0x20, 0x60), GFX_RGB(0xF0, 0xF0, 0xF0));

    /* 6 colour swatches with labels */
    static const struct { int r, g, b; const char *name; } sw[6] = {
        {0xC8, 0x28, 0x28, "Red"    },
        {0x28, 0xC8, 0x28, "Green"  },
        {0x28, 0x28, 0xC8, "Blue"   },
        {0xC8, 0xC8, 0x28, "Yellow" },
        {0xC8, 0x28, 0xC8, "Magenta"},
        {0x28, 0xC8, 0xC8, "Cyan"   },
    };
    int i;
    for (i = 0; i < 6; i++) {
        int     sx  = 12 + i * 80;
        color_t col = GFX_RGB(sw[i].r, sw[i].g, sw[i].b);
        gfx_fill_rect(scr, (rect_t){sx, 56, 64, 32}, col);
        gfx_draw_rect(scr, (rect_t){sx, 56, 64, 32}, GFX_RGB(0x40, 0x40, 0x40));
        gfx_draw_text(scr, sx + 2, 92, sw[i].name,
                      GFX_RGB(0x20, 0x20, 0x20), GFX_RGB(0xF0, 0xF0, 0xF0));
    }

    /* Grey status bar at the bottom */
    gfx_fill_rect(scr, (rect_t){0, H - 20, W, 20}, GFX_RGB(0xA0, 0xA0, 0xA0));
    gfx_draw_text(scr, 4, H - 16, "ring-3 user-space | libgfx | SYS_GFX_FLUSH",
                  GFX_RGB(0x10, 0x10, 0x10), GFX_RGB(0xA0, 0xA0, 0xA0));

    gfx_flush();
    printf("gfx: scene rendered and flushed to framebuffer\r\n");
    printf("=== done ===\r\n");
}

/* -- Section 14.2: Window Manager & Compositor demo -------------------- */

static void cmd_wm_demo(void)
{
    printf("=== Window Manager & Compositor Demo (section 14.2) ===\r\n");

    gfx_info_t info;
    canvas_t  *scr = gfx_screen_init(&info);
    if (!scr) {
        printf("wm-demo: VBE not active or framebuffer map failed\r\n");
        printf("\r\nWM design summary:\r\n");
        printf("  wm_state_t  -- window list (up to 16 windows)\r\n");
        printf("  wm_composite() -- painter's algorithm (back-to-front)\r\n");
        printf("  wm_hittest()   -- front-to-back to find click target\r\n");
        printf("  wm_raise()     -- rotate window to top of z-order\r\n");
        printf("  wm_handle_mouse_down/move/up() -- drag & focus\r\n");
        printf("  IPC: apps write wm_msg_t to /wm_cmd pipe\r\n");
        printf("=== done ===\r\n");
        return;
    }

    int W = (int)info.width;
    int H = (int)info.height;
    printf("screen: %ux%u\r\n", (unsigned)info.width, (unsigned)info.height);

    /* -- Desktop background -- */
    gfx_fill(scr, GFX_RGB(30, 30, 60));

    /* -- Simulate 3 windows using libgfx primitives -- */

    /* Window layout */
    struct {
        int x, y, w, h, focused;
        const char *title;
        color_t content_col;
        const char *content_text;
    } wins[] = {
        { 60,  80,  240, 120, 0, "About Quilon",
          GFX_RGB(20,20,40),   "Quilon OS WM 14.2"  },
        { 320, 60,  280, 180, 0, "Terminal",
          GFX_BLACK,            "quilon> _"          },
        { 160, 310, 160,  40, 1, "Clock",
          GFX_RGB(10,10,10),   "00:00:00"           },
    };
    int TITLEBAR_H_demo = 20;
    int i;

    for (i = 0; i < 3; i++) {
        int  x  = wins[i].x, y = wins[i].y;
        int  w  = wins[i].w, h = wins[i].h;
        int  tb_y = y - TITLEBAR_H_demo;

        color_t tbar = wins[i].focused ? GFX_RGB(80,80,200) : GFX_RGB(60,60,60);

        /* Title bar */
        gfx_fill_rect(scr, (rect_t){x, tb_y, w, TITLEBAR_H_demo}, tbar);
        gfx_draw_text(scr, x + 4, tb_y + 2, wins[i].title,
                      GFX_WHITE, tbar);

        /* Close button (right side of title bar) */
        int cbx = x + w - 18;
        int cby = tb_y + 2;
        gfx_fill_rect(scr, (rect_t){cbx, cby, 16, 16}, GFX_RED);
        gfx_draw_text(scr, cbx + 4, cby + 2, "x", GFX_WHITE, GFX_RED);

        /* Content area */
        gfx_fill_rect(scr, (rect_t){x, y, w, h}, wins[i].content_col);
        color_t fg = wins[i].focused ? GFX_RGB(0, 255, 128) : GFX_WHITE;
        gfx_draw_text(scr, x + 8, y + 12, wins[i].content_text,
                      fg, wins[i].content_col);

        /* Border (wraps title bar + content) */
        gfx_draw_rect(scr, (rect_t){x, tb_y, w, TITLEBAR_H_demo + h},
                      GFX_RGB(100, 100, 100));
    }

    /* -- Mouse cursor at screen centre (8×8 arrow sprite) -- */
    int mx = W / 2, my = H / 2;
    static const uint8_t cshape[8] = {0xFE,0xFC,0xF8,0xF0,0xE0,0xC0,0x80,0x00};
    int r, c;
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++)
            if ((cshape[r] & (0x80u >> c)) && mx+c < W && my+r < H)
                scr->pixels[(my+r) * scr->pitch + (mx+c)] = GFX_WHITE;

    /* -- Status bar -- */
    gfx_fill_rect(scr, (rect_t){0, H - 18, W, 18}, GFX_RGB(50, 50, 50));
    gfx_draw_text(scr, 4, H - 14,
                  "wm-demo | painter compositor | 3 windows | section 14.2",
                  GFX_RGB(200, 200, 200), GFX_RGB(50, 50, 50));

    gfx_flush();
    printf("wm-demo: frame composited (%d windows, %ux%u)\r\n", 3,
           (unsigned)info.width, (unsigned)info.height);
    printf("         run 'exec wm.elf' to launch the live WM process\r\n");
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

/* -- Command dispatch --------------------------------------------------- */

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
    else if (strcmp(cmd, "pci")    == 0) cmd_pci();
    else if (strcmp(cmd, "net")     == 0) cmd_net();
    else if (strcmp(cmd, "netsend") == 0) cmd_netsend();
    else if (strcmp(cmd, "dhcp")   == 0) cmd_dhcp();
    else if (strcmp(cmd, "ip")     == 0) cmd_ip();
    else if (strcmp(cmd, "ping")   == 0) cmd_ping(arg);
    else if (strcmp(cmd, "vga")      == 0) cmd_vga();
    else if (strcmp(cmd, "smp")      == 0) cmd_smp();
    else if (strcmp(cmd, "ansitest") == 0) cmd_ansitest();
    else if (strcmp(cmd, "psf")      == 0) cmd_psf();
    else if (strcmp(cmd, "stat")   == 0) cmd_stat(arg);
    else if (strcmp(cmd, "pwd")    == 0) cmd_pwd();
    else if (strcmp(cmd, "cd")     == 0) cmd_cd(arg);
    else if (strcmp(cmd, "mkdir")  == 0) cmd_mkdir_dir(arg);
    else if (strcmp(cmd, "rename") == 0) cmd_rename_file(arg);
    else if (strcmp(cmd, "thread") == 0) cmd_thread();
    else if (strcmp(cmd, "mouse")  == 0) cmd_mouse();
    else if (strcmp(cmd, "gfx")     == 0) cmd_gfx();
    else if (strcmp(cmd, "wm-demo") == 0) cmd_wm_demo();
    else if (strcmp(cmd, "startx") == 0) cmd_exec("/wm.elf");
    else if (strcmp(cmd, "exit")   == 0) {
        printf("Bye.\r\n");
        exit(0);
    }
    else {
        printf("Unknown command: %s  (try 'help')\r\n", cmd);
    }
}

/* -- Entry point -------------------------------------------------------- */

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
