#ifndef _KERNEL_ELF_H
#define _KERNEL_ELF_H

#include <stdint.h>
#include <kernel/vma.h>   /* vma_t -- demand paging (section 9.2) */

/* -- ELF identification constants ------------------------------------------ */

#define ELFCLASS32   1u   /* 32-bit objects                                 */
#define ELFDATA2LSB  1u   /* little-endian data encoding                    */
#define EV_CURRENT   1u   /* current ELF format version                     */

/* Object file types (e_type) */
#define ET_EXEC      2u   /* executable file                                */

/* Target machine (e_machine) */
#define EM_386       3u   /* Intel 80386                                    */

/* Segment types (p_type) */
#define PT_NULL      0u   /* unused entry                                   */
#define PT_LOAD      1u   /* loadable segment -- copy to memory              */
#define PT_DYNAMIC   2u   /* dynamic linking information                    */
#define PT_INTERP    3u   /* interpreter path string                        */
#define PT_NOTE      4u   /* auxiliary information                          */

/* Segment permission flags (p_flags) */
#define PF_X         (1u << 0)   /* execute permission                     */
#define PF_W         (1u << 1)   /* write permission                       */
#define PF_R         (1u << 2)   /* read permission                        */

/* -- ELF32 file header (52 bytes) ----------------------------------------- */

/*
 * elf32_ehdr_t -- the first 52 bytes of every ELF32 file.
 *
 * Key fields for a loader:
 *   e_entry     -- virtual address of the first instruction to execute.
 *   e_phoff     -- byte offset of the program header table from the start
 *                 of the file (typically 52, immediately after this header).
 *   e_phentsize -- size of one program header entry (32 for ELF32).
 *   e_phnum     -- number of program header entries.
 */
typedef struct {
    uint8_t  e_ident[16];   /* magic (0x7F 'E' 'L' 'F'), class, encoding, … */
    uint16_t e_type;        /* object file type (ET_EXEC for executables)     */
    uint16_t e_machine;     /* target ISA (EM_386 for i386)                   */
    uint32_t e_version;     /* ELF version (EV_CURRENT = 1)                   */
    uint32_t e_entry;       /* virtual address of the program entry point      */
    uint32_t e_phoff;       /* byte offset of the program header table         */
    uint32_t e_shoff;       /* byte offset of the section header table (0 ok) */
    uint32_t e_flags;       /* processor-specific flags (0 for i386)          */
    uint16_t e_ehsize;      /* ELF header size in bytes (52 for ELF32)        */
    uint16_t e_phentsize;   /* size of one program header entry (32 for ELF32)*/
    uint16_t e_phnum;       /* number of program header entries                */
    uint16_t e_shentsize;   /* size of one section header entry               */
    uint16_t e_shnum;       /* number of section header entries               */
    uint16_t e_shstrndx;    /* index of section-name string table             */
} elf32_ehdr_t;

/* -- ELF32 program header (32 bytes) --------------------------------------- */

/*
 * elf32_phdr_t -- one entry in the program header table.
 *
 * The loader iterates the program header table looking for entries whose
 * p_type == PT_LOAD.  Each PT_LOAD entry describes one contiguous region
 * of the executable that must be copied into virtual memory before the
 * program can run.
 *
 * p_vaddr  -- where in virtual memory to put this segment.
 * p_filesz -- how many bytes to read from the file (may be 0 for BSS-only).
 * p_memsz  -- how many bytes to reserve in virtual memory.  If p_memsz >
 *             p_filesz, the extra bytes are the BSS region and must be
 *             zero-initialised.
 * p_offset -- byte offset in the file where this segment's data begins.
 * p_flags  -- combination of PF_R, PF_W, PF_X.
 * p_align  -- required alignment (power of 2; loader should respect it).
 */
typedef struct {
    uint32_t p_type;    /* segment type                                      */
    uint32_t p_offset;  /* offset of segment data in the file                */
    uint32_t p_vaddr;   /* virtual address to place this segment at          */
    uint32_t p_paddr;   /* physical address (ignored in virtual-memory OSes) */
    uint32_t p_filesz;  /* bytes of data in the file for this segment        */
    uint32_t p_memsz;   /* bytes to occupy in memory (>= p_filesz)          */
    uint32_t p_flags;   /* segment access flags (PF_R | PF_W | PF_X)        */
    uint32_t p_align;   /* alignment constraint (0 or 1 = no constraint)     */
} elf32_phdr_t;

/* -- ELF loader API -------------------------------------------------------- */

