/* test_smp.c — Host-side unit tests for section 10.5 SMP
 *
 * Tests that run on the host (no __is_kernel, no hardware):
 *   1. Spinlock:          trylock, release, is_locked
 *   2. APIC helpers:      ICR encoding, ID extraction, SIPI vector
 *   3. SMP inline helpers: find_bsp_idx, ap_count, online_count
 *   4. mp_checksum:       byte sum over ranges
 *   5. mp_parse_config:   synthetic MP table with 1–4 processors
 *   6. TRAMPOLINE constants: page-aligned, < 1 MB, comm slots
 */

#include "framework.h"
#include <stdint.h>
#include <string.h>

/* Pull in the headers under test (host build, no __is_kernel). */
#include "../kernel/include/kernel/spinlock.h"
#include "../kernel/include/kernel/apic.h"
#include "../kernel/include/kernel/smp.h"

/* Include smp.c directly to get mp_parse_config() and the global definitions
 * (smp_cpus, smp_cpu_count, smp_cpus_online).  The #ifdef __is_kernel guard
 * in smp.c excludes all kernel-only functions (apic_send_ipi, pit_sleep_ticks,
 * etc.) from the host build, leaving only pure-C code.                    */
#include "../kernel/arch/i386/smp.c"

/* ── Synthetic MP table builder ─────────────────────────────────────────── */

static mp_config_t *make_mp_table(uint8_t *buf, uint32_t bufsz,
                                  uint8_t n_procs)
{
    memset(buf, 0, bufsz);
    mp_config_t *cfg = (mp_config_t *)buf;
    cfg->signature[0] = 'P'; cfg->signature[1] = 'C';
    cfg->signature[2] = 'M'; cfg->signature[3] = 'P';
    cfg->lapic_addr   = 0xFEE00000u;
    cfg->length       = (uint16_t)(sizeof(mp_config_t) + n_procs * 20u);

    uint8_t *p = (uint8_t *)(cfg + 1);
    for (uint8_t i = 0; i < n_procs; i++) {
        mp_proc_entry_t *pe = (mp_proc_entry_t *)p;
        pe->type     = 0;  /* processor entry */
        pe->apic_id  = i;
        pe->flags    = (uint8_t)(MP_PROC_ENABLED | (i == 0 ? MP_PROC_BSP : 0));
        p += 20;
    }

    /* Fix checksum: all bytes of the structure must sum to 0. */
    uint8_t sum = 0;
    for (uint16_t j = 0; j < cfg->length; j++) sum += buf[j];
    cfg->checksum = (uint8_t)(0u - sum);
    return cfg;
}

/* ── Suite 1: Spinlock ───────────────────────────────────────────────────── */

static void test_spinlock(void)
{
    {
        spinlock_t lk = SPINLOCK_INIT;
        ASSERT_EQ(0, spinlock_is_locked(&lk), "init: unlocked");
    }
    {
        spinlock_t lk = SPINLOCK_INIT;
        ASSERT_EQ(1, spinlock_trylock(&lk), "trylock on free lock succeeds");
        ASSERT_EQ(1, spinlock_is_locked(&lk), "after trylock: is_locked");
    }
    {
        spinlock_t lk = SPINLOCK_INIT;
        spinlock_trylock(&lk);
        ASSERT_EQ(0, spinlock_trylock(&lk), "second trylock fails (already held)");
    }
    {
        spinlock_t lk = SPINLOCK_INIT;
        spinlock_trylock(&lk);
        spinlock_release(&lk);
        ASSERT_EQ(0, spinlock_is_locked(&lk), "after release: unlocked");
    }
    {
        spinlock_t lk = SPINLOCK_INIT;
        spinlock_acquire(&lk);
        ASSERT_EQ(1, spinlock_is_locked(&lk), "acquire: is_locked");
        spinlock_release(&lk);
        ASSERT_EQ(0, spinlock_is_locked(&lk), "after release: unlocked");
    }
    {
        spinlock_t lk = SPINLOCK_INIT;
        spinlock_trylock(&lk);
        spinlock_release(&lk);
        ASSERT_EQ(1, spinlock_trylock(&lk), "reacquire after release");
    }
}

/* ── Suite 2: APIC helpers ───────────────────────────────────────────────── */

