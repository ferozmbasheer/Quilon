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
#include <kernel/vma.h>
#include <kernel/process.h>
#include <kernel/initrd.h>
#include <kernel/pci.h>
#include <kernel/rtl8139.h>
#include <kernel/net.h>
#include <kernel/vbe.h>

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
    uint32_t entry = elf_load_into(path, proc_pd, NULL);  /* ring-0 path: no VMA table */
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
    paging_switch(paging_kernel_cr3());
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

    paging_switch(paging_kernel_cr3());
    asm volatile("sti");
    exec_return_active = 0;
    pmm_free_page(proc_pd);
    printf("%s: task exited; back in kernel\r\n", label);
}

static void shell_cmd_initrd(void)
{
    /* Build a tiny 3-file initrd image in kernel memory and demonstrate
     * mount, readdir, and open+read through the driver API directly.
     * Using the ops vtable avoids replacing the current VFS mount.     */
    static uint8_t    img[256];
    static initrd_ctx_t ctx;

    printf("=== initrd demo (section 8.3) ===\r\n");

    uint32_t img_size = initrd_build_demo(img, sizeof(img));
    if (img_size == 0 || initrd_mount(&ctx, img, img_size) != 0) {
        printf("initrd: demo build failed\r\n");
        return;
    }
    printf("1. mounted: %d files\r\n", (int)ctx.file_count);

    /* List all entries via readdir. */
    vfs_dirent_t ent;
    uint32_t idx = 0;
    while (initrd_vfs_ops.readdir(&ctx, idx, &ent) == 0) {
        printf("   [%d] %s  (%d bytes)\r\n", (int)idx,
               ent.name, (int)ent.size);
        idx++;
    }

    /* Open and read MOTD.TXT. */
    vfs_node_t nd = {0};
    if (initrd_vfs_ops.open(&ctx, "MOTD.TXT", &nd) == 0) {
        char buf[64];
        int n = initrd_vfs_ops.read(&ctx, &nd, 0,
                                    sizeof(buf) - 1, (uint8_t *)buf);
        if (n > 0) { buf[n] = '\0'; printf("2. MOTD.TXT: \"%s\"\r\n", buf); }
    } else {
        printf("2. MOTD.TXT: not found\r\n");
    }

    /* Verify write/create/remove are not supported (read-only FS). */
    printf("3. write=%-3s  create=%-3s  remove=%s  (read-only)\r\n",
           initrd_vfs_ops.write  ? "yes" : "no",
           initrd_vfs_ops.create ? "yes" : "no",
           initrd_vfs_ops.remove ? "yes" : "no");

    printf("=== done ===\r\n");
}

static void shell_cmd_cow(void)
{
    printf("=== CoW fork demo (section 9.3) ===\r\n");

    /* ── 1. PMM reference-count lifecycle ───────────────────────────── */
    uint32_t free0 = pmm_free_page_count();
    void *page_a = pmm_alloc_page();
    if (!page_a) { printf("cow: out of memory\r\n"); return; }

    printf("1. alloc page_a @ 0x%x  refcount=%d\r\n",
           (unsigned)(uintptr_t)page_a, (int)pmm_page_refcount(page_a));

    pmm_ref_page(page_a);
    printf("2. pmm_ref_page  -> refcount=%d  (shared between two PTEs)\r\n",
           (int)pmm_page_refcount(page_a));

    pmm_free_page(page_a);
    printf("3. first free    -> refcount=%d  (page NOT released)\r\n",
           (int)pmm_page_refcount(page_a));

    pmm_free_page(page_a);
    printf("4. second free   -> free count=%d  (restored to %d: page freed)\r\n",
           (int)pmm_free_page_count(), (int)free0);

    /* ── 2. CoW fork: writable page shared, not copied ──────────────── */
    printf("5. PAGE_COW=0x%x  (bit 9, OS-reserved PTE bit)\r\n",
           (unsigned)PAGE_COW);

    uint32_t *pd_p = paging_create_address_space();
    uint32_t *pd_c = paging_create_address_space();
    void     *upage = pmm_alloc_page();

    if (pd_p && pd_c && upage) {
        /* Map a writable user page in the parent's address space. */
        paging_map_page_alloc_into(pd_p, 0x00400000u,
            (uint32_t)(uintptr_t)upage,
            PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);

        uint32_t pre = pmm_free_page_count();
        paging_fork_address_space(pd_p, pd_c);
        uint32_t post = pmm_free_page_count();

        printf("6. free pages before CoW fork: %d\r\n", (int)pre);
        printf("   free pages after  CoW fork: %d  "
               "(only PT alloc, data page shared)\r\n", (int)post);
        printf("   data page refcount: %d  (expect 2)\r\n",
               (int)pmm_page_refcount(upage));

        /* Inspect parent PDE: writable bit should be clear, COW set. */
        uint32_t pd_idx = VIRT_PD_INDEX(0x00400000u);
        if (pd_p[pd_idx] & PAGE_PRESENT) {
            uint32_t *pt_p = (uint32_t *)(pd_p[pd_idx] & ~(uint32_t)0xFFF);
            uint32_t  pte  = pt_p[VIRT_PT_INDEX(0x00400000u)];
            printf("   parent PTE flags: WRITABLE=%d  COW=%d  "
                   "(expect 0, 1)\r\n",
                   (int)!!(pte & PAGE_WRITABLE),
                   (int)!!(pte & PAGE_COW));
        }

        pmm_free_page(pd_p);
        pmm_free_page(pd_c);
        pmm_free_page(upage);
    }

    printf("7. fork+write demo: run from SHELL.ELF with 'cow' command\r\n");
    printf("=== done ===\r\n");
}

