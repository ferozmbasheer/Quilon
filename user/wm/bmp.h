/*
 * Quilon OS -- BMP decoding for the desktop wallpaper (ROADMAP3 18.5)
 *
 * Pure logic, host-tested (tests/test_widgets.c).  Handles the uncompressed
 * Windows BMPs every image tool writes: 24-bit, or 32-bit (BI_RGB or
 * BI_BITFIELDS with the usual 0x00RRGGBB masks), bottom-up or top-down.
 *
 * wm_wallpaper_render() turns one into a screen-sized background, drawn
 * once at startup, so compositing the desktop stays a plain copy: a small
 * image (<= BMP_TILE_MAX px both ways) is tiled, a larger one scaled to
 * fill the screen.
 */

#ifndef _USER_WM_BMP_H
#define _USER_WM_BMP_H

#include <stdint.h>
#include <gfx.h>

typedef struct {
    int      w, h;
    int      bpp;           /* 24 or 32 */
    int      top_down;      /* 1 = first row in the file is the top row */
    uint32_t data_off;      /* file offset of the pixel array */
    uint32_t stride;        /* bytes per row, padded to 4 */
} bmp_info_t;

static inline uint32_t bmp_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint32_t bmp_le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

#define BMP_MAX_DIM 4096

/* Parse the headers of a `len`-byte file.  0 = OK, -1 = not a BMP we draw. */
static inline int bmp_parse(const uint8_t *d, uint32_t len, bmp_info_t *out)
{
    if (len < 54 || d[0] != 'B' || d[1] != 'M') return -1;
    uint32_t off  = bmp_le32(d + 10);
    uint32_t hsz  = bmp_le32(d + 14);
    if (hsz < 40 || 14 + hsz > len) return -1;
    int32_t  w    = (int32_t)bmp_le32(d + 18);
    int32_t  h    = (int32_t)bmp_le32(d + 22);
    uint32_t bpp  = bmp_le16(d + 28);
    uint32_t comp = bmp_le32(d + 30);
    if (bmp_le16(d + 26) != 1) return -1;                 /* planes */
    if (bpp != 24 && bpp != 32) return -1;
    if (comp == 3) {                                      /* BI_BITFIELDS */
        if (bpp != 32 || 14 + 40 + 12 > len) return -1;
        /* The masks follow a 40-byte header, or sit in a v4/v5 header at
         * the same offset. */
        if (bmp_le32(d + 54) != 0x00FF0000u || bmp_le32(d + 58) != 0x0000FF00u ||
            bmp_le32(d + 62) != 0x000000FFu)
            return -1;
    } else if (comp != 0) {
        return -1;                                        /* RLE etc. */
    }
    int top_down = h < 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || w > BMP_MAX_DIM || h > BMP_MAX_DIM) return -1;
    uint32_t stride = ((uint32_t)w * (bpp / 8) + 3u) & ~3u;
    if (off > len || (uint64_t)stride * (uint32_t)h > len - off) return -1;

    out->w = w; out->h = h; out->bpp = (int)bpp; out->top_down = top_down;
    out->data_off = off; out->stride = stride;
    return 0;
}

/* Pixel (x, y), y = 0 the top row, as 0x00RRGGBB. */
static inline uint32_t bmp_pixel(const uint8_t *d, const bmp_info_t *b,
                                 int x, int y)
{
    int row = b->top_down ? y : b->h - 1 - y;
    const uint8_t *p = d + b->data_off + (uint32_t)row * b->stride
                         + (uint32_t)x * (uint32_t)(b->bpp / 8);
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];  /* B,G,R */
}

/* Images no bigger than this (both sides) are tiled; larger ones are
 * scaled to fill the screen. */
#define BMP_TILE_MAX 128

static inline uint32_t bmp_lerp(uint32_t a, uint32_t b, uint32_t f)  /* f: 0..256 */
{
    uint32_t rb = ((a & 0xFF00FFu) * (256 - f) + (b & 0xFF00FFu) * f) >> 8;
    uint32_t g  = ((a & 0x00FF00u) * (256 - f) + (b & 0x00FF00u) * f) >> 8;
    return (rb & 0xFF00FFu) | (g & 0x00FF00u);
}

/* Source coordinate (16.16 fixed point, clamped to the image) of the centre
 * of destination pixel `d`, for a scale of `s` (16.16 dst px per src px) and
 * the image's left/top edge at destination `o`. */
static inline int32_t bmp_src_coord(int d, int o, int64_t s, int n)
{
    int64_t u = (((int64_t)(d - o) << 16) + 32768) * 65536 / s - 32768;
    if (u < 0) u = 0;
    if (u > ((int64_t)(n - 1) << 16)) u = (int64_t)(n - 1) << 16;
    return (int32_t)u;
}

/*
 * Draw the wallpaper into the screen-sized canvas `bg`.  A small image
 * (<= BMP_TILE_MAX both ways) is tiled.  Anything bigger is scaled -- up or
 * down, bilinear, keeping its aspect -- to cover the whole canvas, centred,
 * the overflow cropped.  `fill` is unused unless the image is degenerate.
 */
static inline void wm_wallpaper_render(canvas_t *bg, const uint8_t *d,
                                       const bmp_info_t *b, uint32_t fill)
{
    int x, y;
    if (b->w <= BMP_TILE_MAX && b->h <= BMP_TILE_MAX) {
        for (y = 0; y < bg->h; y++)
            for (x = 0; x < bg->w; x++)
                bg->pixels[y * bg->pitch + x] =
                    bmp_pixel(d, b, x % b->w, y % b->h);
        return;
    }
    if (b->w <= 0 || b->h <= 0) {
        for (y = 0; y < bg->h; y++)
            for (x = 0; x < bg->w; x++) bg->pixels[y * bg->pitch + x] = fill;
        return;
    }
    /* Cover: the larger of the two scale factors. */
    int64_t sx = ((int64_t)bg->w << 16) / b->w, sy = ((int64_t)bg->h << 16) / b->h;
    int64_t sc = sx > sy ? sx : sy;
    int ox = (int)((bg->w - ((int64_t)b->w * sc >> 16)) / 2);
    int oy = (int)((bg->h - ((int64_t)b->h * sc >> 16)) / 2);
    for (y = 0; y < bg->h; y++) {
        int32_t v  = bmp_src_coord(y, oy, sc, b->h);
        int     y0 = v >> 16, y1 = y0 + 1 < b->h ? y0 + 1 : y0;
        uint32_t fy = (uint32_t)(v & 0xFFFF) >> 8;
        uint32_t *row = bg->pixels + y * bg->pitch;
        for (x = 0; x < bg->w; x++) {
            int32_t u  = bmp_src_coord(x, ox, sc, b->w);
            int     x0 = u >> 16, x1 = x0 + 1 < b->w ? x0 + 1 : x0;
            uint32_t fx = (uint32_t)(u & 0xFFFF) >> 8;
            uint32_t top = bmp_lerp(bmp_pixel(d, b, x0, y0), bmp_pixel(d, b, x1, y0), fx);
            uint32_t bot = bmp_lerp(bmp_pixel(d, b, x0, y1), bmp_pixel(d, b, x1, y1), fx);
            row[x] = bmp_lerp(top, bot, fy);
        }
    }
}

#endif /* _USER_WM_BMP_H */