static void test_apic_helpers(void)
{
    {
        uint32_t icr = apic_icr_init_assert();
        ASSERT_EQ(1, (icr & APIC_ICR_INIT)   != 0, "INIT assert: INIT bit set");
        ASSERT_EQ(1, (icr & APIC_ICR_LEVEL)  != 0, "INIT assert: LEVEL bit set");
        ASSERT_EQ(1, (icr & APIC_ICR_ASSERT) != 0, "INIT assert: ASSERT bit set");
        /* Delivery mode field [10:8]: INIT=0x500, SIPI=0x600 — check it is INIT. */
        ASSERT_EQ((int)APIC_ICR_INIT, (int)(icr & 0x700u), "INIT assert: delivery mode=INIT");
    }
    {
        uint32_t icr = apic_icr_init_deassert();
        ASSERT_EQ(1, (icr & APIC_ICR_INIT)   != 0, "INIT deassert: INIT bit set");
        ASSERT_EQ(1, (icr & APIC_ICR_LEVEL)  != 0, "INIT deassert: LEVEL bit set");
        ASSERT_EQ(0, (icr & APIC_ICR_ASSERT) != 0, "INIT deassert: no ASSERT");
    }
    {
        uint32_t icr = apic_icr_sipi(0x8000u);
        ASSERT_EQ(1, (icr & APIC_ICR_SIPI) != 0, "SIPI: SIPI delivery mode");
        ASSERT_EQ(8, (int)(icr & 0xFFu), "SIPI at 0x8000: vector=8");
    }
    {
        uint32_t icr = apic_icr_sipi(0x9000u);
        ASSERT_EQ(9, (int)(icr & 0xFFu), "SIPI at 0x9000: vector=9");
    }
    {
        uint32_t hi = apic_icr_hi_dest(3u);
        ASSERT_EQ(3u << 24, (int)hi, "ICR_HI dest=3 at bits [31:24]");
    }
    {
        ASSERT_EQ(1, apic_icr_is_pending(APIC_ICR_PENDING), "pending bit set");
        ASSERT_EQ(0, apic_icr_is_pending(0u), "no pending bit");
    }
    {
        uint32_t reg = 0x02000000u;
        ASSERT_EQ(2, (int)apic_id_from_reg(reg), "apic_id_from_reg: bits[31:24]");
        ASSERT_EQ(0, (int)apic_id_from_reg(0u),  "apic_id_from_reg: zero");
    }
    {
        /* BSP LAPIC is always APIC ID 0 in a fresh system; ID must be < 16
         * for any reasonable test-machine emulation.                       */
        uint32_t reg_id5 = 0x05000000u;
        ASSERT_EQ(5, (int)apic_id_from_reg(reg_id5), "apic_id_from_reg(5)");
    }
}

/* ── Suite 3: SMP inline helpers ─────────────────────────────────────────── */

static void test_smp_helpers(void)
{
    /* smp_find_bsp_idx */
    {
        cpu_info_t cpus[3] = {
            {0, 0, 0, 0},
            {1, 1, 0, 0},
            {2, 0, 0, 0},
        };
        ASSERT_EQ(1, smp_find_bsp_idx(cpus, 3), "BSP at index 1");
    }
    {
        cpu_info_t cpus[2] = {{0, 1, 1, 1}, {1, 0, 0, 0}};
        ASSERT_EQ(0, smp_find_bsp_idx(cpus, 2), "BSP at index 0");
    }
    {
        cpu_info_t cpus[2] = {{0, 0, 0, 0}, {1, 0, 0, 0}};
        ASSERT_EQ(-1, smp_find_bsp_idx(cpus, 2), "no BSP found returns -1");
    }
    {
        ASSERT_EQ(-1, smp_find_bsp_idx(NULL, 0), "empty array returns -1");
    }

    /* smp_ap_count */
    {
        cpu_info_t cpus[1] = {{0, 1, 1, 1}};
        ASSERT_EQ(0, (int)smp_ap_count(cpus, 1), "no APs: only BSP");
    }
    {
        cpu_info_t cpus[2] = {{0, 1, 1, 1}, {1, 0, 1, 1}};
        ASSERT_EQ(1, (int)smp_ap_count(cpus, 2), "one active AP");
    }
    {
        cpu_info_t cpus[2] = {{0, 1, 1, 1}, {1, 0, 0, 0}};
        ASSERT_EQ(0, (int)smp_ap_count(cpus, 2), "AP not active: not counted");
    }
    {
        cpu_info_t cpus[4] = {
            {0, 1, 1, 1}, {1, 0, 1, 1}, {2, 0, 1, 1}, {3, 0, 1, 1}
        };
        ASSERT_EQ(3, (int)smp_ap_count(cpus, 4), "three active APs");
    }

    /* smp_online_count */
    {
        cpu_info_t cpus[2] = {{0, 1, 1, 0}, {1, 0, 1, 0}};
        ASSERT_EQ(0, (int)smp_online_count(cpus, 2), "none online");
    }
    {
        cpu_info_t cpus[2] = {{0, 1, 1, 1}, {1, 0, 1, 1}};
        ASSERT_EQ(2, (int)smp_online_count(cpus, 2), "both online");
    }
    {
        cpu_info_t cpus[3] = {{0, 1, 1, 1}, {1, 0, 1, 0}, {2, 0, 1, 1}};
        ASSERT_EQ(2, (int)smp_online_count(cpus, 3), "2 of 3 online");
    }
}

