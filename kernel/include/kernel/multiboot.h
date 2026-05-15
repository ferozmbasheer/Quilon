#ifndef _KERNEL_MULTIBOOT_H
#define _KERNEL_MULTIBOOT_H

#include <stdint.h>

/* Bits in the multiboot_info_t.flags field */
#define MULTIBOOT_FLAG_MEM      (1 << 0)   /* mem_lower / mem_upper are valid  */
#define MULTIBOOT_FLAG_MODS     (1 << 3)   /* mods_count / mods_addr are valid */
#define MULTIBOOT_FLAG_MMAP     (1 << 6)   /* mmap_length / mmap_addr are valid */
#define MULTIBOOT_FLAG_VBE      (1 << 11)  /* vbe_* fields are valid            */
#define MULTIBOOT_FLAG_FB       (1 << 12)  /* framebuffer_* fields are valid    */

/* framebuffer_type values */
#define MULTIBOOT_FB_TYPE_INDEXED 0   /* palette mode        */
#define MULTIBOOT_FB_TYPE_RGB     1   /* direct-color RGB    */
#define MULTIBOOT_FB_TYPE_EGA     2   /* VGA text / EGA mode */

/* Values for multiboot_mmap_entry_t.type */
#define MULTIBOOT_MEMORY_AVAILABLE  1

/* Memory-map entry as written by GRUB.
 * 'size' does not include the size field itself; advance by size+4. */
typedef struct {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed)) multiboot_mmap_entry_t;

/*
 * Multiboot module descriptor -- one entry in the array at mods_addr.
 * Only valid when multiboot_info_t.flags has MULTIBOOT_FLAG_MODS set.
 */
typedef struct {
    uint32_t mod_start;   /* physical address of module data (inclusive) */
    uint32_t mod_end;     /* physical address of module data end (exclusive) */
    uint32_t string;      /* module name / command line (physical addr, may be 0) */
    uint32_t reserved;
} __attribute__((packed)) multiboot_module_t;

/* The info struct that GRUB places in memory and whose address is in %ebx.
 * Field offsets follow the Multiboot v1 specification exactly; the packed
 * attribute prevents any compiler-inserted padding.                         */
typedef struct {
    uint32_t flags;               /* offset   0 */
    uint32_t mem_lower;           /* offset   4 -- KB of lower memory  (bit 0) */
    uint32_t mem_upper;           /* offset   8 -- KB of upper memory  (bit 0) */
    uint32_t boot_device;         /* offset  12 */
    uint32_t cmdline;             /* offset  16 */
    uint32_t mods_count;          /* offset  20 */
    uint32_t mods_addr;           /* offset  24 */
    uint32_t syms[4];             /* offset  28 -- aout/elf table info (16 B) */
    uint32_t mmap_length;         /* offset  44 -- bytes in mmap buffer (bit 6) */
    uint32_t mmap_addr;           /* offset  48 -- phys addr of mmap buffer     */
    uint32_t drives_length;       /* offset  52 -- (bit 7) */
    uint32_t drives_addr;         /* offset  56 -- (bit 7) */
    uint32_t config_table;        /* offset  60 -- (bit 8) */
    uint32_t boot_loader_name;    /* offset  64 -- (bit 9) */
    uint32_t apm_table;           /* offset  68 -- (bit 10) */
    uint32_t vbe_control_info;    /* offset  72 -- (bit 11) */
    uint32_t vbe_mode_info;       /* offset  76 -- (bit 11) */
    uint16_t vbe_mode;            /* offset  80 -- (bit 11) */
    uint16_t vbe_interface_seg;   /* offset  82 -- (bit 11) */
    uint16_t vbe_interface_off;   /* offset  84 -- (bit 11) */
    uint16_t vbe_interface_len;   /* offset  86 -- (bit 11) */
    uint64_t framebuffer_addr;    /* offset  88 -- (bit 12) phys base of fb    */
    uint32_t framebuffer_pitch;   /* offset  96 -- (bit 12) bytes per scanline */
    uint32_t framebuffer_width;   /* offset 100 -- (bit 12) pixels per row     */
    uint32_t framebuffer_height;  /* offset 104 -- (bit 12) pixel rows         */
    uint8_t  framebuffer_bpp;     /* offset 108 -- (bit 12) bits per pixel     */
    uint8_t  framebuffer_type;    /* offset 109 -- (bit 12) 0=indexed 1=RGB    */
    uint8_t  color_info[6];       /* offset 110 -- (bit 12) RGB field offsets  */
} __attribute__((packed)) multiboot_info_t;

#endif /* _KERNEL_MULTIBOOT_H */
