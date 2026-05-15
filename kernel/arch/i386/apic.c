#include <stdint.h>
#include <stdio.h>

#include <kernel/apic.h>
#include <kernel/paging.h>

/* Pointer to the LAPIC MMIO registers (mapped by apic_initialize). */
static volatile uint32_t *lapic;

static inline uint32_t apic_read(uint32_t reg)
{
    return lapic[reg / 4];
}

static inline void apic_write(uint32_t reg, uint32_t val)
{
    lapic[reg / 4] = val;
    /* Serialising read: ensures the write has propagated to the LAPIC
     * before any subsequent IPI-related reads (e.g. checking ICR pending). */
    (void)lapic[APIC_REG_ID / 4];
}

/* -- LAPIC enable (shared between BSP and AP paths) ----------------------- */
static void lapic_enable(void)
{
    /* Map LAPIC MMIO if not already mapped (first call from BSP does this). */
    if (!lapic) {
        paging_map_mmio(APIC_BASE_VIRT, APIC_BASE_PHYS);
        lapic = (volatile uint32_t *)APIC_BASE_VIRT;
    }

    /* Set the Spurious Interrupt Vector Register:
     *   bit  8 = software-enable the APIC
     *   bits 0-7 = IDT vector for spurious interrupts (0xFF = entry 255)
     * IDT[255] is left as a null gate; spurious interrupts are ignored.    */
    uint32_t svr = apic_read(APIC_REG_SVR);
    svr |= APIC_SVR_ENABLE | APIC_SPURIOUS_VEC;
    apic_write(APIC_REG_SVR, svr);
}

/* -- Public API ----------------------------------------------------------- */

void apic_initialize(void)
{
    /* Disable the legacy PIC so it cannot inject stray interrupts while the
     * APIC is taking over.  In this kernel the PIT/scheduler still uses the
     * original PIC interrupt path (IRQ0 -> IDT[32]); disabling the PIC here
     * means the PIT will no longer fire after this call.  For the SMP demo
     * we keep the PIC enabled and only use the APIC for IPIs.             */

    /* Map LAPIC MMIO and enable the LAPIC on the BSP. */
    lapic_enable();

    printf("[APIC] BSP LAPIC enabled (ID=%d)\r\n", (int)apic_id());
}

void apic_initialize_ap(void)
{
    /* APs reuse the same lapic pointer (BSP mapped it before sending SIPI). */
    if (!lapic) {
        paging_map_mmio(APIC_BASE_VIRT, APIC_BASE_PHYS);
        lapic = (volatile uint32_t *)APIC_BASE_VIRT;
    }
    lapic_enable();
}

void apic_eoi(void)
{
    apic_write(APIC_REG_EOI, 0);
}

uint8_t apic_id(void)
{
    return apic_id_from_reg(apic_read(APIC_REG_ID));
}

void apic_send_ipi(uint8_t dest_id, uint32_t icr_lo)
{
    /* Write destination first, then command (writing ICR_LO triggers send). */
    apic_write(APIC_REG_ICR_HI, apic_icr_hi_dest(dest_id));
    apic_write(APIC_REG_ICR_LO, icr_lo);

    /* Spin until the delivery-status bit clears (IPI has left the LAPIC). */
    while (apic_icr_is_pending(apic_read(APIC_REG_ICR_LO)))
        asm volatile("pause");
}
