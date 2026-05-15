#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <kernel/smp.h>
#include <kernel/apic.h>
#include <kernel/paging.h>
#include <kernel/pmm.h>
#include <kernel/gdt.h>
#include <kernel/interrupts.h>
#include <kernel/pit.h>

/* -- Global SMP state ---------------------------------------------------- */
cpu_info_t        smp_cpus[SMP_MAX_CPUS];
volatile uint32_t smp_cpu_count   = 0;
volatile uint32_t smp_cpus_online = 0;

/* -- mp_parse_config -- pure C, testable on host ------------------------ */
uint32_t mp_parse_config(const mp_config_t *cfg,
                         cpu_info_t *cpus, uint32_t max)
{
    if (!cfg || !cpus || max == 0) return 0;

    uint32_t count = 0;
    const uint8_t *p   = (const uint8_t *)(cfg + 1); /* entries follow header */
    const uint8_t *end = (const uint8_t *)cfg + cfg->length;

    while (p < end && count < max) {
        switch (*p) {
        case 0: { /* Processor entry (20 bytes) */
            const mp_proc_entry_t *proc = (const mp_proc_entry_t *)p;
            if (proc->flags & MP_PROC_ENABLED) {
                cpus[count].apic_id = proc->apic_id;
                cpus[count].is_bsp  = (proc->flags & MP_PROC_BSP) ? 1u : 0u;
                cpus[count].active  = 0;
                cpus[count].online  = 0;
                count++;
            }
            p += 20;
            break;
        }
        case 1: p += 8;  break; /* Bus entry            */
        case 2: p += 8;  break; /* I/O APIC entry       */
        case 3: p += 8;  break; /* I/O interrupt entry  */
        case 4: p += 8;  break; /* Local interrupt entry */
        default:
            /* Unknown entry type -- stop parsing to avoid reading garbage. */
            return count;
        }
    }
    return count;
}

/* -- MP floating pointer search (kernel-only) ----------------------------- */
#ifdef __is_kernel

/* Search a memory range for the "_MP_" signature at 16-byte boundaries.   */
static mp_float_t *mp_search_range(uint32_t base, uint32_t len)
{
    for (uint32_t addr = base; addr < base + len; addr += 16) {
        mp_float_t *f = (mp_float_t *)(uintptr_t)addr;
        if (f->signature[0] == '_' && f->signature[1] == 'M' &&
            f->signature[2] == 'P' && f->signature[3] == '_') {
            /* Verify checksum: all bytes of the structure must sum to 0.   */
            if (mp_checksum((const uint8_t *)f, (uint32_t)f->length * 16) == 0)
                return f;
        }
    }
    return NULL;
}

static mp_float_t *mp_find_float(void)
{
    /* 1. EBDA -- Extended BIOS Data Area (segment address at 0x40E × 16).  */
    uint32_t ebda_seg = (uint32_t)(*(const uint16_t *)(uintptr_t)0x040E);
    uint32_t ebda_base = ebda_seg << 4;
    if (ebda_base) {
        mp_float_t *f = mp_search_range(ebda_base, 1024);
        if (f) return f;
    }

    /* 2. Top of base memory (0x9FC00–0xA0000).                            */
    {
        mp_float_t *f = mp_search_range(0x9FC00u, 0x400u);
        if (f) return f;
    }

    /* 3. BIOS ROM (0xF0000–0xFFFFF).                                      */
    {
        mp_float_t *f = mp_search_range(0xF0000u, 0x10000u);
        if (f) return f;
    }

    return NULL;
}

