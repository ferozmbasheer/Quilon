/*
 * Quilon OS -- ELF Loader Unit Tests
 *
 * Tests elf_validate() and elf_phdr() from kernel/kernel/elf.c using
 * hand-crafted ELF32 buffers.  No kernel services (VFS, PMM, paging) are
 * needed because both functions are pure parsing helpers.
 *
 * elf_load() is guarded by #ifdef __is_kernel and is not tested here; it
 * requires real hardware (MMU, PMM) and is exercised by running the kernel
 * and using the shell `exec` command.
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/elf.h>

/* -- Minimal valid ELF32 i386 executable -------------------------------------
 *
 * Hand-crafted byte array representing the smallest possible ELF32 executable
 * accepted by elf_validate():
 *
 *   Offset   0: ELF header (52 bytes)
 *   Offset  52: one PT_LOAD program header (32 bytes)
 *   Offset  84: four bytes of "code" (NOP NOP NOP RET = 0x90 0x90 0x90 0xC3)
 *
 * Memory layout requested by the program header:
 *   p_vaddr  = 0x00001000  (virtual address to load at)
 *   p_paddr  = 0x00001000
 *   p_offset = 84          (code starts at file offset 84)
 *   p_filesz = 4
 *   p_memsz  = 4096        (one full page; 4092 zero-filled BSS bytes)
 *   p_flags  = PF_R | PF_X
 *   p_align  = 4096
 *
 * Entry point: 0x00001000
 * --------------------------------------------------------------------------- */

/* little-endian helpers */
#define U16LE(v)  (uint8_t)((v) & 0xFF), (uint8_t)(((v) >> 8) & 0xFF)
#define U32LE(v)  (uint8_t)((v) & 0xFF), (uint8_t)(((v) >> 8) & 0xFF), \
                  (uint8_t)(((v) >> 16) & 0xFF), (uint8_t)(((v) >> 24) & 0xFF)

static const uint8_t valid_elf[] = {
    /* -- e_ident (16 bytes) ------------------------------------------------ */
    0x7F, 'E', 'L', 'F',   /* magic                                         */
    0x01,                   /* EI_CLASS    = ELFCLASS32                      */
    0x01,                   /* EI_DATA     = ELFDATA2LSB (little-endian)     */
    0x01,                   /* EI_VERSION  = EV_CURRENT                      */
    0x00,                   /* EI_OSABI    = ELFOSABI_NONE                   */
    0x00, 0x00, 0x00, 0x00, /* EI_ABIVERSION + 4 bytes padding               */
    0x00, 0x00, 0x00, 0x00, /* 4 bytes padding                               */

    /* -- rest of ELF header (36 bytes) ------------------------------------- */
    U16LE(2),          /* e_type      = ET_EXEC                              */
    U16LE(3),          /* e_machine   = EM_386                               */
    U32LE(1),          /* e_version   = 1                                    */
    U32LE(0x00001000), /* e_entry     = 0x1000                               */
    U32LE(52),         /* e_phoff     = 52 (program header table right after)*/
    U32LE(0),          /* e_shoff     = 0 (no section headers)               */
    U32LE(0),          /* e_flags     = 0                                    */
    U16LE(52),         /* e_ehsize    = 52                                   */
    U16LE(32),         /* e_phentsize = 32 (sizeof elf32_phdr_t)             */
    U16LE(1),          /* e_phnum     = 1                                    */
    U16LE(0),          /* e_shentsize = 0                                    */
    U16LE(0),          /* e_shnum     = 0                                    */
    U16LE(0),          /* e_shstrndx  = 0                                    */

    /* -- Program header #0 (32 bytes, starts at offset 52) ---------------- */
    U32LE(1),          /* p_type   = PT_LOAD                                 */
    U32LE(84),         /* p_offset = 84 (code starts at offset 84)          */
    U32LE(0x00001000), /* p_vaddr  = 0x1000                                  */
    U32LE(0x00001000), /* p_paddr  = 0x1000                                  */
    U32LE(4),          /* p_filesz = 4 bytes                                 */
    U32LE(4096),       /* p_memsz  = 4096 bytes (one page; rest = BSS)      */
    U32LE(PF_R|PF_X),  /* p_flags  = readable + executable                  */
    U32LE(4096),       /* p_align  = 4096 (one page)                        */

    /* -- Code bytes (4 bytes, starts at offset 84) ------------------------- */
    0x90, 0x90, 0x90, 0xC3   /* NOP NOP NOP RET                             */
};

#define VALID_ELF_SIZE  ((uint32_t)sizeof(valid_elf))