/* ── Suite 4: mp_checksum ────────────────────────────────────────────────── */

static void test_mp_checksum(void)
{
    {
        uint8_t buf[4] = {0, 0, 0, 0};
        ASSERT_EQ(0, (int)mp_checksum(buf, 4), "all-zero sum is 0");
    }
    {
        uint8_t buf[1] = {0xFF};
        ASSERT_EQ(0xFF, (int)mp_checksum(buf, 1), "single byte 0xFF");
    }
    {
        uint8_t buf[2] = {0x80, 0x80};
        ASSERT_EQ(0, (int)mp_checksum(buf, 2), "0x80+0x80 wraps to 0");
    }
    {
        uint8_t buf[3] = {0x01, 0x02, 0x03};
        ASSERT_EQ(6, (int)mp_checksum(buf, 3), "1+2+3 = 6");
    }
    {
        uint8_t buf[4] = {0xAA, 0x55, 0xAA, 0x55};
        ASSERT_EQ(0xFE & 0xFF, (int)(mp_checksum(buf, 4) & 0xFF),
                  "alternating bytes");
    }
}

/* ── Suite 5: mp_parse_config ────────────────────────────────────────────── */

static void test_mp_parse(void)
{
    /* 1 CPU (uniprocessor) */
    {
        uint8_t buf[256];
        mp_config_t *cfg = make_mp_table(buf, sizeof(buf), 1);
        cpu_info_t cpus[8];
        memset(cpus, 0, sizeof(cpus));
        uint32_t n = mp_parse_config(cfg, cpus, 8);
        ASSERT_EQ(1, (int)n, "1-CPU: count=1");
        ASSERT_EQ(0, (int)cpus[0].apic_id, "1-CPU: APIC ID=0");
        ASSERT_EQ(1, (int)cpus[0].is_bsp,  "1-CPU: is BSP");
        ASSERT_EQ(0, (int)cpus[0].active,  "1-CPU: not yet active");
        ASSERT_EQ(0, (int)cpus[0].online,  "1-CPU: not yet online");
    }

    /* 2 CPUs (BSP + 1 AP) */
    {
        uint8_t buf[256];
        mp_config_t *cfg = make_mp_table(buf, sizeof(buf), 2);
        cpu_info_t cpus[8];
        memset(cpus, 0, sizeof(cpus));
        uint32_t n = mp_parse_config(cfg, cpus, 8);
        ASSERT_EQ(2, (int)n, "2-CPU: count=2");
        ASSERT_EQ(0, (int)cpus[0].apic_id, "2-CPU: CPU0 APIC ID=0");
        ASSERT_EQ(1, (int)cpus[0].is_bsp,  "2-CPU: CPU0 is BSP");
        ASSERT_EQ(1, (int)cpus[1].apic_id, "2-CPU: CPU1 APIC ID=1");
        ASSERT_EQ(0, (int)cpus[1].is_bsp,  "2-CPU: CPU1 is AP");
    }

    /* 4 CPUs */
    {
        uint8_t buf[512];
        mp_config_t *cfg = make_mp_table(buf, sizeof(buf), 4);
        cpu_info_t cpus[8];
        memset(cpus, 0, sizeof(cpus));
        uint32_t n = mp_parse_config(cfg, cpus, 8);
        ASSERT_EQ(4, (int)n, "4-CPU: count=4");
        for (int i = 0; i < 4; i++)
            ASSERT_EQ(i, (int)cpus[i].apic_id, "4-CPU: APIC IDs sequential");
        ASSERT_EQ(1, (int)cpus[0].is_bsp, "4-CPU: CPU0 is BSP");
        ASSERT_EQ(0, (int)cpus[3].is_bsp, "4-CPU: CPU3 is AP");
    }

    /* max parameter limits output */
    {
        uint8_t buf[512];
        mp_config_t *cfg = make_mp_table(buf, sizeof(buf), 4);
        cpu_info_t cpus[2];
        memset(cpus, 0, sizeof(cpus));
        uint32_t n = mp_parse_config(cfg, cpus, 2);
        ASSERT_EQ(2, (int)n, "max=2 caps at 2 even with 4 entries");
    }

    /* edge cases */
    {
        cpu_info_t cpus[2];
        ASSERT_EQ(0, (int)mp_parse_config(NULL, cpus, 2), "null cfg → 0");
    }
    {
        uint8_t buf[256];
        mp_config_t *cfg = make_mp_table(buf, sizeof(buf), 1);
        ASSERT_EQ(0, (int)mp_parse_config(cfg, NULL, 1), "null cpus → 0");
    }
    {
        uint8_t buf[256];
        mp_config_t *cfg = make_mp_table(buf, sizeof(buf), 1);
        cpu_info_t cpus[1];
        ASSERT_EQ(0, (int)mp_parse_config(cfg, cpus, 0), "max=0 → 0");
    }

    /* checksum validation: make_mp_table sets checksum so total=0 */
    {
        uint8_t buf[256];
        mp_config_t *cfg = make_mp_table(buf, sizeof(buf), 2);
        uint8_t sum = mp_checksum(buf, cfg->length);
        ASSERT_EQ(0, (int)sum, "synthetic table checksum == 0");
    }
}