/*
 * elf_validate -- verify that `buf` contains a valid ELF32 i386 executable.
 *
 * Checks performed:
 *   • buf is non-NULL and size >= sizeof(elf32_ehdr_t)
 *   • Magic bytes: e_ident[0..3] == { 0x7F, 'E', 'L', 'F' }
 *   • Class: e_ident[4] == ELFCLASS32
 *   • Data encoding: e_ident[5] == ELFDATA2LSB (little-endian)
 *   • ELF version: e_ident[6] == EV_CURRENT
 *   • File type: e_type == ET_EXEC
 *   • Machine: e_machine == EM_386
 *   • Program header entry size: e_phentsize == sizeof(elf32_phdr_t) (32)
 *   • Program header table fits within buf
 *
 * Does NOT validate segment data offsets or sizes.
 *
 * Returns 0 if valid, -1 if any check fails.
 */
int elf_validate(const uint8_t *buf, uint32_t size);

/*
 * elf_phdr -- return a read-only pointer to program header entry i.
 *
 * Precondition: elf_validate(buf, size) returned 0 and i < ehdr->e_phnum.
 * No bounds check is performed -- the caller is responsible for i.
 *
 * Returns a pointer into buf (the same storage; not a copy).
 */
const elf32_phdr_t *elf_phdr(const uint8_t *buf, uint16_t i);

/*
 * elf_load -- load an ELF32 i386 executable from the VFS into virtual memory.
 *
 * This function ties together every major kernel subsystem built in
 * sections 4.1–4.11 of the Quilon roadmap:
 *
 *   VFS (4.11)  -> opens and reads the binary
 *   kmalloc (4.4) -> holds the file in memory while parsing
 *   PMM (4.2)   -> allocates physical pages for each PT_LOAD segment
 *   Paging (4.3) -> maps those pages at the ELF's requested virtual addresses
 *
 * Algorithm:
 *   1. vfs_open(path) -- open the file.
 *   2. Read up to ELF_MAX_SIZE bytes into a heap buffer.
 *   3. elf_validate() -- check magic, class, machine.
 *   4. For each PT_LOAD program header:
 *       a. page-align the virtual address range [p_vaddr, p_vaddr+p_memsz)
 *       b. allocate one physical page per virtual page via pmm_alloc_page()
 *       c. zero-initialise the physical page (handles BSS automatically)
 *       d. map it via paging_map_page_alloc() with PAGE_PRESENT|PAGE_WRITABLE|
 *          PAGE_USER flags so ring-3 code can read and execute it
 *       e. copy p_filesz bytes from the file image to p_vaddr
 *   5. kfree the heap buffer.
 *   6. Return e_entry (the program's virtual entry point).
 *
 * Typical call sequence after a successful return:
 *
 *   uint32_t entry = elf_load("HELLO.ELF");
 *   if (entry) {
 *       usermode_initialize();
 *       usermode_enter((void (*)(void))(uintptr_t)entry);
 *   }
 *
 * Returns the entry point virtual address on success.
 * Returns 0 on any failure: file not found, invalid ELF, out-of-memory,
 * or page-mapping error.
 *
 * NOTE: Only compiled in the kernel build (__is_kernel is defined).
 *       elf_validate() and elf_phdr() are available in both builds.
 */
uint32_t elf_load(const char *path);

/*
 * elf_load_into -- load an ELF32 executable into an explicit page directory.
 *
 * Process-isolation version of elf_load().  Maps each PT_LOAD segment into
 * target_pd (not into the currently active global page directory), writing
 * file data directly to the allocated physical pages via their identity-
 * mapped addresses.  This allows the kernel to prepare a child process's
 * address space entirely before the child is first scheduled.
 *
 * Section 9.2 (demand paging) extension:
 *   - Pages that lie entirely within the BSS region of a segment (virtual
 *     address >= page-aligned(p_vaddr + p_filesz)) are NOT allocated here;
 *     they are left unmapped and will be zero-filled by the page-fault
 *     handler when first accessed.
 *   - If vmas is not NULL, a VMA is added for every PT_LOAD segment and for
 *     the user stack, so the fault handler can validate demand-page requests.
 *
 * target_pd -- pointer to the child's page directory (must be in the first
 *             4 MiB, identity-mapped, as all pmm_alloc_page() results are).
 * vmas      -- if non-NULL, pointer to a PROC_VMA_MAX-entry VMA array that
 *             will be populated with one VMA per PT_LOAD segment plus one for
 *             the stack.  Pass NULL to skip VMA registration (ring-0 exec).
 *
 * Returns the entry point virtual address on success, or 0 on failure.
 *
 * Only compiled in the kernel build (__is_kernel defined).
 */
uint32_t elf_load_into(const char *path, uint32_t *target_pd, vma_t *vmas);

#endif /* _KERNEL_ELF_H */