static void shell_cmd_pipetest(void)
{
    const char *msg = "Hello through the pipe!";
    int  msglen = 0;
    while (msg[msglen]) msglen++;

    printf("=== Pipe demo (section 8.2) ===\r\n");

    /* Create a pipe: fds[0] = read end, fds[1] = write end. */
    int fds[2];
    if (vfs_pipe(fds) != 0) {
        printf("pipetest: vfs_pipe() failed\r\n");
        return;
    }
    printf("1. pipe created: read_fd=%d  write_fd=%d\r\n", fds[0], fds[1]);

    /* Write into the write end. */
    int n = vfs_write(fds[1], msg, (uint32_t)msglen);
    printf("2. wrote %d bytes to write_fd\r\n", n);

    /* Close the write end so the read end sees EOF after draining. */
    vfs_close(fds[1]);
    printf("3. write end closed\r\n");

    /* Read from the read end. */
    char buf[64];
    int r = vfs_read(fds[0], buf, sizeof(buf) - 1);
    if (r > 0) {
        buf[r] = '\0';
        printf("4. read %d bytes: \"%s\"\r\n", r, buf);
        printf("   %s\r\n", strcmp(buf, msg) == 0 ? "content matches" : "MISMATCH");
    } else {
        printf("4. read returned %d\r\n", r);
    }

    /* Second read should return 0 (EOF). */
    int eof = vfs_read(fds[0], buf, sizeof(buf));
    printf("5. second read (EOF expected): %d\r\n", eof);

    vfs_close(fds[0]);
    printf("=== done ===\r\n");
}

static void shell_cmd_net(void)
{
    printf("=== RTL8139 Network Card Driver (section 10.2) ===\r\n");

    if (!rtl8139_is_ready()) {
        if (rtl8139_init() != 0) {
            printf("rtl8139: not present\r\n");
            printf("  (add -device rtl8139,netdev=net0"
                   " -netdev user,id=net0 to qemu.sh)\r\n");
            printf("=== done ===\r\n");
            return;
        }
    }

    printf("rtl8139: status: ready\r\n");

    uint8_t mac[RTL8139_MAC_LEN];
    rtl8139_get_mac(mac);
    printf("rtl8139: MAC = %02x:%02x:%02x:%02x:%02x:%02x\r\n",
           (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
           (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);

    printf("rtl8139: RX buffer: %u bytes  TX slots: %u\r\n",
           (unsigned)RTL8139_RX_BUF_SIZE, (unsigned)RTL8139_TX_SLOTS);
    printf("rtl8139: max frame: %u bytes\r\n", (unsigned)RTL8139_MAX_ETH_FRAME);
    printf("=== done ===\r\n");
}

/* Parse "a.b.c.d" into a host-byte-order uint32_t.  Returns 0 on error. */
static uint32_t parse_ipv4(const char *s)
{
    uint32_t ip = 0;
    int octet = 0, dots = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            octet = octet * 10 + (*s - '0');
            if (octet > 255) return 0;
        } else if (*s == '.') {
            ip = (ip << 8) | (uint32_t)octet;
            octet = 0;
            dots++;
            if (dots > 3) return 0;
        } else {
            return 0;
        }
        s++;
    }
    if (dots != 3) return 0;
    return (ip << 8) | (uint32_t)octet;
}