/* ===========================================================================
 * 1. elf_validate() -- good inputs
 * =========================================================================== */

static void test_validate_good_elf(void)
{
    int r = elf_validate(valid_elf, VALID_ELF_SIZE);
    ASSERT_EQ(r, 0, "valid ELF32 i386 exec passes validation");
}

static void test_validate_exact_header_size(void)
{
    /* Buffer exactly the size of the ELF header alone (no program headers).
     * elf_validate checks that the program header table fits in the buffer,
     * so a 52-byte buffer with e_phnum=1 should FAIL (table doesn't fit).
     * A buffer containing the full header + one program header (84 bytes)
     * but no segment data should PASS.                                      */
    int r = elf_validate(valid_elf, 52u + 32u); /* header + one phdr */
    ASSERT_EQ(r, 0, "buffer with just header+phdrs passes (no segment data needed)");
}

/* ===========================================================================
 * 2. elf_validate() -- NULL / size failures
 * =========================================================================== */

static void test_validate_null_buf(void)
{
    int r = elf_validate(NULL, 128);
    ASSERT_EQ(r, -1, "NULL buffer returns -1");
}

static void test_validate_zero_size(void)
{
    int r = elf_validate(valid_elf, 0);
    ASSERT_EQ(r, -1, "zero size returns -1");
}

static void test_validate_too_small(void)
{
    /* One byte short of a full ELF header */
    int r = elf_validate(valid_elf, sizeof(elf32_ehdr_t) - 1u);
    ASSERT_EQ(r, -1, "buffer shorter than ELF header returns -1");
}

static void test_validate_phdr_table_truncated(void)
{
    /* Header fits but the program header table is cut short.
     * e_phnum=1 requires offset 52 + 32 = 84 bytes; give only 60.         */
    int r = elf_validate(valid_elf, 60u);
    ASSERT_EQ(r, -1, "truncated program header table returns -1");
}

/* ===========================================================================
 * 3. elf_validate() -- bad magic
 * =========================================================================== */

static void test_validate_bad_magic_byte0(void)
{
    uint8_t buf[VALID_ELF_SIZE];
    memcpy(buf, valid_elf, VALID_ELF_SIZE);
    buf[0] = 0x00;   /* corrupt first magic byte */
    ASSERT_EQ(elf_validate(buf, VALID_ELF_SIZE), -1, "bad magic[0] returns -1");
}

static void test_validate_bad_magic_byte1(void)
{
    uint8_t buf[VALID_ELF_SIZE];
    memcpy(buf, valid_elf, VALID_ELF_SIZE);
    buf[1] = 'X';    /* 'E' -> 'X' */
    ASSERT_EQ(elf_validate(buf, VALID_ELF_SIZE), -1, "bad magic[1] returns -1");
}

static void test_validate_bad_magic_byte2(void)
{
    uint8_t buf[VALID_ELF_SIZE];
    memcpy(buf, valid_elf, VALID_ELF_SIZE);
    buf[2] = 'X';    /* 'L' -> 'X' */
    ASSERT_EQ(elf_validate(buf, VALID_ELF_SIZE), -1, "bad magic[2] returns -1");
}

static void test_validate_bad_magic_byte3(void)
{
    uint8_t buf[VALID_ELF_SIZE];
    memcpy(buf, valid_elf, VALID_ELF_SIZE);
    buf[3] = 'X';    /* 'F' -> 'X' */
    ASSERT_EQ(elf_validate(buf, VALID_ELF_SIZE), -1, "bad magic[3] returns -1");
}

/* ===========================================================================
 * 4. elf_validate() -- bad class / data / version
 * =========================================================================== */

static void test_validate_64bit_class(void)
{
    uint8_t buf[VALID_ELF_SIZE];
    memcpy(buf, valid_elf, VALID_ELF_SIZE);
    buf[4] = 2;  /* ELFCLASS64 */
    ASSERT_EQ(elf_validate(buf, VALID_ELF_SIZE), -1, "ELF64 class rejected");
}

static void test_validate_big_endian_data(void)
{
    uint8_t buf[VALID_ELF_SIZE];
    memcpy(buf, valid_elf, VALID_ELF_SIZE);
    buf[5] = 2;  /* ELFDATA2MSB -- big-endian */
    ASSERT_EQ(elf_validate(buf, VALID_ELF_SIZE), -1, "big-endian ELF rejected");
}

