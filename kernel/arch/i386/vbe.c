/*
 * Quilon OS — VGA Graphics Mode (VESA/VBE) driver  (section 10.4)
 *
 * Supports 32 bpp and 24 bpp linear framebuffers.
 * GRUB sets up the mode via:
 *   set gfxmode=800x600x32
 *   set gfxpayload=keep
 * and fills in the Multiboot framebuffer_* fields.
 *
 * kernel_main calls vbe_init() after paging is ready (so that the
 * framebuffer physical pages can be mapped into virtual memory).  Once
 * vbe_init() returns true, tty.c redirects all terminal_* calls here.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include <kernel/vbe.h>
#include <kernel/psf.h>
#include <kernel/paging.h>
#include <kernel/pmm.h>

/* ── Module state ───────────────────────────────────────────────────────── */

static vbe_info_t vbe;           /* cached copy of the framebuffer geometry */
static bool       vbe_ready = false;

/*
 * Double-buffer: render into shadow_buf (RAM), then flush to hardware in one
 * memcpy.  Allocated from PMM pages mapped at VBE_SHADOW_VBASE so it lives
 * outside the 4 MiB kernel heap window and is inherited by all process page
 * directories via paging_create_address_space (which copies PD[768..1023]).
 */
static uint8_t *shadow_buf = NULL;

/*
 * Dirty-row tracking: record the pixel-row range [dirty_y_min, dirty_y_max)
 * modified since the last vbe_flush().  vbe_flush() copies only that band,
 * typically 16–32 pixel rows, instead of the full 4 MiB framebuffer.
 * Sentinel dirty_y_min=0xFFFFFFFFu means "nothing dirty yet".
 */
static uint32_t dirty_y_min = 0xFFFFFFFFu;
static uint32_t dirty_y_max = 0u;

static inline void dirty_mark(uint32_t y)
{
    if (y < dirty_y_min) dirty_y_min = y;
    if (y + 1u > dirty_y_max) dirty_y_max = y + 1u;
}

static inline void dirty_mark_all(void)
{
    dirty_y_min = 0;
    dirty_y_max = vbe.height;
}

/* Terminal state */
static uint32_t term_cols;
static uint32_t term_rows;
static uint32_t term_col;        /* cursor column (char units) */
static uint32_t term_row;        /* cursor row    (char units) */
static uint32_t term_fg;
static uint32_t term_bg;

/* ── Runtime font dimension helpers ─────────────────────────────────────── */
/*
 * Return the active glyph cell width/height.  When a PSF2 font has been
 * loaded via psf2_load(), these return the PSF2 font's dimensions; otherwise
 * they fall back to the compile-time built-in 8×16 constants.
 *
 * All terminal and drawing code must call these instead of using VBE_FONT_W /
 * VBE_FONT_H directly so that a loaded PSF2 font is picked up automatically.
 */
static inline uint32_t cur_font_w(void)
{
    const psf2_font_t *psf = psf2_get_font();
    return psf ? psf->width : (uint32_t)VBE_FONT_W;
}

static inline uint32_t cur_font_h(void)
{
    const psf2_font_t *psf = psf2_get_font();
    return psf ? psf->height : (uint32_t)VBE_FONT_H;
}

/* ── Internal helpers ───────────────────────────────────────────────────── */

/* Return a pointer to the start of pixel (x, y).
 * Writes go to the shadow buffer when it is available, otherwise directly
 * to the hardware framebuffer (fallback before shadow_buf is set up). */
static inline uint8_t *fb_ptr(uint32_t x, uint32_t y)
{
    uint8_t *base = shadow_buf
        ? shadow_buf
        : (uint8_t *)(uintptr_t)(uint32_t)vbe.addr;
    return base + vbe_pixel_offset(x, y, vbe.pitch, vbe.bpp >> 3u);
}

/* Write one pixel at (x, y).  Handles both 32 bpp and 24 bpp. */
static inline void fb_write(uint32_t x, uint32_t y, uint32_t color)
{
    dirty_mark(y);
    uint8_t *p = fb_ptr(x, y);
    if (vbe.bpp == 32) {
        /* 32 bpp: BGRX layout — write as a native 32-bit dword. */
        *(uint32_t *)p = color;
    } else {
        /* 24 bpp: three individual bytes. */
        p[0] = (uint8_t)( color        & 0xFF); /* B */
        p[1] = (uint8_t)((color >>  8) & 0xFF); /* G */
        p[2] = (uint8_t)((color >> 16) & 0xFF); /* R */
    }
}