static void shell_cmd_ping(const char *arg)
{
    if (arg[0] == '\0') {
        printf("Usage: ping <ip>\r\n");
        return;
    }
    uint32_t dst = parse_ipv4(arg);
    if (dst == 0) {
        printf("ping: invalid address '%s'\r\n", arg);
        return;
    }
    if (net_init() != 0) {
        printf("ping: NIC not ready\r\n");
        return;
    }
    printf("PING %d.%d.%d.%d ...\r\n",
           (int)((dst >> 24) & 0xFF), (int)((dst >> 16) & 0xFF),
           (int)((dst >> 8)  & 0xFF), (int)(dst & 0xFF));
    int r = net_ping(dst);
    if (r > 0)
        printf("reply received\r\n");
    else if (r == 0)
        printf("timeout - no reply\r\n");
    else
        printf("error (NIC not ready or no IP configured)\r\n");
}

static void shell_cmd_dhcp(void)
{
    printf("=== DHCP (section 10.3) ===\r\n");
    if (net_init() != 0) {
        printf("dhcp: NIC not ready\r\n");
        return;
    }
    printf("dhcp: sending DISCOVER...\r\n");
    int r = net_dhcp();
    if (r == 0) {
        uint32_t ip = 0;
        net_get_ip(&ip);
        printf("dhcp: ACK - IP = %d.%d.%d.%d\r\n",
               (int)((ip >> 24) & 0xFF), (int)((ip >> 16) & 0xFF),
               (int)((ip >> 8)  & 0xFF), (int)(ip & 0xFF));
    } else {
        printf("dhcp: failed (timeout or no server)\r\n");
    }
    printf("=== done ===\r\n");
}

static void shell_cmd_arp(void)
{
    printf("=== ARP cache (section 10.3) ===\r\n");
    if (net_init() != 0) {
        printf("arp: NIC not ready\r\n");
        return;
    }
    net_arp_cache_print();
    printf("=== done ===\r\n");
}

static void shell_cmd_tcpip(void)
{
    printf("=== TCP/IP stack status (section 10.3) ===\r\n");
    if (net_init() != 0) {
        printf("tcpip: NIC not ready\r\n");
        return;
    }
    uint32_t ip = 0;
    int has_ip = net_get_ip(&ip);
    if (has_ip)
        printf("IP: %d.%d.%d.%d\r\n",
               (int)((ip >> 24) & 0xFF), (int)((ip >> 16) & 0xFF),
               (int)((ip >> 8)  & 0xFF), (int)(ip & 0xFF));
    else
        printf("IP: not configured (run 'dhcp')\r\n");

    printf("TCP state: %d  ARP cache slots: %d\r\n",
           (int)net_tcp_state(), (int)NET_ARP_CACHE_SIZE);
    printf("protocols: Ethernet/ARP/IPv4/ICMP/UDP/TCP/DHCP\r\n");
    printf("=== done ===\r\n");
}

static void shell_cmd_netsend(void)
{
    printf("=== RTL8139 TX/RX demo (section 10.2) ===\r\n");

    if (!rtl8139_is_ready()) {
        printf("netsend: NIC not initialized - run 'net' first\r\n");
        printf("=== done ===\r\n");
        return;
    }

    uint8_t mac[RTL8139_MAC_LEN];
    rtl8139_get_mac(mac);

    /* Transmit an ARP Who-has broadcast for 10.0.2.2 (QEMU gateway). */
    static uint8_t frame[42];
    int i;
    for (i = 0; i < 6; i++) frame[i]      = 0xFFu; /* dst: broadcast */
    for (i = 0; i < 6; i++) frame[6 + i]  = mac[i]; /* src: our MAC  */
    frame[12] = 0x08; frame[13] = 0x06;              /* EtherType ARP */
    frame[14] = 0x00; frame[15] = 0x01;              /* hw: Ethernet  */
    frame[16] = 0x08; frame[17] = 0x00;              /* proto: IPv4   */
    frame[18] = 6;    frame[19] = 4;
    frame[20] = 0x00; frame[21] = 0x01;              /* opcode: req   */
    for (i = 0; i < 6; i++) frame[22 + i] = mac[i]; /* sender MAC    */
    frame[28] = 0; frame[29] = 0; frame[30] = 0; frame[31] = 0;
    for (i = 0; i < 6; i++) frame[32 + i] = 0;      /* target MAC: 0 */
    frame[38] = 10; frame[39] = 0;
    frame[40] = 2;  frame[41] = 2;                   /* 10.0.2.2      */

    printf("1. TX: ARP Who-has 10.0.2.2 (broadcast)...\r\n");
    int tx = rtl8139_send(frame, (uint16_t)sizeof(frame));
    printf("   %s\r\n", tx == 0 ? "OK" : "FAILED");

    /* Poll for a received frame (may be our own loopback on some configs). */
    static uint8_t rx_buf[RTL8139_MAX_ETH_FRAME + 4];
    int received = 0;
    for (int attempt = 0; attempt < 2000000; attempt++) {
        int n = rtl8139_recv(rx_buf, (uint16_t)sizeof(rx_buf));
        if (n > 14) {
            printf("2. RX: %d bytes\r\n", n);
            printf("   src  %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                   (unsigned)rx_buf[6],  (unsigned)rx_buf[7],
                   (unsigned)rx_buf[8],  (unsigned)rx_buf[9],
                   (unsigned)rx_buf[10], (unsigned)rx_buf[11]);
            printf("   dst  %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                   (unsigned)rx_buf[0],  (unsigned)rx_buf[1],
                   (unsigned)rx_buf[2],  (unsigned)rx_buf[3],
                   (unsigned)rx_buf[4],  (unsigned)rx_buf[5]);
            printf("   etype 0x%02x%02x\r\n",
                   (unsigned)rx_buf[12], (unsigned)rx_buf[13]);
            received = 1;
            break;
        }
    }
    if (!received)
        printf("2. RX: no reply (normal without a full TCP/IP stack)\r\n");

    printf("=== done ===\r\n");
}