static void test_validate_bad_version(void)
{
    uint8_t buf[VALID_ELF_SIZE];
    memcpy(buf, valid_elf, VALID_ELF_SIZE);
    buf[6] = 0;  /* version 0 is invalid */
    ASSERT_EQ(elf_validate(buf, VALID_ELF_SIZE), -1, "ELF version 0 rejected");
}

/* ===========================================================================
 * 5. elf_validate() -- bad type / machine
 * =========================================================================== */

static void test_validate_shared_lib_type(void)
{
    uint8_t buf[VALID_ELF_SIZE];
    memcpy(buf, valid_elf, VALID_ELF_SIZE);
    /* ET_DYN = 3 (shared object) -- Quilon only loads executables (ET_EXEC=2) */
    buf[16] = 3; buf[17] = 0;
    ASSERT_EQ(elf_validate(buf, VALID_ELF_SIZE), -1, "ET_DYN (shared lib) rejected");
}

static void test_validate_relocatable_type(void)
{
    uint8_t buf[VALID_ELF_SIZE];
    memcpy(buf, valid_elf, VALID_ELF_SIZE);
    /* ET_REL = 1 (relocatable object file) */
    buf[16] = 1; buf[17] = 0;
    ASSERT_EQ(elf_validate(buf, VALID_ELF_SIZE), -1, "ET_REL (object file) rejected");
}

static void test_validate_wrong_machine_arm(void)
{
    uint8_t buf[VALID_ELF_SIZE];
    memcpy(buf, valid_elf, VALID_ELF_SIZE);
    /* EM_ARM = 40 */
    buf[18] = 40; buf[19] = 0;
    ASSERT_EQ(elf_validate(buf, VALID_ELF_SIZE), -1, "ARM machine code rejected");
}

static void test_validate_wrong_machine_x64(void)
{
    uint8_t buf[VALID_ELF_SIZE];
    memcpy(buf, valid_elf, VALID_ELF_SIZE);
    /* EM_X86_64 = 62 */
    buf[18] = 62; buf[19] = 0;
    ASSERT_EQ(elf_validate(buf, VALID_ELF_SIZE), -1, "x86-64 machine code rejected");
}

/* ===========================================================================
 * 6. elf_validate() -- bad program header entry size
 * =========================================================================== */

static void test_validate_bad_phentsize(void)
{
    uint8_t buf[VALID_ELF_SIZE];
    memcpy(buf, valid_elf, VALID_ELF_SIZE);
    /* e_phentsize is at offset 42 (little-endian 16-bit).
     * Set it to 64 (ELF64 phdr size) -- should be rejected.                */
    buf[42] = 64; buf[43] = 0;
    ASSERT_EQ(elf_validate(buf, VALID_ELF_SIZE), -1,
              "wrong e_phentsize (64 instead of 32) rejected");
}

static void test_validate_zero_phentsize(void)
{
    uint8_t buf[VALID_ELF_SIZE];
    memcpy(buf, valid_elf, VALID_ELF_SIZE);
    buf[42] = 0; buf[43] = 0;
    ASSERT_EQ(elf_validate(buf, VALID_ELF_SIZE), -1,
              "zero e_phentsize rejected");
}

/* ===========================================================================
 * 7. elf_validate() -- zero program header count is allowed
 * =========================================================================== */

static void test_validate_zero_phnum(void)
{
    /* An ELF with e_phnum=0 has no program headers to load, so the program
     * header table trivially "fits" in the buffer.  elf_validate() accepts
     * this -- elf_load() would simply skip the load loop and return e_entry.*/
    uint8_t buf[VALID_ELF_SIZE];
    memcpy(buf, valid_elf, VALID_ELF_SIZE);
    /* e_phnum is at offset 44 (little-endian 16-bit) */
    buf[44] = 0; buf[45] = 0;
    ASSERT_EQ(elf_validate(buf, VALID_ELF_SIZE), 0,
              "e_phnum=0 passes validation (no segments to check)");
}

/* ===========================================================================
 * 8. elf_phdr() -- program header access
 * =========================================================================== */

static void test_phdr_type_is_load(void)
{
    const elf32_phdr_t *ph = elf_phdr(valid_elf, 0);
    ASSERT_EQ((int)ph->p_type, (int)PT_LOAD, "phdr[0].p_type == PT_LOAD");
}

static void test_phdr_vaddr(void)
{
    const elf32_phdr_t *ph = elf_phdr(valid_elf, 0);
    ASSERT_EQ(ph->p_vaddr, 0x00001000u, "phdr[0].p_vaddr == 0x1000");
}

static void test_phdr_filesz(void)
{
    const elf32_phdr_t *ph = elf_phdr(valid_elf, 0);
    ASSERT_EQ(ph->p_filesz, 4u, "phdr[0].p_filesz == 4");
}

