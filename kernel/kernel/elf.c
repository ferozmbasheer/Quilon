/*
 * Quilon OS - ELF32 Loader (section 4.12)
 *
 * Loads an ELF32 i386 executable from the VFS into virtual memory and
 * returns the program's entry-point address.
 *
 * Background: what is ELF?
 * ────────────────────────
 * ELF (Executable and Linkable Format) is the standard binary format on
 * Linux and most Unix-like systems.  A compiled program is an ELF file; so
 * are shared libraries and kernel modules.  An ELF file contains:
 *
 *   ELF header (52 bytes)
 *     Identifies the file (magic bytes, architecture, entry point) and tells
 *     the loader where to find the program header table.
 *
 *   Program header table (array of 32-byte entries)
 *     Each entry describes one "segment" - a contiguous region of the file
 *     that must be placed somewhere in virtual memory before the program runs.
 *     The loader only cares about PT_LOAD entries; all others are ignored.
 *
 *   PT_LOAD segment
 *     p_vaddr:  where in virtual memory this segment lives.
 *     p_offset: where in the file this segment's data starts.
 *     p_filesz: how many bytes to copy from the file.
 *     p_memsz:  how much virtual memory to reserve (>= p_filesz).
 *               The extra p_memsz − p_filesz bytes are BSS (zero-filled data).
 *
 * How this loader works
 * ─────────────────────
 * 1. Read the entire ELF into a kmalloc heap buffer.
 *    (Small programs are at most a few KiB, so whole-file buffering is fine.)
 *
 * 2. Walk the program header table.  For each PT_LOAD entry:
 *      a. Page-align the virtual range [p_vaddr, p_vaddr + p_memsz).
 *      b. For each page: allocate a fresh physical page with pmm_alloc_page(),
 *         zero it (covering BSS), and map it at the virtual address via
 *         paging_map_page_alloc() with PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER.
 *      c. Copy p_filesz bytes from the heap buffer to the virtual address.
 *
 * 3. Return e_entry - the first virtual address the CPU should execute.
 *    The caller does:  usermode_enter((void(*)(void))(uintptr_t)entry);
 *
 * Why PAGE_USER on segment pages?
 * ────────────────────────────────
 * Ring-3 code needs the PAGE_USER bit set on every page it touches.
 * The loader maps them PAGE_USER here so usermode_enter() can jump straight
 * to e_entry without any additional page-table adjustments.
 *
 * Why allocate new physical pages instead of reusing existing mappings?
 * ──────────────────────────────────────────────────────────────────────
 * Each process should own its memory privately.  If two processes loaded the
 * same binary, they must each get their own writable copy of BSS and data.
 * Allocating fresh pages guarantees isolation even in our single-process
 * kernel (it prevents the ELF from overwriting kernel or heap data).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <kernel/elf.h>

#ifdef __is_kernel
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/pmm.h>
#include <kernel/paging.h>
#include <kernel/usermode.h>
#endif

/* Maximum ELF file size accepted by the loader.
 * 256 KiB fits any hello-world-scale user program.  Raise if needed.    */
#define ELF_MAX_SIZE (256u * 1024u)

/* ── Pure parsing helpers ────────────────────────────────────────────────────
 * These functions depend only on the buffer pointer and arithmetic - no
 * kernel services.  They compile and run identically on the host for tests.
 * ─────────────────────────────────────────────────────────────────────────── */