static void shell_cmd_pci(void)
{
    printf("=== PCI Bus Enumeration (section 10.1) ===\r\n");
    pci_enumerate();
    printf("Found %d device(s):\r\n", pci_device_count);

    for (int i = 0; i < pci_device_count; i++) {
        const pci_device_t *d = &pci_devices[i];
        printf("  [%d] %d:%d.%d  vendor=0x%x  device=0x%x"
               "  class=0x%x (%s)\r\n",
               i,
               (int)d->bus, (int)d->slot, (int)d->func,
               (unsigned)d->vendor_id, (unsigned)d->device_id,
               (unsigned)d->class_code,
               pci_class_name(d->class_code));
    }

    if (pci_device_count == 0)
        printf("  (no PCI devices found)\r\n");

    /* Highlight the RTL8139 if present (useful for section 10.2). */
    const pci_device_t *rtl =
        pci_find_device(PCI_VENDOR_REALTEK, PCI_DEVICE_RTL8139);
    if (rtl)
        printf("  RTL8139 NIC @ %d:%d.%d  "
               "(add -device rtl8139 to qemu.sh)\r\n",
               (int)rtl->bus, (int)rtl->slot, (int)rtl->func);

    printf("=== done ===\r\n");
}

static void shell_cmd_vga(void)
{
    if (!vbe_active()) {
        printf("vga: no VBE framebuffer active\r\n");
        printf("  (add set gfxmode=800x600x32 + set gfxpayload=keep to grub.cfg)\r\n");
        return;
    }
    const vbe_info_t *info = vbe_get_info();
    printf("vga: framebuffer %dx%dx%d @ 0x%x\r\n",
           (int)info->width, (int)info->height, (int)info->bpp,
           (unsigned)info->addr);
    vbe_demo();
}

static void shell_execute(const char *cmd) {
    if (strcmp(cmd, "help") == 0) {
        printf("Commands: help, clear, cls, halt, ticks, seconds,\r\n");
        printf("          ring3, syscall, sbrk, fork, cow, ps, ls,\r\n");
        printf("          cat <file>, touch <file>, write <file> <data>,\r\n");
        printf("          rm <file>, fstest, pipetest, initrd, pci,\r\n");
        printf("          net, netsend, exec <file.elf>\r\n");
        printf("          dhcp, ping <ip>, arp, tcpip, vga\r\n");
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
    } else if (strcmp(cmd, "cow") == 0) {
        shell_cmd_cow();
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
    } else if (strcmp(cmd, "pipetest") == 0) {
        shell_cmd_pipetest();
    } else if (strcmp(cmd, "initrd") == 0) {
        shell_cmd_initrd();
    } else if (strcmp(cmd, "pci") == 0) {
        shell_cmd_pci();
    } else if (strcmp(cmd, "net") == 0) {
        shell_cmd_net();
    } else if (strcmp(cmd, "netsend") == 0) {
        shell_cmd_netsend();
    } else if (strcmp(cmd, "dhcp") == 0) {
        shell_cmd_dhcp();
    } else if (strncmp(cmd, "ping ", 5) == 0) {
        shell_cmd_ping(cmd + 5);
    } else if (strcmp(cmd, "ping") == 0) {
        shell_cmd_ping("");
    } else if (strcmp(cmd, "arp") == 0) {
        shell_cmd_arp();
    } else if (strcmp(cmd, "tcpip") == 0) {
        shell_cmd_tcpip();
    } else if (strcmp(cmd, "vga") == 0) {
        shell_cmd_vga();
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
