/*
 * Quilon OS -- ktest: in-OS kernel regression tests (REVIEW.md)
 *
 * The host unit tests (tests/) cover the kernel's pure-C policy code; this
 * program covers the paths that only exist on real (emulated) hardware --
 * page faults taken in ring 0, CR0.WP, process teardown.  Each check prints
 * "ktest: <name> ... ok|FAIL", and the run ends with exactly one of
 *
 *     KTEST: ALL PASS
 *     KTEST: n FAILED
 *
 * which tools/smoke_test.sh greps for (SMOKE_KEYS / SMOKE_SENTINEL), so the
 * whole suite runs headless:  make ktest
 *
 * Launch by hand from the ring-3 shell with:  exec ktest.elf
 *
 * Every check must be a regression test for a real bug: written so that it
 * FAILS (or panics the kernel) on the unfixed code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include <gfx.h>
#include <signal.h>

static int failures;
static int me_pid;

static void report(const char *name, int ok)
{
    printf("ktest: %s ... %s\r\n", name, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

/* The largest file on the disk is the WM; any big file will do. */
#define BIG_FILE   "wm.elf"
#define BIG_READ   (96 * 1024)       /* spans 24 untouched heap pages */
#define CHUNK      512

/*
 * REVIEW.md A -- a single read() into a large, freshly-malloc'd (so not yet
 * demand-paged) buffer.  The kernel's copy into the buffer page-faults in
 * ring 0; before the fix that fault was a kernel panic.  The data is checked
 * against the same bytes read in small chunks through a stack buffer.
 */
static void test_read_into_untouched_heap(void)
{
    stat_t st;
    if (stat(BIG_FILE, &st) != 0 || st.st_size < 2 * 4096) {
        report("read() into untouched heap pages (no big file)", 0);
        return;
    }
    int want = st.st_size < BIG_READ ? (int)st.st_size : BIG_READ;

    char *buf = (char *)malloc(BIG_READ);
    if (!buf) { report("read() into untouched heap pages (malloc)", 0); return; }

    int fd = open(BIG_FILE);
    int got = (fd >= 0) ? read(fd, buf, want) : -1;   /* the ring-0 faults */
    if (fd >= 0) close(fd);

    int ok = (got == want);
    if (ok) {
        char chunk[CHUNK];
        fd = open(BIG_FILE);
        for (int off = 0; ok && off < want; off += CHUNK) {
            int n = want - off < CHUNK ? want - off : CHUNK;
            ok = (read(fd, chunk, n) == n) && memcmp(chunk, buf + off, n) == 0;
        }
        close(fd);
    }
    free(buf);
    report("read() into untouched heap pages", ok);
}

/*
 * REVIEW.md B -- copy-on-write isolation for kernel writes.  The parent
 * touches a heap page (so it is present, and CoW-shared after fork); the
 * child read()s file data into that same page.  The kernel's write must break
 * CoW: before CR0.WP was set, ring 0 ignored the read-only PTE and wrote the
 * shared frame, so the file data showed up in the parent.
 */
static void test_kernel_write_respects_cow(void)
{
    char *buf = (char *)malloc(4096);
    if (!buf) { report("kernel write breaks CoW after fork", 0); return; }
    memset(buf, 'P', 4096);

    int pid = fork();
    if (pid == 0) {
        int fd = open(BIG_FILE);
        int n  = (fd >= 0) ? read(fd, buf, 4096) : -1;
        exit(n == 4096 ? 0 : 1);
    }
    int code = -1;
    if (pid > 0) wait(pid, &code);

    int intact = 1;
    for (int i = 0; i < 4096; i++)
        if (buf[i] != 'P') { intact = 0; break; }
    free(buf);
    report("kernel write breaks CoW after fork", pid > 0 && code == 0 && intact);
}

/* -- helpers for the lifecycle checks -------------------------------------- */

static void sleep_ms(unsigned ms)
{
    unsigned hz = gethz(), t0 = getticks();
    unsigned want = (ms * hz + 999) / 1000;
    while (getticks() - t0 < want) { }
}

/* Pages leaked by `rounds` runs of fn (after one warm-up run). */
static int leaked_pages(void (*fn)(void), int rounds)
{
    fn();                                   /* warm-up: first-touch growth */
    unsigned before = free_pages();
    for (int i = 0; i < rounds; i++) fn();
    unsigned after = free_pages();
    return (int)before - (int)after;
}

/* A leak of even one address space is >= 3 pages (PD + PT + stack); a tiny
 * tolerance absorbs unrelated kernel bookkeeping. */
#define LEAK_SLACK 2

static void fork_touch_exit(void)
{
    int pid = fork();
    if (pid == 0) {
        char *p = (char *)malloc(64 * 1024);
        for (int i = 0; p && i < 64 * 1024; i += 4096) p[i] = 1;
        exit(0);
    }
    if (pid > 0) wait(pid, (int *)0);
}