/* ── Public drawing primitives ──────────────────────────────────────────── */

void vbe_draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (!vbe_ready || x >= vbe.width || y >= vbe.height) return;
    fb_write(x, y, color);
}

void vbe_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   uint32_t color)
{
    if (!vbe_ready) return;
    if (x >= vbe.width  || y >= vbe.height) return;
    if (x + w > vbe.width)  w = vbe.width  - x;
    if (y + h > vbe.height) h = vbe.height - y;

    dirty_mark(y);
    if (h > 0) dirty_mark(y + h - 1u);

    if (vbe.bpp == 32) {
        for (uint32_t row = 0; row < h; row++) {
            uint32_t *line = (uint32_t *)fb_ptr(x, y + row);
            for (uint32_t col = 0; col < w; col++)
                line[col] = color;
        }
    } else {
        for (uint32_t row = 0; row < h; row++)
            for (uint32_t col = 0; col < w; col++)
                fb_write(x + col, y + row, color);
    }
}

void vbe_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
    if (!vbe_ready) return;
    unsigned char ci = (unsigned char)c;

    const psf2_font_t *psf = psf2_get_font();
    if (psf) {
        /* PSF2 path: use loaded bitmap font */
        if ((uint32_t)ci >= psf->glyph_count) ci = (unsigned char)'?';
        for (uint32_t row = 0; row < psf->height; row++) {
            for (uint32_t col = 0; col < psf->width; col++) {
                uint32_t color = psf2_glyph_pixel(psf, ci, col, row) ? fg : bg;
                if (x + col < vbe.width && y + row < vbe.height)
                    fb_write(x + col, y + row, color);
            }
        }
    } else {
        /* Built-in 8×16 path (rows 0–7: data, rows 8–15: blank) */
        if (ci > 127u) ci = '?';
        for (uint32_t row = 0; row < VBE_FONT_H; row++) {
            uint8_t bits = (row < 8u) ? vbe_font8x8[ci][row] : 0u;
            for (uint32_t col = 0; col < VBE_FONT_W; col++) {
                uint32_t color = (bits & (0x80u >> col)) ? fg : bg;
                if (x + col < vbe.width && y + row < vbe.height)
                    fb_write(x + col, y + row, color);
            }
        }
    }
}

uint32_t vbe_draw_string(uint32_t x, uint32_t y, const char *s,
                          uint32_t fg, uint32_t bg)
{
    uint32_t fw = cur_font_w();
    while (*s) {
        vbe_draw_char(x, y, *s++, fg, bg);
        x += fw;
    }
    return x;
}

/* ── Terminal scrolling ─────────────────────────────────────────────────── */

static void vbe_scroll_up(void)
{
    uint8_t *fb = shadow_buf
        ? shadow_buf
        : (uint8_t *)(uintptr_t)(uint32_t)vbe.addr;
    uint32_t fh    = cur_font_h();
    uint32_t row_b = vbe.pitch * fh;    /* bytes per text row */
    uint32_t total = row_b * (term_rows - 1u);

    /* memmove shifts the whole framebuffer — mark entire screen dirty before
     * the move so vbe_flush() copies the full updated content.             */
    dirty_mark_all();
    memmove(fb, fb + row_b, total);

    /* Clear the last text row. */
    vbe_fill_rect(0, (term_rows - 1u) * fh, vbe.width, fh, term_bg);
}

/* ── Terminal emulator ──────────────────────────────────────────────────── */

void vbe_terminal_init(void)
{
    if (!vbe_ready) return;

    /* Use PSF2 dimensions when a font is loaded; fall back to built-in 8×16. */
    term_cols = vbe.width  / cur_font_w();
    term_rows = vbe.height / cur_font_h();
    term_col  = 0;
    term_row  = 0;
    term_fg   = VBE_COLOR_LIGHT_GREY;
    term_bg   = VBE_COLOR_DARK_BLUE;

    /* Clear screen with background colour. */
    vbe_fill_rect(0, 0, vbe.width, vbe.height, term_bg);
}

void vbe_terminal_setcolor(uint32_t fg, uint32_t bg)
{
    term_fg = fg;
    term_bg = bg;
}

