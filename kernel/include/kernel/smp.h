#ifndef _KERNEL_SMP_H
#define _KERNEL_SMP_H

#include <stdint.h>

/* ── Configuration ──────────────────────────────────────────────────────── */
#define SMP_MAX_CPUS        8     /* maximum CPUs we track                  */

/* Physical address where the 16-bit AP trampoline is copied.
 * SIPI vector = TRAMPOLINE_PHYS >> 12 (must be < 0x100000 and page-aligned).*/
#define TRAMPOLINE_PHYS     0x8000u

/* Offsets within the trampoline page for BSP→AP communication.
 * Must match the constants embedded in smp_trampoline.S.                   */
#define TRAMPOLINE_CR3      0x8F00u  /* BSP writes kernel CR3 here          */
#define TRAMPOLINE_ENTRYC   0x8F04u  /* BSP writes ap_entry_c() VA here     */
#define TRAMPOLINE_STACK    0x8F08u  /* BSP writes per-AP stack top VA here  */

/* ── Per-CPU descriptor ─────────────────────────────────────────────────── */
typedef struct {
    uint8_t  apic_id;  /* Local APIC hardware ID of this CPU               */
    uint8_t  is_bsp;   /* 1 = Bootstrap Processor (first CPU to run GRUB)  */
    uint8_t  active;   /* 1 = BSP sent SIPI to this AP                     */
    uint8_t  online;   /* 1 = AP called ap_entry_c() and checked in        */
} cpu_info_t;

/* ── Intel MP floating pointer structure ────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  signature[4]; /* "_MP_"                                        */
    uint32_t phys_addr;    /* physical address of MP configuration table    */
    uint8_t  length;       /* structure length in 16-byte paragraphs        */
    uint8_t  spec_rev;     /* MP spec revision (1 or 4)                     */
    uint8_t  checksum;     /* all bytes sum to 0                            */
    uint8_t  feature1;     /* 0 = MP config table present                   */
    uint8_t  feature2_5[4];
} mp_float_t;

/* ── Intel MP configuration table header ───────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  signature[4]; /* "PCMP"                                        */
    uint16_t length;       /* total table length in bytes                   */
    uint8_t  spec_rev;
    uint8_t  checksum;
    uint8_t  oem_id[8];
    uint8_t  product_id[12];
    uint32_t oem_table_ptr;
    uint16_t oem_table_size;
    uint16_t entry_count;
    uint32_t lapic_addr;   /* Local APIC physical address (usually 0xFEE00000) */
    uint16_t ext_length;
    uint8_t  ext_checksum;
    uint8_t  reserved;
} mp_config_t;

/* ── MP processor entry (entry type 0) ─────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  type;         /* 0 = processor entry                           */
    uint8_t  apic_id;      /* Local APIC ID                                 */
    uint8_t  apic_ver;     /* Local APIC version register                   */
    uint8_t  flags;        /* bit 0 = enabled; bit 1 = BSP                  */
    uint32_t signature;    /* CPU family/model/stepping signature            */
    uint32_t feature_flags;
    uint32_t reserved[2];
} mp_proc_entry_t;

#define MP_PROC_ENABLED  0x01u
#define MP_PROC_BSP      0x02u

/* ── Global SMP state ──────────────────────────────────────────────────── */
extern cpu_info_t        smp_cpus[SMP_MAX_CPUS];
extern volatile uint32_t smp_cpu_count;   /* CPUs discovered via MP table   */
extern volatile uint32_t smp_cpus_online; /* APs that called ap_entry_c()   */

/* ── Pure-C helpers (testable on host) ─────────────────────────────────── */

/* Compute the byte checksum of a memory region (must equal 0 for valid MP
 * structures).  Pure C, no hardware access.                               */
static inline uint8_t mp_checksum(const uint8_t *buf, uint32_t len)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum += buf[i];
    return sum;
}

/* Return the index of the BSP entry in cpus[], or -1 if not found. */
static inline int smp_find_bsp_idx(const cpu_info_t *cpus, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (cpus[i].is_bsp)
            return (int)i;
    return -1;
}

/* Return the number of APs (non-BSP, active CPUs). */
static inline uint32_t smp_ap_count(const cpu_info_t *cpus, uint32_t n)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!cpus[i].is_bsp && cpus[i].active)
            c++;
    return c;
}

/* Return the number of CPUs that have checked in (online flag set). */
static inline uint32_t smp_online_count(const cpu_info_t *cpus, uint32_t n)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++)
        if (cpus[i].online)
            c++;
    return c;
}

/* Parse the MP configuration table and populate cpus[0..max-1].
 * Returns the number of CPUs found.
 * Pure C: both kernel and host tests can call this with a synthetic table.*/
uint32_t mp_parse_config(const mp_config_t *cfg,
                         cpu_info_t *cpus, uint32_t max);

#ifdef __is_kernel

/* ── Kernel-only SMP API ───────────────────────────────────────────────── */

/* Scan known physical addresses for the MP floating pointer structure.
 * Populates smp_cpus[] and smp_cpu_count.  Call once during kernel init. */
void smp_initialize(void);

/* Send INIT+SIPI to every non-BSP CPU discovered by smp_initialize().
 * Waits for each AP to call ap_entry_c() before sending the next SIPI. */
void smp_boot_aps(void);

/* Return the LAPIC ID of the calling CPU (reads APIC MMIO). */
uint8_t smp_this_cpu_id(void);

/* Entry point for each Application Processor after the trampoline boots it
 * into 32-bit protected mode with paging enabled.  Never returns.        */
void ap_entry_c(void) __attribute__((noreturn));

#endif /* __is_kernel */

#endif /* _KERNEL_SMP_H */