static void exec_exit(void)
{
    int pid = exec("hello.elf");
    if (pid > 0) wait(pid, (int *)0);
}

/*
 * REVIEW.md C -- exit + wait must return the child's whole address space.
 * Before the fix only the slot state changed: every fork/exec leaked its page
 * directory, page tables and every page it touched.
 */
static void test_exit_frees_memory(void)
{
    int f = leaked_pages(fork_touch_exit, 4);
    printf("ktest:   fork+exit x4 leaked %d pages\r\n", f);
    report("fork+exit returns all memory", f <= LEAK_SLACK);

    int e = leaked_pages(exec_exit, 4);
    printf("ktest:   exec+exit x4 leaked %d pages\r\n", e);
    report("exec+exit returns all memory", e <= LEAK_SLACK);
}

static void fork_segv(void)
{
    int pid = fork();
    if (pid == 0) {
        *(volatile int *)0 = 1;             /* SIGSEGV */
        exit(0);
    }
    int code = 0;
    if (pid > 0) wait(pid, &code);
}

/*
 * REVIEW.md D -- death by signal goes through the same exit path: memory is
 * returned and the graphics surface is released, so a crashed WM no longer
 * blocks the next startx.  The child also maps the framebuffer, so its
 * teardown exercises PAGE_NOFREE: freeing the kernel's shadow buffer would
 * show up as ~1000 pages "gained".
 */
static void test_signal_death_cleans_up(void)
{
    unsigned before = free_pages();
    int pid = fork();
    if (pid == 0) {
        gfx_info_t info;
        if (!gfx_screen_init(&info)) exit(2);   /* take the screen */
        *(volatile int *)0 = 1;                 /* ...and crash */
        exit(0);
    }
    int code = 0;
    if (pid > 0) wait(pid, &code);
    int delta = (int)free_pages() - (int)before;
    report("SIGSEGV child reports exit code -11", code == -11);
    printf("ktest:   framebuffer owner crash: free pages %+d\r\n", delta);
    report("teardown keeps the kernel framebuffer (PAGE_NOFREE)",
           delta <= LEAK_SLACK && delta >= -LEAK_SLACK);

    int l = leaked_pages(fork_segv, 3);
    printf("ktest:   segv x3 leaked %d pages\r\n", l);
    report("SIGSEGV death returns all memory", l <= LEAK_SLACK);

    /* The crashed child owned the screen; it must be free again.  PIDs are
     * slot numbers, so a placeholder child takes the crashed child's slot
     * first: the claimant gets a DIFFERENT pid and cannot pass on a stale
     * owner record by accident.  Claiming from a child keeps this process out
     * of graphics mode. */
    int hold = fork();
    if (hold == 0) { sleep_ms(300); exit(0); }
    int claim = fork();
    if (claim == 0) {
        gfx_info_t info;
        exit(gfx_screen_init(&info) ? 0 : 1);
    }
    code = -1;
    if (claim > 0) wait(claim, &code);
    if (hold > 0)  wait(hold, (int *)0);
    report("graphics surface released after the owner crashed",
           claim > 0 && claim != pid && code == 0);
}

static void orphan_round(void)
{
    int pid = fork();
    if (pid == 0) {
        if (fork() == 0) {                  /* grandchild outlives its parent */
            sleep_ms(30);
            exit(0);
        }
        exit(0);                            /* parent exits first */
    }
    if (pid > 0) wait(pid, (int *)0);
    sleep_ms(80);                           /* grandchild has exited by now */
}

/*
 * REVIEW.md E -- an orphan (parent exited first) is reaped when it exits,
 * instead of holding its slot and memory forever.  The next process creation
 * (the following round's fork) sweeps it up.
 */
static void test_orphans_are_reaped(void)
{
    int l = leaked_pages(orphan_round, 3);
    printf("ktest:   orphan x3 leaked %d pages\r\n", l);
    report("orphaned processes are reaped", l <= LEAK_SLACK + 4);
}

/*
 * REVIEW.md E -- a shell whose terminal goes away (stdin pipe writer closed)
 * must exit.  It used to retry read() forever, so every closed WM terminal
 * left an orphan shell spinning at 100% CPU.
 */
static void test_shell_exits_on_eof(void)
{
    int in[2], out[2];
    if (pipe(in) || pipe(out)) { report("shell exits on stdin EOF (pipe)", 0); return; }
    int pid = exec_redir("shell.elf", in[0], out[1]);
    if (pid < 0) { report("shell exits on stdin EOF (exec)", 0); return; }
    close(in[1]);                           /* the terminal "closes" */

    int state = 1;
    char sink[256];
    unsigned hz = gethz(), t0 = getticks();
    while (state == 1 && getticks() - t0 < 2 * hz) {
        while (read_nonblock(out[0], sink, sizeof sink) > 0) { }  /* drain */
        state = wait_nohang(pid, (int *)0);
    }
    close(out[0]);
    report("shell exits on stdin EOF (and is reaped)", state == 0);
}