int elf_validate(const uint8_t *buf, uint32_t size)
{
    if (buf == NULL)                      return -1;
    if (size < sizeof(elf32_ehdr_t))      return -1;

    const elf32_ehdr_t *h = (const elf32_ehdr_t *)buf;

    /* ── e_ident field checks ─────────────────────────────────────────────── */

    /* Bytes 0-3: ELF magic number */
    if (h->e_ident[0] != 0x7Fu ||
        h->e_ident[1] != 'E'   ||
        h->e_ident[2] != 'L'   ||
        h->e_ident[3] != 'F')
        return -1;

    /* Byte 4: EI_CLASS - must be 32-bit */
    if (h->e_ident[4] != ELFCLASS32)  return -1;

    /* Byte 5: EI_DATA - must be little-endian */
    if (h->e_ident[5] != ELFDATA2LSB) return -1;

    /* Byte 6: EI_VERSION - must equal EV_CURRENT */
    if (h->e_ident[6] != EV_CURRENT)  return -1;

    /* ── Header field checks ─────────────────────────────────────────────── */

    /* Must be an executable, not a shared library or object file */
    if (h->e_type    != ET_EXEC)           return -1;

    /* Must target Intel 386 */
    if (h->e_machine != EM_386)            return -1;

    /* Program header entry size must exactly match our struct.
     * (Larger entries would indicate a newer ELF variant we don't handle.) */
    if (h->e_phentsize != sizeof(elf32_phdr_t)) return -1;

    /* Program header table must be present */
    if (h->e_phoff == 0)  return -1;

    /* Program header table must fit inside the buffer we have */
    if ((uint32_t)h->e_phoff +
        (uint32_t)h->e_phnum * sizeof(elf32_phdr_t) > size)
        return -1;

    return 0;
}

const elf32_phdr_t *elf_phdr(const uint8_t *buf, uint16_t i)
{
    const elf32_ehdr_t *h = (const elf32_ehdr_t *)buf;
    /* Cast to byte pointer for arithmetic, then step by e_phentsize × i. */
    return (const elf32_phdr_t *)(
        buf + h->e_phoff + (uint32_t)i * h->e_phentsize);
}

/* ── Kernel-only: full ELF loader ─────────────────────────────────────────── */

#ifdef __is_kernel

uint32_t elf_load(const char *path)
{
    /* ── Step 1: open the file from VFS ──────────────────────────────────── */
    int fd = vfs_open(path);
    if (fd < 0) {
        printf("[elf] cannot open '%s'\r\n", path);
        return 0;
    }

    /* ── Step 2: read the entire file into a heap buffer ─────────────────── */
    uint8_t *buf = (uint8_t *)kmalloc(ELF_MAX_SIZE);
    if (!buf) {
        printf("[elf] out of heap memory for '%s'\r\n", path);
        vfs_close(fd);
        return 0;
    }

    uint32_t total = 0;
    int      n;
    while (total < ELF_MAX_SIZE &&
           (n = vfs_read(fd, buf + total, 512)) > 0)
        total += (uint32_t)n;
    vfs_close(fd);

    if (total == 0) {
        printf("[elf] '%s' is empty\r\n", path);
        kfree(buf);
        return 0;
    }

    /* ── Step 3: validate the ELF header ─────────────────────────────────── */
    if (elf_validate(buf, total) != 0) {
        printf("[elf] '%s': not a valid ELF32 i386 executable\r\n", path);
        kfree(buf);
        return 0;
    }

    const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)buf;
    printf("[elf] '%s': entry=0x%x  phnum=%d\r\n",
           path, (unsigned)ehdr->e_entry, (int)ehdr->e_phnum);

    /* ── Step 4: map and populate each PT_LOAD segment ───────────────────── */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf32_phdr_t *ph = elf_phdr(buf, i);

        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0)      continue;   /* nothing to map */

        /* Sanity-check: file data must be within the buffer */
        if (ph->p_filesz > 0 &&
            ph->p_offset + ph->p_filesz > total) {
            printf("[elf] segment %d extends past end of file\r\n", (int)i);
            kfree(buf);
            return 0;
        }

        /* Page-align the virtual range.
         *
         * virt_start - round p_vaddr DOWN to the nearest page boundary.
         * virt_end   - round p_vaddr+p_memsz UP to the nearest page boundary.
         *
         * Example: p_vaddr=0x401010, p_memsz=20
         *   virt_start = 0x401000 (page containing the segment start)
         *   virt_end   = 0x402000 (first page entirely past the segment end)
         *   → one page to map: 0x401000
         */
        uint32_t virt_start = ph->p_vaddr & ~(PAGE_SIZE - 1u);
        uint32_t virt_end   = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1u)
                              & ~(PAGE_SIZE - 1u);

        printf("[elf] segment %d: vaddr=0x%x  filesz=%d  memsz=%d  "
               "flags=0x%x\r\n",
               (int)i, (unsigned)ph->p_vaddr,
               (int)ph->p_filesz, (int)ph->p_memsz, (unsigned)ph->p_flags);

        /* Allocate and map one physical page per virtual page in the range */
        for (uint32_t virt = virt_start; virt < virt_end; virt += PAGE_SIZE) {
            void *phys = pmm_alloc_page();
            if (!phys) {
                printf("[elf] PMM out of pages at virt=0x%x\r\n",
                       (unsigned)virt);
                kfree(buf);
                return 0;
            }

            /* Zero the physical page.  This handles BSS automatically:
             * the extra (p_memsz − p_filesz) bytes are never written by
             * the memcpy below, so they stay zero.                        */
            memset(phys, 0, PAGE_SIZE);

            /* Map: present, writable (for BSS init), user-accessible.
             * paging_map_page_alloc() allocates a page table if this is the
             * first page mapped in this 4-MiB PD slot.                    */
            if (paging_map_page_alloc(virt, (uint32_t)(uintptr_t)phys,
                    PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER) != 0) {
                printf("[elf] mapping failed at virt=0x%x\r\n",
                       (unsigned)virt);
                pmm_free_page(phys);
                kfree(buf);
                return 0;
            }
        }

        /* Copy the file image into virtual memory.
         *
         * After paging_map_page_alloc() returns, writing to p_vaddr goes
         * through the MMU and lands in the freshly allocated physical pages.
         * The BSS region (if any) is already zero from memset above.       */
        if (ph->p_filesz > 0)
            memcpy((void *)(uintptr_t)ph->p_vaddr,
                   buf + ph->p_offset,
                   ph->p_filesz);
    }

    /* ── Step 5: return the entry point ──────────────────────────────────── */
    uint32_t entry = ehdr->e_entry;
    kfree(buf);

    printf("[elf] loaded '%s' - entry=0x%x\r\n", path, (unsigned)entry);
    return entry;
}