/* ── Suite 6: TRAMPOLINE constants ──────────────────────────────────────── */

static void test_trampoline_constants(void)
{
    ASSERT_EQ(0, (int)(TRAMPOLINE_PHYS & 0xFFFu),
              "TRAMPOLINE_PHYS is 4 KiB-aligned");

    ASSERT_EQ(1, (int)(TRAMPOLINE_PHYS < 0x100000u),
              "TRAMPOLINE_PHYS is below 1 MiB");

    /* SIPI vector must fit in 8 bits and be non-zero. */
    uint32_t vec = TRAMPOLINE_PHYS >> 12;
    ASSERT_EQ(1, (int)(vec > 0 && vec <= 0xFFu),
              "SIPI vector field fits in 8 bits");

    /* Communication area must not overlap the first 0x100 bytes of code. */
    ASSERT_EQ(1, (int)(TRAMPOLINE_CR3    >= TRAMPOLINE_PHYS + 0x100u),
              "TRAMPOLINE_CR3 is above code area");
    ASSERT_EQ(1, (int)(TRAMPOLINE_ENTRYC >= TRAMPOLINE_PHYS + 0x100u),
              "TRAMPOLINE_ENTRYC is above code area");
    ASSERT_EQ(1, (int)(TRAMPOLINE_STACK  >= TRAMPOLINE_PHYS + 0x100u),
              "TRAMPOLINE_STACK is above code area");

    /* Three consecutive 4-byte slots. */
    ASSERT_EQ(4, (int)(TRAMPOLINE_ENTRYC - TRAMPOLINE_CR3),
              "ENTRYC is 4 bytes after CR3");
    ASSERT_EQ(4, (int)(TRAMPOLINE_STACK  - TRAMPOLINE_ENTRYC),
              "STACK is 4 bytes after ENTRYC");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    RUN_SUITE(test_spinlock);
    RUN_SUITE(test_apic_helpers);
    RUN_SUITE(test_smp_helpers);
    RUN_SUITE(test_mp_checksum);
    RUN_SUITE(test_mp_parse);
    RUN_SUITE(test_trampoline_constants);

    TEST_SUMMARY();
}