/*
 * REVIEW.md H -- ring 3 must not reach kernel memory.  usermode_initialize()
 * used to set PAGE_USER on the identity map AND the kernel-high window (they
 * share one page table) for the old in-kernel ring-3 demos, so every process
 * could read and write the kernel image, PCBs and page tables: a stray app
 * pointer silently corrupted the kernel instead of raising SIGSEGV.
 * Each probe runs in a child; being killed by SIGSEGV (-11) is the pass.
 */
static int probe_kernel(char which)
{
    int in[2];
    if (pipe(in)) return 99;
    write(in[1], &which, 1);
    int pid = exec_redir("kprobe.elf", in[0], -1);   /* child owns in[0] */
    close(in[1]);
    int code = 99;
    if (pid > 0) wait(pid, &code);
    return code;
}

static void test_kernel_memory_isolated(void)
{
    report("ring 3 cannot read kernel text (0xC0100000)",
           probe_kernel('k') == -11);
    report("ring 3 cannot read the identity map (0x00100000)",
           probe_kernel('i') == -11);
}

/*
 * REVIEW.md F -- the VFS and the FAT16/ext2 drivers had no locking.  Syscalls
 * are preemptible, and the drivers share static scratch buffers, so two
 * processes reading files at once could get each other's sectors.  Several
 * processes read the same file in small chunks (many driver calls, many
 * chances to be preempted mid-read) and compare checksums with a reference.
 */
static unsigned file_checksum(const char *path, int limit)
{
    char chunk[512];
    unsigned sum = 2166136261u;             /* FNV-1a */
    int fd = open(path);
    if (fd < 0) return 0;
    int total = 0, n;
    while (total < limit && (n = read(fd, chunk, sizeof chunk)) > 0) {
        for (int i = 0; i < n; i++) sum = (sum ^ (unsigned char)chunk[i]) * 16777619u;
        total += n;
    }
    close(fd);
    return sum;
}

#define RACE_PROCS  4
#define RACE_ROUNDS 150
#define RACE_LIMIT  (8 * 1024)

/* Each racer uses a DIFFERENT file: if two concurrent open()s are handed the
 * same fd slot, one process silently reads the other's file -- invisible if
 * they all read the same one.  (The vfs_open window is the driver's whole
 * directory lookup: several disk reads, easily preempted.) */
static const char *race_files[RACE_PROCS] = {
    "wm.elf", "shell.elf", "wget.elf", "gterm.elf"
};

static int race_worker(int p, unsigned ref)
{
    int bad = 0;
    for (int r = 0; r < RACE_ROUNDS; r++)
        if (file_checksum(race_files[p], RACE_LIMIT) != ref) bad++;
    return bad;
}

static void test_concurrent_reads(void)
{
    unsigned ref[RACE_PROCS];
    int refs_ok = 1;
    for (int p = 0; p < RACE_PROCS; p++) {
        ref[p] = file_checksum(race_files[p], RACE_LIMIT);
        if (!ref[p]) refs_ok = 0;
    }
    unsigned t0 = getticks();
    int pids[RACE_PROCS];
    for (int p = 1; p < RACE_PROCS; p++) {
        pids[p] = fork();
        if (pids[p] == 0) exit(race_worker(p, ref[p]));
    }
    int bad = race_worker(0, ref[0]);
    for (int p = 1; p < RACE_PROCS; p++) {
        int code = 99;
        if (pids[p] > 0) wait(pids[p], &code);
        bad += code;
    }
    printf("ktest:   %d of %d concurrent file reads got wrong data (%u ticks)\r\n",
           bad, RACE_PROCS * RACE_ROUNDS, getticks() - t0);
    report("concurrent opens/reads of different files stay correct",
           refs_ok && bad == 0);
}

/*
 * Lost wakeup in wait() (found by the concurrent-read check, which hung ~1 run
 * in 4): the parent is woken by ANY child's exit, but SYS_WAIT set BLOCKED
 * only once -- after a sibling's exit woke it, it slept again still RUNNING
 * and the awaited child's exit, which only wakes a BLOCKED parent, was lost.
 * Deterministic: wait for a slow child while a fast sibling exits first.
 * On the old kernel this hangs (no KTEST line -> the run fails).
 */
static void test_wait_survives_sibling_exit(void)
{
    int slow = fork();
    if (slow == 0) { sleep_ms(300); exit(7); }
    int fast = fork();
    if (fast == 0) exit(0);                 /* exits while we wait for slow */
    int code = -1, fcode = -1;
    int r1 = (slow > 0) ? wait(slow, &code) : -1;
    int r2 = (fast > 0) ? wait(fast, &fcode) : -1;
    report("wait() for one child survives a sibling exiting first",
           r1 == 0 && code == 7 && r2 == 0 && fcode == 0);
}