/* -- smp_initialize ------------------------------------------------------- */
void smp_initialize(void)
{
    memset(smp_cpus, 0, sizeof(smp_cpus));
    smp_cpu_count   = 0;
    smp_cpus_online = 0;

    mp_float_t *fp = mp_find_float();
    if (!fp) {
        printf("[SMP] No MP table found, assuming 1 CPU\r\n");
        smp_cpus[0].apic_id = 0;
        smp_cpus[0].is_bsp  = 1;
        smp_cpus[0].active  = 1;
        smp_cpus[0].online  = 1;
        smp_cpu_count   = 1;
        smp_cpus_online = 1;
        return;
    }

    /* Verify the MP configuration table.                                  */
    if (fp->feature1 != 0 || fp->phys_addr == 0) {
        printf("[SMP] MP default config, assuming 1 CPU\r\n");
        smp_cpus[0].apic_id = 0;
        smp_cpus[0].is_bsp  = 1;
        smp_cpus[0].active  = 1;
        smp_cpus[0].online  = 1;
        smp_cpu_count   = 1;
        smp_cpus_online = 1;
        return;
    }

    const mp_config_t *cfg = (const mp_config_t *)(uintptr_t)fp->phys_addr;
    if (cfg->signature[0] != 'P' || cfg->signature[1] != 'C' ||
        cfg->signature[2] != 'M' || cfg->signature[3] != 'P') {
        printf("[SMP] MP config table signature invalid\r\n");
        smp_cpu_count = 0;
        return;
    }

    uint32_t found = mp_parse_config(cfg, smp_cpus, SMP_MAX_CPUS);
    smp_cpu_count = found;

    /* CPUID leaf 1 EBX[23:16] = max logical CPU count in this package.
     * QEMU -smp N sets this field.  Use as fallback when the MP table only
     * listed the BSP (common with SeaBIOS + -smp 2).                       */
    {
        uint32_t ebx = 0;
        asm volatile("cpuid" : "=b"(ebx) : "a"(1) : "ecx", "edx");
        uint32_t cpuid_count = (ebx >> 16) & 0xFFu;
        if (cpuid_count > 1 && found < cpuid_count &&
            cpuid_count <= SMP_MAX_CPUS) {
            printf("[SMP] MP table: %d CPU(s); CPUID leaf1 EBX[23:16]=%d"
                   " -- adding synthetic APs\r\n",
                   (int)found, (int)cpuid_count);
            for (uint32_t i = found; i < cpuid_count; i++) {
                smp_cpus[i].apic_id = (uint8_t)i;
                smp_cpus[i].is_bsp  = 0;
                smp_cpus[i].active  = 0;
                smp_cpus[i].online  = 0;
            }
            smp_cpu_count = cpuid_count;
        }
    }

    /* The BSP is already running -- mark it online immediately.             */
    int bsp_idx = smp_find_bsp_idx(smp_cpus, smp_cpu_count);
    if (bsp_idx >= 0) {
        smp_cpus[bsp_idx].online = 1;
        smp_cpus_online = 1;
    }

    printf("[SMP] Found %d CPU(s) via MP table\r\n", (int)found);
}

/* -- smp_this_cpu_id ------------------------------------------------------ */
uint8_t smp_this_cpu_id(void)
{
    return apic_id();
}

/* -- ap_entry_c -- called by each AP after trampoline enables paging -------
 *
 * At entry:
 *   - Running at ring 0 in kernel-high virtual address space
 *   - Paging is on (BSP's kernel page directory loaded by trampoline)
 *   - Stack is a freshly-allocated kernel page (stack top at ESP)
 *   - No GDT or IDT loaded yet (we only have the bootstrap GDT from trampoline)
 */
void __attribute__((noreturn)) ap_entry_c(void)
{
    /* Reload the BSP's GDT (which has the proper kernel + TSS descriptors). */
    gdt_initialize_ap();

    /* Reload the IDT so this CPU can handle exceptions and interrupts.     */
    idt_load_ap();

    /* Find our entry in the SMP table by LAPIC ID.                        */
    uint8_t my_id = apic_id();
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        if (smp_cpus[i].apic_id == my_id) {
            smp_cpus[i].online = 1;
            break;
        }
    }
    __sync_fetch_and_add(&smp_cpus_online, 1u);

    printf("[SMP] AP (LAPIC ID=%d) online, entering idle loop\r\n",
           (int)my_id);

    /* Enable interrupts on this AP so it can receive IPIs in the future.  */
    asm volatile("sti");

    /* Idle loop: let the CPU sleep on each interrupt (or simply spin).    */
    for (;;)
        asm volatile("hlt");
}