void vbe_terminal_putchar(char c)
{
    if (!vbe_ready) return;

    if (c == '\n') {
        term_col = 0;
        if (++term_row >= term_rows) {
            vbe_scroll_up();
            term_row = term_rows - 1u;
        }
        return;
    }
    if (c == '\r') {
        term_col = 0;
        return;
    }
    if (c == '\b') {
        if (term_col > 0) {
            term_col--;
        } else if (term_row > 0) {
            term_row--;
            term_col = term_cols - 1u;
        }
        vbe_draw_char(term_col * cur_font_w(), term_row * cur_font_h(),
                      ' ', term_fg, term_bg);
        return;
    }
    if ((unsigned char)c < 0x20u) return;  /* skip other control chars */

    vbe_draw_char(term_col * cur_font_w(), term_row * cur_font_h(),
                  c, term_fg, term_bg);

    if (++term_col >= term_cols) {
        term_col = 0;
        if (++term_row >= term_rows) {
            vbe_scroll_up();
            term_row = term_rows - 1u;
        }
    }
}

void vbe_terminal_write(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
        vbe_terminal_putchar(data[i]);
}

void vbe_terminal_writestring(const char *s)
{
    while (*s) vbe_terminal_putchar(*s++);
}

void vbe_terminal_set_cursor(uint32_t row, uint32_t col)
{
    if (!vbe_ready) return;
    term_row = (row < term_rows) ? row : term_rows - 1u;
    term_col = (col < term_cols) ? col : term_cols - 1u;
}

void vbe_terminal_get_cursor(uint32_t *row, uint32_t *col)
{
    *row = vbe_ready ? term_row : 0u;
    *col = vbe_ready ? term_col : 0u;
}

void vbe_terminal_clear_screen(void)
{
    if (!vbe_ready) return;
    vbe_fill_rect(0, 0, vbe.width, vbe.height, term_bg);
    /* cursor position intentionally unchanged */
}

void vbe_terminal_erase_line(int mode)
{
    if (!vbe_ready) return;
    uint32_t fw = cur_font_w();
    uint32_t fh = cur_font_h();
    uint32_t x, w;
    switch (mode) {
    case 1:  /* from start of line to cursor */
        x = 0;
        w = (term_col + 1u) * fw;
        break;
    case 2:  /* entire line */
        x = 0;
        w = term_cols * fw;
        break;
    default: /* 0: from cursor to end of line */
        x = term_col * fw;
        w = (term_cols - term_col) * fw;
        break;
    }
    vbe_fill_rect(x, term_row * fh, w, fh, term_bg);
}

/* ── Double-buffer flush ────────────────────────────────────────────────── */

void vbe_flush(void)
{
    if (!vbe_ready || !shadow_buf) return;
    if (dirty_y_min >= dirty_y_max) return;
    uint32_t off  = dirty_y_min * vbe.pitch;
    uint32_t size = (dirty_y_max - dirty_y_min) * vbe.pitch;
    memcpy((uint8_t *)(uintptr_t)(uint32_t)vbe.addr + off,
           shadow_buf + off, size);
    dirty_y_min = 0xFFFFFFFFu;
    dirty_y_max = 0u;
}

/* ── Initialization ─────────────────────────────────────────────────────── */

bool vbe_init(const vbe_info_t *info)
{
    if (!info) return false;
    if (info->bpp < 24) return false;
    if (info->width == 0 || info->height == 0) return false;
    if (info->type != 1) return false;  /* must be RGB, not indexed or EGA */

    vbe = *info;

    /*
     * Map the framebuffer physical pages into the kernel virtual address
     * space at the same address (identity mapping).  paging_map_page_alloc()
     * allocates a page table if one does not already exist for the PD slot
     * covering virt.  No PAGE_USER: framebuffer is kernel-only.
     */
    uint32_t phys  = (uint32_t)info->addr;
    uint32_t bytes = info->pitch * info->height;
    uint32_t pages = (bytes + (PAGE_SIZE - 1u)) / PAGE_SIZE;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t pa = phys + i * PAGE_SIZE;
        paging_map_page_alloc(pa, pa, PAGE_PRESENT | PAGE_WRITABLE);
    }

    /*
     * Allocate the shadow buffer: one PMM page per framebuffer page, mapped
     * contiguously at VBE_SHADOW_VBASE (PD[769], outside the heap window).
     * paging_create_address_space copies PD[768..1023], so all future
     * process page directories inherit this mapping automatically.
     */
    {
        uint32_t shadow_pages = (bytes + (PAGE_SIZE - 1u)) / PAGE_SIZE;
        uint32_t virt = VBE_SHADOW_VBASE;
        bool ok = true;
        for (uint32_t i = 0; i < shadow_pages; i++) {
            /* Prefer pages above 4 MiB: shadow buffer data is only ever
             * accessed via virtual addresses, so the physical page can live
             * anywhere.  Using high pages preserves the sub-4 MiB identity-
             * mapped pool for paging structures (PDs and PTs).             */
            void *pg = pmm_alloc_page_above_4mib();
            if (!pg) pg = pmm_alloc_page();   /* fallback if high mem full */
            if (!pg) { ok = false; break; }
            paging_map_page_alloc(virt, (uint32_t)pg,
                                  PAGE_PRESENT | PAGE_WRITABLE);
            virt += PAGE_SIZE;
        }
        if (ok) {
            shadow_buf = (uint8_t *)VBE_SHADOW_VBASE;
            memset(shadow_buf, 0, bytes);
        }
    }

    vbe_ready = true;
    vbe_terminal_init();
    vbe_flush();          /* push the cleared screen to hardware */
    return true;
}