/*
 * ROADMAP3 17.0.1 -- signals reach a process wherever it is.
 *
 * Before: there was no kill() syscall, and signals were acted on only at
 * syscall exit -- a process spinning in user code, or asleep in a blocking
 * read, could never be stopped (so Ctrl-C could not work).  On the old kernel
 * these checks hang (kill() is an unknown syscall, the child never dies, and
 * wait() never returns), so the run fails with no KTEST line.
 */
static int wait_code(int pid)
{
    int code = 12345;
    if (pid <= 0 || wait(pid, &code) != 0) return 12345;
    return code;
}

static void test_kill_reaches_any_state(void)
{
    /* Spinning in ring 3, never making a syscall: needs the timer-IRQ path. */
    int spin = fork();
    if (spin == 0) { for (;;) { } }
    sleep_ms(50);
    int k = kill(spin, SIGTERM);
    report("kill() stops a child spinning in user code (-15)",
           k == 0 && wait_code(spin) == -SIGTERM);

    /* Asleep in a pipe read with no writer activity. */
    int fds[2];
    if (pipe(fds) != 0) { report("kill() wakes a blocked pipe reader (pipe)", 0); return; }
    int rd = fork();
    if (rd == 0) {
        char c;
        read(fds[0], &c, 1);            /* blocks: nobody writes */
        exit(0);
    }
    sleep_ms(50);
    k = kill(rd, SIGINT);
    report("kill() wakes a child blocked in a pipe read (-2)",
           k == 0 && wait_code(rd) == -SIGINT);
    close(fds[0]);
    close(fds[1]);

    /* Asleep reading the keyboard (stdin is the console under ktest). */
    int kb = fork();
    if (kb == 0) {
        char c;
        read(STDIN_FILENO, &c, 1);      /* blocks: nobody types */
        exit(0);
    }
    sleep_ms(50);
    k = kill(kb, SIGKILL);
    report("kill() wakes a child blocked reading the keyboard (-9)",
           k == 0 && wait_code(kb) == -SIGKILL);

    report("kill() of a nonexistent pid fails", kill(1000, SIGTERM) == -1);
}

/*
 * ROADMAP3 17.0.1 -- Ctrl-C reaches the foreground job, and only it.  intr()
 * is what the desktop terminal calls on Ctrl-C (the console path runs the
 * same process_intr_target() from the keyboard IRQ).
 */
static void test_ctrl_c_foreground_job(void)
{
    me_pid = getpid();
    report("intr() with no foreground job sends nothing", intr(me_pid) == 0);

    int job = fork();
    if (job == 0) { for (;;) { } }
    setfg(job);
    int r = intr(me_pid);
    int code = wait_code(job);
    setfg(-1);
    report("intr() SIGINTs the foreground job (-2)", r == 1 && code == -SIGINT);

    /* A job that ignores SIGINT (like the WM) gets the key as data instead. */
    int ready[2];
    if (pipe(ready) != 0) { report("SIG_IGN job is not interrupted (pipe)", 0); return; }
    int ign = fork();
    if (ign == 0) {
        signal(SIGINT, SIG_IGN);
        write(ready[1], "r", 1);
        for (;;) { }
    }
    char c;
    read(ready[0], &c, 1);              /* child has ignored SIGINT */
    setfg(ign);
    r = intr(me_pid);
    int k2 = kill(ign, SIGINT);         /* ignored: discarded, still alive */
    sleep_ms(30);
    int alive = (wait_nohang(ign, (int *)0) == 1);
    kill(ign, SIGKILL);
    code = wait_code(ign);
    setfg(-1);
    close(ready[0]);
    close(ready[1]);
    report("a job ignoring SIGINT is not interrupted (intr()==0, survives)",
           r == 0 && k2 == 0 && alive && code == -SIGKILL);

    report("signal() rejects ignoring SIGKILL",
           signal(SIGKILL, SIG_IGN) == SIG_ERR);
}

int main(void)
{
    printf("ktest: kernel regression tests\r\n");
    test_read_into_untouched_heap();
    test_kernel_write_respects_cow();
    test_exit_frees_memory();
    test_signal_death_cleans_up();
    test_orphans_are_reaped();
    test_shell_exits_on_eof();
    test_kernel_memory_isolated();
    test_wait_survives_sibling_exit();
    test_concurrent_reads();
    test_kill_reaches_any_state();
    test_ctrl_c_foreground_job();

    if (failures == 0) printf("KTEST: ALL PASS\r\n");
    else               printf("KTEST: %d FAILED\r\n", failures);
    return failures ? 1 : 0;
}