/* -- smp_boot_aps ---------------------------------------------------------
 *
 * For each AP discovered in smp_cpus[]:
 *   1. Allocate a kernel stack page for the AP.
 *   2. Fill the trampoline communication area (CR3, entry, stack).
 *   3. Copy the trampoline code to physical 0x8000.
 *   4. Send INIT IPI -> wait 10 ms -> send SIPI -> wait for AP to check in.
 */
void smp_boot_aps(void)
{
    extern uint8_t smp_trampoline_start[], smp_trampoline_end[];

    if (smp_cpu_count == 0) return;

    /* Copy trampoline to physical 0x8000 (identity-mapped: VA = PA).     */
    uint32_t tramp_size = (uint32_t)(smp_trampoline_end - smp_trampoline_start);
    memcpy((void *)(uintptr_t)TRAMPOLINE_PHYS, smp_trampoline_start, tramp_size);

    /* Pre-fill the trampoline's static GDT (at offsets 0x80 and 0x88 in
     * the trampoline page = physical 0x8080 and 0x8088).
     * The trampoline assembly already embeds the GDT bytes, so we only
     * need to fill the BSP-specific fields in the communication area.     */

    /* Kernel CR3 -- same page directory for all APs.                       */
    *(volatile uint32_t *)(uintptr_t)TRAMPOLINE_CR3 = paging_kernel_cr3();

    /* ap_entry_c() virtual address.                                       */
    *(volatile uint32_t *)(uintptr_t)TRAMPOLINE_ENTRYC =
        (uint32_t)(uintptr_t)ap_entry_c;

    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        if (smp_cpus[i].is_bsp) continue;  /* skip BSP                    */

        uint8_t dest = smp_cpus[i].apic_id;

        /* Allocate a 4 KiB kernel stack for this AP.                      */
        void *stack_page = pmm_alloc_page();
        if (!stack_page) {
            printf("[SMP] OOM: cannot allocate AP stack\r\n");
            continue;
        }
        /* The allocated page is in the first 4 MiB (identity-mapped).
         * Translate to kernel-high VA for the AP to use as its stack.     */
        uint32_t stack_top_va =
            (uint32_t)(uintptr_t)stack_page + KERNEL_OFFSET + PAGE_SIZE;
        *(volatile uint32_t *)(uintptr_t)TRAMPOLINE_STACK = stack_top_va;

        printf("[SMP] Booting AP LAPIC ID=%d\r\n", (int)dest);

        /* -- INIT IPI -- */
        apic_send_ipi(dest, apic_icr_init_assert());
        pit_sleep_ticks(1);      /* ≈10 ms at 100 Hz                       */
        apic_send_ipi(dest, apic_icr_init_deassert());
        pit_sleep_ticks(1);

        /* -- SIPI (Startup IPI) -- send twice as required by the MP spec -- */
        uint32_t sipi = apic_icr_sipi(TRAMPOLINE_PHYS);
        apic_send_ipi(dest, sipi);
        pit_sleep_ticks(1);
        apic_send_ipi(dest, sipi);

        /* Wait up to ~200 ms for the AP to set its online flag.           */
        uint32_t timeout = 20;
        while (!smp_cpus[i].online && timeout-- > 0)
            pit_sleep_ticks(1);

        if (smp_cpus[i].online) {
            smp_cpus[i].active = 1;
            printf("[SMP] AP online: OK\r\n");
        } else {
            printf("[SMP] AP timed out\r\n");
        }
    }
}

#endif /* __is_kernel */