/*
 * elf_load_into - load an ELF32 executable into a specific page directory.
 *
 * This is the process-isolation version of elf_load().  Instead of mapping
 * segments into the global (kernel) page directory, it maps them into the
 * target_pd provided by the caller (typically created by
 * paging_create_address_space() for a new child process).
 *
 * Key difference: physical pages are written to via their identity-mapped
 * physical addresses (not via the ELF virtual address), so the data lands
 * in the right physical memory even though target_pd is not active in CR3.
 *
 * This allows the kernel to prepare a child's address space entirely in
 * the background, before the child is ever scheduled.
 */
uint32_t elf_load_into(const char *path, uint32_t *target_pd)
{
    /* ── Step 1: open the file ───────────────────────────────────────────── */
    int fd = vfs_open(path);
    if (fd < 0) {
        printf("[elf] cannot open '%s'\r\n", path);
        return 0;
    }

    /* ── Step 2: read into heap buffer ──────────────────────────────────── */
    uint8_t *buf = (uint8_t *)kmalloc(ELF_MAX_SIZE);
    if (!buf) {
        printf("[elf] out of heap memory for '%s'\r\n", path);
        vfs_close(fd);
        return 0;
    }

    uint32_t total = 0;
    int n;
    while (total < ELF_MAX_SIZE && (n = vfs_read(fd, buf + total, 512)) > 0)
        total += (uint32_t)n;
    vfs_close(fd);

    if (total == 0) {
        printf("[elf] '%s' is empty\r\n", path);
        kfree(buf);
        return 0;
    }

    /* ── Step 3: validate ────────────────────────────────────────────────── */
    if (elf_validate(buf, total) != 0) {
        printf("[elf] '%s': not a valid ELF32 i386 executable\r\n", path);
        kfree(buf);
        return 0;
    }

    const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)buf;
    printf("[elf] loading '%s' into pd=0x%x  entry=0x%x  phnum=%d\r\n",
           path, (unsigned)(uintptr_t)target_pd,
           (unsigned)ehdr->e_entry, (int)ehdr->e_phnum);

    /* ── Step 4: map segments into target_pd ─────────────────────────────── */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf32_phdr_t *ph = elf_phdr(buf, i);

        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0)      continue;

        if (ph->p_filesz > 0 &&
            ph->p_offset + ph->p_filesz > total) {
            printf("[elf] segment %d extends past end of file\r\n", (int)i);
            kfree(buf);
            return 0;
        }

        uint32_t virt_start = ph->p_vaddr & ~(PAGE_SIZE - 1u);
        uint32_t virt_end   = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1u)
                              & ~(PAGE_SIZE - 1u);

        printf("[elf] segment %d: vaddr=0x%x  filesz=%d  memsz=%d\r\n",
               (int)i, (unsigned)ph->p_vaddr,
               (int)ph->p_filesz, (int)ph->p_memsz);

        for (uint32_t virt = virt_start; virt < virt_end; virt += PAGE_SIZE) {
            /* Allocate a fresh physical page (in the first 4 MiB, so
             * its physical address equals its virtual address).        */
            uint8_t *phys_page = (uint8_t *)pmm_alloc_page();
            if (!phys_page) {
                printf("[elf] PMM out of pages at virt=0x%x\r\n",
                       (unsigned)virt);
                kfree(buf);
                return 0;
            }
            memset(phys_page, 0, PAGE_SIZE);   /* zero including BSS  */

            /* Map the physical page at virt in the child's PD. */
            if (paging_map_page_alloc_into(
                    target_pd, virt,
                    (uint32_t)(uintptr_t)phys_page,
                    PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER) != 0) {
                printf("[elf] mapping failed at virt=0x%x\r\n", (unsigned)virt);
                pmm_free_page(phys_page);
                kfree(buf);
                return 0;
            }

            /*
             * Copy the portion of this segment that overlaps the current page.
             *
             * The page covers [virt, virt + PAGE_SIZE).
             * File data covers [p_vaddr, p_vaddr + p_filesz).
             * Intersection: [copy_start, copy_end).
             *
             * We write to phys_page (identity-mapped), not to virt -
             * because virt lives in target_pd which is not currently
             * active in CR3.
             */
            if (ph->p_filesz > 0) {
                uint32_t copy_start =
                    (ph->p_vaddr > virt) ? ph->p_vaddr : virt;
                uint32_t copy_end =
                    (ph->p_vaddr + ph->p_filesz < virt + PAGE_SIZE)
                    ? ph->p_vaddr + ph->p_filesz : virt + PAGE_SIZE;

                if (copy_start < copy_end) {
                    uint8_t       *dst = phys_page + (copy_start - virt);
                    const uint8_t *src = buf + ph->p_offset
                                         + (copy_start - ph->p_vaddr);
                    memcpy(dst, src, copy_end - copy_start);
                }
            }
        }
    }

    /* ── Step 5: allocate and map a per-process user stack ──────────────── */
    /*
     * Each ELF process gets its own private 4-KiB stack page mapped at
     * [USER_STACK_TOP - PAGE_SIZE, USER_STACK_TOP) in target_pd.
     * Ring-3 ESP starts at USER_STACK_TOP (top of this page) and grows
     * downward into it.
     *
     * Placing the stack high (near 3 GiB) keeps it well clear of the ELF
     * text/data segments at 0x400000 and the heap above them, so no two
     * processes can accidentally share stack memory even if they are both
     * mapped at the same virtual addresses in different page directories.
     */
    uint8_t *stack_phys = (uint8_t *)pmm_alloc_page();
    if (!stack_phys) {
        printf("[elf] PMM out of pages for stack of '%s'\r\n", path);
        kfree(buf);
        return 0;
    }
    memset(stack_phys, 0, PAGE_SIZE);

    if (paging_map_page_alloc_into(
            target_pd,
            USER_STACK_TOP - PAGE_SIZE,
            (uint32_t)(uintptr_t)stack_phys,
            PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER) != 0) {
        printf("[elf] stack page mapping failed for '%s'\r\n", path);
        pmm_free_page(stack_phys);
        kfree(buf);
        return 0;
    }

    uint32_t entry = ehdr->e_entry;
    kfree(buf);
    printf("[elf] '%s' ready in pd=0x%x - entry=0x%x  stack=0x%x\r\n",
           path, (unsigned)(uintptr_t)target_pd,
           (unsigned)entry, (unsigned)USER_STACK_TOP);
    return entry;
}

#endif /* __is_kernel */