bool vbe_active(void)
{
    return vbe_ready;
}

const vbe_info_t *vbe_get_info(void)
{
    return vbe_ready ? &vbe : (const vbe_info_t *)0;
}

/* ── Graphical demo ─────────────────────────────────────────────────────── */

void vbe_demo(void)
{
    if (!vbe_ready) {
        printf("vbe: not active (grub.cfg needs gfxmode/gfxpayload)\r\n");
        return;
    }

    const uint32_t W = vbe.width;
    const uint32_t H = vbe.height;

    /* ── 1. Full-screen background ─────────────────────────────────────── */
    vbe_fill_rect(0, 0, W, H, VBE_COLOR_DARK_BLUE);

    /* ── 2. Horizontal colour gradient bar ─────────────────────────────── */
    uint32_t bar_h = 32;
    for (uint32_t x = 0; x < W; x++) {
        uint32_t r = (x * 255u) / W;
        uint32_t g = ((W - x) * 128u) / W;
        uint32_t b = 128u + (x * 127u) / W;
        uint32_t c = (r << 16) | (g << 8) | b;
        for (uint32_t y = 0; y < bar_h; y++)
            vbe_draw_pixel(x, y, c);
    }

    /* ── 3. Colour swatches ─────────────────────────────────────────────── */
    static const uint32_t swatches[] = {
        VBE_COLOR_WHITE, VBE_COLOR_RED,   VBE_COLOR_GREEN,  VBE_COLOR_BLUE,
        VBE_COLOR_CYAN,  VBE_COLOR_MAGENTA, VBE_COLOR_YELLOW, VBE_COLOR_ORANGE,
        VBE_COLOR_LIGHT_GREY, VBE_COLOR_DARK_GREY, VBE_COLOR_DARK_BLUE,
    };
    uint32_t sw = 48, sh = 24;
    uint32_t swatch_y = bar_h + 8;
    for (uint32_t i = 0; i < 11u; i++)
        vbe_fill_rect(8u + i * (sw + 4u), swatch_y, sw, sh, swatches[i]);

    /* ── 4. Font/info header ────────────────────────────────────────────── */
    uint32_t fw = cur_font_w();
    uint32_t fh = cur_font_h();
    uint32_t ty = swatch_y + sh + 12;
    vbe_draw_string(8, ty, "Quilon VBE terminal  (section 10.4)",
                    VBE_COLOR_YELLOW, VBE_COLOR_DARK_BLUE);
    ty += fh + 2;

    char info_buf[64];
    /* Print framebuffer info using the existing kernel printf convention. */
    static const char *hex_digits = "0123456789ABCDEF";
    /* Manually format "WxHxBPP @ 0xADDR  pitch=P" */
    uint32_t wi = vbe.width, hi = vbe.height, bi = vbe.bpp, pi = vbe.pitch;
    uint32_t ai = (uint32_t)vbe.addr;
    int pos = 0;
    /* width */
    if (wi >= 1000) info_buf[pos++] = (char)('0' + wi / 1000);
    info_buf[pos++] = (char)('0' + (wi / 100) % 10);
    info_buf[pos++] = (char)('0' + (wi /  10) % 10);
    info_buf[pos++] = (char)('0' + (wi)       % 10);
    info_buf[pos++] = 'x';
    /* height */
    if (hi >= 1000) info_buf[pos++] = (char)('0' + hi / 1000);
    info_buf[pos++] = (char)('0' + (hi / 100) % 10);
    info_buf[pos++] = (char)('0' + (hi /  10) % 10);
    info_buf[pos++] = (char)('0' + (hi)       % 10);
    info_buf[pos++] = 'x';
    /* bpp */
    info_buf[pos++] = (char)('0' + bi / 10);
    info_buf[pos++] = (char)('0' + bi % 10);
    /* addr */
    static const char addr_prefix[] = " @ 0x";
    for (int k = 0; addr_prefix[k]; k++) info_buf[pos++] = addr_prefix[k];
    for (int k = 28; k >= 0; k -= 4)
        info_buf[pos++] = hex_digits[(ai >> k) & 0xFu];
    /* pitch */
    static const char pitch_prefix[] = "  pitch=";
    for (int k = 0; pitch_prefix[k]; k++) info_buf[pos++] = pitch_prefix[k];
    if (pi >= 10000) info_buf[pos++] = (char)('0' + pi / 10000);
    if (pi >= 1000)  info_buf[pos++] = (char)('0' + (pi / 1000) % 10);
    info_buf[pos++] = (char)('0' + (pi / 100) % 10);
    info_buf[pos++] = (char)('0' + (pi /  10) % 10);
    info_buf[pos++] = (char)('0' + (pi)       % 10);
    info_buf[pos] = '\0';

    vbe_draw_string(8, ty, info_buf, VBE_COLOR_CYAN, VBE_COLOR_DARK_BLUE);
    ty += fh + 4;

    /* Terminal dimensions. */
    uint32_t tc = W / fw, tr = H / fh;
    /* Reuse info_buf for "Terminal: CCxRR chars" */
    pos = 0;
    static const char term_prefix[] = "Terminal: ";
    for (int k = 0; term_prefix[k]; k++) info_buf[pos++] = term_prefix[k];
    if (tc >= 100) info_buf[pos++] = (char)('0' + tc / 100);
    info_buf[pos++] = (char)('0' + (tc / 10) % 10);
    info_buf[pos++] = (char)('0' + tc % 10);
    info_buf[pos++] = 'x';
    if (tr >= 100) info_buf[pos++] = (char)('0' + tr / 100);
    info_buf[pos++] = (char)('0' + (tr / 10) % 10);
    info_buf[pos++] = (char)('0' + tr % 10);
    static const char chars_suffix[] = " chars";
    for (int k = 0; chars_suffix[k]; k++) info_buf[pos++] = chars_suffix[k];
    info_buf[pos] = '\0';
    vbe_draw_string(8, ty, info_buf, VBE_COLOR_GREEN, VBE_COLOR_DARK_BLUE);
    ty += fh + 8;

    /* ── 5. Full printable ASCII glyph table ────────────────────────────── */
    vbe_draw_string(8, ty, "Printable ASCII (0x20-0x7E):",
                    VBE_COLOR_WHITE, VBE_COLOR_DARK_BLUE);
    ty += fh + 2;

    uint32_t gx = 8, gy = ty;
    for (unsigned char ch = 0x20; ch <= 0x7Eu; ch++) {
        vbe_draw_char(gx, gy, (char)ch, VBE_COLOR_WHITE, VBE_COLOR_DARK_BLUE);
        gx += fw + 1u;
        if (gx + fw + 1u >= W - 8u) {
            gx = 8;
            gy += fh + 1u;
        }
    }
    ty = gy + fh + 12;

    /* ── 6. Progress bar ────────────────────────────────────────────────── */
    uint32_t bar_w = W - 16u, bar_filled = bar_w * 3u / 4u;
    vbe_fill_rect(8, ty, bar_w, 12, VBE_COLOR_DARK_GREY);
    vbe_fill_rect(8, ty, bar_filled, 12, VBE_COLOR_GREEN);
    vbe_draw_string(bar_w + 12, ty, "vbe demo done", VBE_COLOR_WHITE, VBE_COLOR_DARK_BLUE);

    /*
     * Restore the VBE terminal cursor below all the drawn graphics so that
     * subsequent printf() calls do not overwrite the demo.
     */
    uint32_t new_row = (ty + 20) / fh;
    if (new_row >= term_rows) new_row = term_rows - 1u;
    term_row = new_row;
    term_col = 0;

    vbe_flush();
}