static void test_phdr_memsz(void)
{
    const elf32_phdr_t *ph = elf_phdr(valid_elf, 0);
    ASSERT_EQ(ph->p_memsz, 4096u, "phdr[0].p_memsz == 4096 (one page)");
}

static void test_phdr_offset(void)
{
    const elf32_phdr_t *ph = elf_phdr(valid_elf, 0);
    ASSERT_EQ(ph->p_offset, 84u, "phdr[0].p_offset == 84");
}

static void test_phdr_flags(void)
{
    const elf32_phdr_t *ph = elf_phdr(valid_elf, 0);
    ASSERT_EQ(ph->p_flags, PF_R | PF_X, "phdr[0].p_flags == PF_R|PF_X");
}

static void test_phdr_align(void)
{
    const elf32_phdr_t *ph = elf_phdr(valid_elf, 0);
    ASSERT_EQ(ph->p_align, 4096u, "phdr[0].p_align == 4096");
}

/* ===========================================================================
 * 9. ELF header field inspection
 * =========================================================================== */

static void test_ehdr_entry_point(void)
{
    const elf32_ehdr_t *h = (const elf32_ehdr_t *)valid_elf;
    ASSERT_EQ(h->e_entry, 0x00001000u, "e_entry == 0x1000");
}

static void test_ehdr_phnum(void)
{
    const elf32_ehdr_t *h = (const elf32_ehdr_t *)valid_elf;
    ASSERT_EQ((int)h->e_phnum, 1, "e_phnum == 1");
}

static void test_ehdr_phoff(void)
{
    const elf32_ehdr_t *h = (const elf32_ehdr_t *)valid_elf;
    ASSERT_EQ(h->e_phoff, 52u, "e_phoff == 52 (immediately after header)");
}

static void test_ehdr_phentsize(void)
{
    const elf32_ehdr_t *h = (const elf32_ehdr_t *)valid_elf;
    ASSERT_EQ((int)h->e_phentsize, (int)sizeof(elf32_phdr_t),
              "e_phentsize == sizeof(elf32_phdr_t)");
}

/* ===========================================================================
 * 10. ELF struct size invariants
 * =========================================================================== */

static void test_struct_sizes(void)
{
    /* These sizes are mandated by the ELF32 ABI spec.  If they ever change
     * (e.g. due to padding on an unusual platform), the loader would produce
     * wrong results when indexing into e_phoff.                             */
    ASSERT_EQ((int)sizeof(elf32_ehdr_t), 52, "elf32_ehdr_t is 52 bytes");
    ASSERT_EQ((int)sizeof(elf32_phdr_t), 32, "elf32_phdr_t is 32 bytes");
}

/* ===========================================================================
 * 11. Multiple program headers
 * =========================================================================== */

