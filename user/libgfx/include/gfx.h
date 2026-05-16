/*
 * Quilon OS -- user-space 2D graphics library (section 14.1)
 *
 * All drawing is done into an off-screen canvas (canvas_t).  The compositor
 * owns the screen canvas, which is backed by the kernel shadow buffer mapped
 * via SYS_GFX_MAP.  All other apps draw into malloc-backed canvases and hand
 * them to the compositor.  Call gfx_flush() to push the screen canvas to the
 * physical display.
 *
 * Coordinate system: (0,0) is the top-left pixel; x increases right, y down.
 * All drawing functions clip to the canvas bounds and, if a clip rect is
 * active, to that rectangle too.
 */

#ifndef _LIBGFX_GFX_H
#define _LIBGFX_GFX_H

#include <stdint.h>

/* -- Framebuffer geometry (filled by SYS_GFX_INFO) ---------------------- */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;   /* bytes per scanline */
    uint32_t bpp;     /* always 32 for VBE */
} gfx_info_t;

/* -- Core types ---------------------------------------------------------- */

typedef uint32_t color_t;   /* 0x00RRGGBB */

typedef struct {
    int x, y, w, h;
} rect_t;

typedef struct {
    uint32_t *pixels;  /* pixel buffer (one uint32_t per pixel) */
    int       w;       /* width in pixels */
    int       h;       /* height in pixels */
    int       pitch;   /* pixels per scanline (>= w) */
    /* active clip rectangle; clamped to canvas bounds by gfx_set_clip() */
    int clip_x0, clip_y0, clip_x1, clip_y1;
    int has_clip;
} canvas_t;

/* -- Colour helpers ------------------------------------------------------ */

#define GFX_RGB(r, g, b) \
    (((color_t)(r) << 16) | ((color_t)(g) << 8) | (color_t)(b))

#define GFX_RGBA(r, g, b, a) \
    (((color_t)(a) << 24) | ((color_t)(r) << 16) | ((color_t)(g) << 8) | (color_t)(b))

/* Standard palette */
#define GFX_BLACK      GFX_RGB(0,   0,   0  )
#define GFX_WHITE      GFX_RGB(255, 255, 255)
#define GFX_RED        GFX_RGB(200, 40,  40 )
#define GFX_GREEN      GFX_RGB(0,   200, 0  )
#define GFX_BLUE       GFX_RGB(0,   0,   200)
#define GFX_DARK_BLUE  GFX_RGB(30,  30,  60 )

/* -- Canvas management --------------------------------------------------- */

/*
 * canvas_create -- allocate an off-screen canvas backed by malloc.
 * Returns NULL on OOM.
 */
canvas_t *canvas_create(int w, int h);

/* Free a canvas returned by canvas_create.  Do NOT call on the screen canvas. */
void canvas_free(canvas_t *c);

/* -- Screen canvas ------------------------------------------------------- */

/*
 * gfx_screen_init -- query VBE info and map the shadow buffer into user space.
 *
 * Fills *info (if non-NULL) with the screen dimensions.
 * Returns a canvas_t wrapping the shadow buffer on success, NULL on failure
 * (VBE not active, or kernel refuses the mapping).
 *
 * Only one process (the compositor) should call this.
 */
canvas_t *gfx_screen_init(gfx_info_t *info);

/*
 * gfx_flush -- copy the shadow buffer to the physical display (SYS_GFX_FLUSH).
 * Call after compositing a complete frame.
 */
void gfx_flush(void);

/* -- Clip rectangle ------------------------------------------------------ */

/* Restrict drawing to the intersection of rect and the canvas bounds.
 * Drawing outside the clip is silently discarded.                       */
void gfx_set_clip(canvas_t *c, rect_t r);

/* Remove the clip rectangle; drawing is limited only by canvas bounds. */
void gfx_clear_clip(canvas_t *c);

/* -- Drawing primitives -------------------------------------------------- */

/* Fill the entire canvas with col. */
void gfx_fill(canvas_t *c, color_t col);

/* Fill rectangle r with col.  Clipped to canvas bounds + active clip rect. */
void gfx_fill_rect(canvas_t *c, rect_t r, color_t col);

/* Draw a 1-pixel outline of rectangle r.  Clipped. */
void gfx_draw_rect(canvas_t *c, rect_t r, color_t col);

/*
 * gfx_blit -- copy a rectangle from src into dst.
 *
 * Pixels at src_rect in src are copied to (dx, dy) in dst.
 * Clipped to dst bounds + dst clip rect.  src_rect is clamped to src bounds.
 */
void gfx_blit(canvas_t *dst, int dx, int dy,
              const canvas_t *src, rect_t src_rect);

/*
 * gfx_draw_text -- render a null-terminated ASCII string at (x, y).
 *
 * Uses the embedded 8×16 bitmap font (8 px wide, 16 px tall cell).
 * fg = foreground colour; bg = background colour.
 * Clipped to canvas bounds + active clip rect.
 */
void gfx_draw_text(canvas_t *c, int x, int y,
                   const char *str, color_t fg, color_t bg);

/*
 * gfx_char_w / gfx_char_h -- width and height of one character cell.
 * Use to compute text layout without magic numbers.
 */
#define GFX_CHAR_W  8
#define GFX_CHAR_H  16

#endif /* _LIBGFX_GFX_H */
