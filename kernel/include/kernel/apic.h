#ifndef _KERNEL_APIC_H
#define _KERNEL_APIC_H

#include <stdint.h>

/* -- LAPIC physical address and kernel virtual mapping -------------------- */
#define APIC_BASE_PHYS   0xFEE00000UL   /* standard LAPIC MMIO physical base */
#define APIC_BASE_VIRT   0xFEE00000UL   /* mapped 1:1 in kernel address space */

/* -- LAPIC register offsets (byte offsets, registers are 32-bit) ----------- */
#define APIC_REG_ID        0x020  /* Local APIC ID                         */
#define APIC_REG_VER       0x030  /* Local APIC version                    */
#define APIC_REG_EOI       0x0B0  /* End Of Interrupt (write 0 to signal)  */
#define APIC_REG_SVR       0x0F0  /* Spurious Interrupt Vector Register    */
#define APIC_REG_ICR_LO    0x300  /* Interrupt Command Register -- low 32   */
#define APIC_REG_ICR_HI    0x310  /* Interrupt Command Register -- high 32  */
#define APIC_REG_LVT_TIMER 0x320  /* LVT Timer entry                       */
#define APIC_REG_TMR_INIT  0x380  /* Timer initial count                   */
#define APIC_REG_TMR_CURR  0x390  /* Timer current count                   */
#define APIC_REG_TMR_DIV   0x3E0  /* Timer divide configuration            */

/* -- SVR (Spurious Vector Register) bits ----------------------------------- */
#define APIC_SVR_ENABLE    (1u << 8)   /* APIC software-enable bit          */
#define APIC_SPURIOUS_VEC  0xFF        /* IDT vector for spurious interrupts */

/* -- ICR (Interrupt Command Register) delivery mode bits ------------------- */
#define APIC_ICR_FIXED     0x00000000u /* Fixed delivery (normal interrupt) */
#define APIC_ICR_INIT      0x00000500u /* INIT IPI                          */
#define APIC_ICR_SIPI      0x00000600u /* Startup IPI (SIPI)                */
#define APIC_ICR_ASSERT    0x00004000u /* Level: assert                     */
#define APIC_ICR_DEASSERT  0x00000000u /* Level: de-assert                  */
#define APIC_ICR_LEVEL     0x00008000u /* Trigger mode: level               */
#define APIC_ICR_PENDING   0x00001000u /* Delivery status: send pending     */

/* ICR destination shorthand */
#define APIC_ICR_DEST_FIELD   0x00000000u  /* Use destination field in ICR_HI */

/* -- Pure-C helpers (testable on host) -------------------------------------
 *
 * These functions encode ICR values and compute SIPI vectors.
 * They have no side effects and can run on the host test harness.
 */

/* Return the ICR_LO value for an INIT IPI (assert level). */
static inline uint32_t apic_icr_init_assert(void)
{
    return APIC_ICR_INIT | APIC_ICR_LEVEL | APIC_ICR_ASSERT;
}

/* Return the ICR_LO value for an INIT IPI de-assert (required after assert). */
static inline uint32_t apic_icr_init_deassert(void)
{
    return APIC_ICR_INIT | APIC_ICR_LEVEL | APIC_ICR_DEASSERT;
}

/* Return the ICR_LO value for a SIPI targeting the given 4 KiB-aligned
 * physical trampoline address.  The vector field is phys_addr >> 12. */
static inline uint32_t apic_icr_sipi(uint32_t phys_addr)
{
    return APIC_ICR_SIPI | ((phys_addr >> 12) & 0xFFu);
}

/* Return the ICR_HI value that targets a specific LAPIC ID.
 * The destination ID occupies bits [31:24] of ICR_HI. */
static inline uint32_t apic_icr_hi_dest(uint8_t dest_id)
{
    return (uint32_t)dest_id << 24;
}

/* Return 1 if the ICR delivery-status bit is set (IPI still in flight). */
static inline int apic_icr_is_pending(uint32_t icr_lo)
{
    return (icr_lo & APIC_ICR_PENDING) != 0;
}

/* Extract the LAPIC ID field from the raw ID register value.
 * Bits [31:24] hold the ID on P6/Pentium 4 family CPUs. */
static inline uint8_t apic_id_from_reg(uint32_t reg)
{
    return (uint8_t)((reg >> 24) & 0xFFu);
}

#ifdef __is_kernel

/* -- Kernel-only LAPIC API ----------------------------------------------- */

/* Map the LAPIC MMIO and enable the LAPIC via SVR.  Call once on the BSP. */
void    apic_initialize(void);

/* Enable the LAPIC on an Application Processor (AP).
 * Identical to apic_initialize but skips the PIC disable step. */
void    apic_initialize_ap(void);

/* Signal End Of Interrupt to the local APIC.  Must be called at the end
 * of every LAPIC-delivered interrupt handler (timer, IPI, spurious, …). */
void    apic_eoi(void);

/* Return the LAPIC ID of the calling CPU (reads APIC_REG_ID MMIO). */
uint8_t apic_id(void);

/* Send an IPI to dest_id with the given ICR_LO value.
 * Writes ICR_HI (destination) first, then ICR_LO (which triggers send). */
void    apic_send_ipi(uint8_t dest_id, uint32_t icr_lo);

#endif /* __is_kernel */

#endif /* _KERNEL_APIC_H */