static void test_two_phdrs(void)
{
    /* Build a synthetic ELF with two program headers:
     *   #0: PT_LOAD  vaddr=0x1000  filesz=4
     *   #1: PT_NOTE  vaddr=0x0000  filesz=0  (non-load entry)
     *
     * Offset  0: ELF header (52 bytes) -- e_phnum=2
     * Offset 52: phdr #0 (32 bytes)
     * Offset 84: phdr #1 (32 bytes)
     * Offset 116: 4 bytes of code
     */
    uint8_t buf[120];
    memset(buf, 0, sizeof(buf));

    /* ELF header */
    buf[0]=0x7F; buf[1]='E'; buf[2]='L'; buf[3]='F';
    buf[4]=1; buf[5]=1; buf[6]=1;          /* class, data, version */
    buf[16]=2; buf[17]=0;                  /* e_type   = ET_EXEC  */
    buf[18]=3; buf[19]=0;                  /* e_machine= EM_386   */
    buf[20]=1; buf[21]=0; buf[22]=0; buf[23]=0;  /* e_version = 1 */
    buf[24]=0x00; buf[25]=0x10; buf[26]=0x00; buf[27]=0x00; /* e_entry=0x1000 */
    buf[28]=52; buf[29]=0; buf[30]=0; buf[31]=0;  /* e_phoff = 52  */
    buf[36]=52; buf[37]=0;               /* e_ehsize    = 52 */
    buf[42]=32; buf[43]=0;               /* e_phentsize = 32 */
    buf[44]=2;  buf[45]=0;               /* e_phnum     = 2  */

    /* phdr #0 -- PT_LOAD */
    uint8_t *ph0 = buf + 52;
    ph0[0]=1; ph0[1]=0; ph0[2]=0; ph0[3]=0;         /* p_type = PT_LOAD */
    ph0[4]=116; ph0[5]=0; ph0[6]=0; ph0[7]=0;        /* p_offset = 116   */
    ph0[8]=0x00; ph0[9]=0x10; ph0[10]=0; ph0[11]=0;  /* p_vaddr  = 0x1000*/
    ph0[12]=ph0[8]; ph0[13]=ph0[9]; ph0[14]=ph0[10]; ph0[15]=ph0[11]; /* paddr */
    ph0[16]=4; ph0[17]=0; ph0[18]=0; ph0[19]=0;      /* p_filesz = 4     */
    ph0[20]=4; ph0[21]=0; ph0[22]=0; ph0[23]=0;      /* p_memsz  = 4     */
    ph0[24]=PF_R|PF_X;                               /* p_flags          */
    ph0[28]=0x10; ph0[29]=0;                         /* p_align  = 16    */

    /* phdr #1 -- PT_NOTE */
    uint8_t *ph1 = buf + 84;
    ph1[0]=4; ph1[1]=0; ph1[2]=0; ph1[3]=0;          /* p_type = PT_NOTE */

    /* code bytes at offset 116 */
    buf[116]=0x90; buf[117]=0x90; buf[118]=0x90; buf[119]=0xC3;

    int r = elf_validate(buf, sizeof(buf));
    ASSERT_EQ(r, 0, "two-phdr ELF passes validation");

    const elf32_phdr_t *p0 = elf_phdr(buf, 0);
    const elf32_phdr_t *p1 = elf_phdr(buf, 1);

    ASSERT_EQ((int)p0->p_type, (int)PT_LOAD, "phdr[0] type == PT_LOAD");
    ASSERT_EQ((int)p1->p_type, (int)PT_NOTE, "phdr[1] type == PT_NOTE");
    ASSERT_EQ(p0->p_vaddr, 0x00001000u,       "phdr[0] vaddr == 0x1000");
    ASSERT_EQ(p0->p_offset, 116u,             "phdr[0] offset == 116");
}

/* ===========================================================================
 * main
 * =========================================================================== */

int main(void)
{
    /* Struct size invariants */
    RUN_SUITE(test_struct_sizes);

    /* ELF header fields */
    RUN_SUITE(test_ehdr_entry_point);
    RUN_SUITE(test_ehdr_phnum);
    RUN_SUITE(test_ehdr_phoff);
    RUN_SUITE(test_ehdr_phentsize);

    /* elf_validate -- good inputs */
    RUN_SUITE(test_validate_good_elf);
    RUN_SUITE(test_validate_exact_header_size);

    /* elf_validate -- NULL / size failures */
    RUN_SUITE(test_validate_null_buf);
    RUN_SUITE(test_validate_zero_size);
    RUN_SUITE(test_validate_too_small);
    RUN_SUITE(test_validate_phdr_table_truncated);

    /* elf_validate -- bad magic */
    RUN_SUITE(test_validate_bad_magic_byte0);
    RUN_SUITE(test_validate_bad_magic_byte1);
    RUN_SUITE(test_validate_bad_magic_byte2);
    RUN_SUITE(test_validate_bad_magic_byte3);

    /* elf_validate -- bad class / data / version */
    RUN_SUITE(test_validate_64bit_class);
    RUN_SUITE(test_validate_big_endian_data);
    RUN_SUITE(test_validate_bad_version);

    /* elf_validate -- bad type / machine */
    RUN_SUITE(test_validate_shared_lib_type);
    RUN_SUITE(test_validate_relocatable_type);
    RUN_SUITE(test_validate_wrong_machine_arm);
    RUN_SUITE(test_validate_wrong_machine_x64);

    /* elf_validate -- bad phentsize */
    RUN_SUITE(test_validate_bad_phentsize);
    RUN_SUITE(test_validate_zero_phentsize);

    /* elf_validate -- edge cases */
    RUN_SUITE(test_validate_zero_phnum);

    /* elf_phdr -- program header field access */
    RUN_SUITE(test_phdr_type_is_load);
    RUN_SUITE(test_phdr_vaddr);
    RUN_SUITE(test_phdr_filesz);
    RUN_SUITE(test_phdr_memsz);
    RUN_SUITE(test_phdr_offset);
    RUN_SUITE(test_phdr_flags);
    RUN_SUITE(test_phdr_align);

    /* Multiple program headers */
    RUN_SUITE(test_two_phdrs);

    TEST_SUMMARY();
}
