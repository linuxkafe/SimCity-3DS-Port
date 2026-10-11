/*
 * 3ds_render.c — top/bottom screen rendering using libctru legacy framebuffer.
 *
 * Uses the legacy GX framebuffer mode for direct pixel access.
 */

#include "3ds_render.h"
#include "3dsdbg.h"
#include "simcity_static_recomp.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

/* Rotation direction of the landscape source into the portrait framebuffer.
   0 = one direction, 1 = the anti-transpose (mirrored). If the picture comes
   out upside-down or mirrored, flip this. */
#ifndef DS3_ROTATE_MIRROR
#define DS3_ROTATE_MIRROR 1
#endif

/* ── Helper: convert 32-bit BGRA to 16-bit RGB565 ─────────────────── */

static inline uint16_t bgra_to_rgb565(uint32_t bgra) {
    uint8_t b = (uint8_t)(bgra & 0xFFu);
    uint8_t g = (uint8_t)((bgra >> 8) & 0xFFu);
    uint8_t r = (uint8_t)((bgra >> 16) & 0xFFu);
    uint8_t r5  = (r >> 3) & 0x1Fu;
    uint8_t g6  = (g >> 2) & 0x3Fu;
    uint8_t b5  = (b >> 3) & 0x1Fu;
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* ── Global framebuffer pointers ──────────────────────────────────── */

static uint16_t *g_top_fb = NULL;
static uint16_t *g_bottom_fb = NULL;
static uint16_t g_top_width = 0, g_top_height = 0;
static uint16_t g_bottom_width = 0, g_bottom_height = 0;

/* ── ds3_render_init() ────────────────────────────────────────────── */

void ds3_render_init(void) {
    /* gfx is already initialized by main. libctru documents that
       gfxGetFramebuffer() returns a DIFFERENT pointer every frame while
       double buffering is enabled (gfx.h:140), so the pointer is re-fetched
       each frame in ds3_render_frame() instead of cached for the session. */
    (void)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &g_top_width, &g_top_height);
    (void)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &g_bottom_width, &g_bottom_height);
}

/* -- ds3_render_exit() ------------------------------------------------ */

void ds3_render_exit(void) {
    /* gfxExit is called by main */
}

/* -- ds3_render_frame() ----------------------------------------------- */

void ds3_render_frame(const uint32_t *src,
                      unsigned src_width,
                      unsigned src_height,
                      unsigned crop_top,
                      unsigned crop_left,
                      unsigned crop_right) {
    if (!src || !g_top_width || !g_top_height) return;

    /* Re-fetch the write pointer every frame: libctru may hand back a
       different buffer after gfxSwapBuffers() (gfx.h:140). Caching it was
       the cause of the triplicated / stale image. */
    g_top_fb = (uint16_t *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT,
                                             &g_top_width, &g_top_height);
    if (!g_top_fb) return;

    /* The 3DS screens are PORTRAIT, rotated 90 degrees counterclockwise
       (gfx.h:9): the top framebuffer is g_top_width (240) wide by
       g_top_height (400) tall but is displayed as a landscape panel. The
       emulated SNES frame is landscape, so it must be TRANSPOSED on the
       way in; drawing it unrotated yields a sideways, over-wide image. */
    const unsigned fb_w = g_top_width;   /* portrait width  (240) */
    const unsigned fb_h = g_top_height;  /* portrait height (400) */

    /* Visible source region after the frontend's crop. */
    unsigned rows = (crop_top < src_height) ? (src_height - crop_top) : 0u;
    unsigned row_pixels = (crop_left + crop_right < src_width)
                        ? (src_width - crop_left - crop_right) : 0u;
    if (rows == 0u || row_pixels == 0u) return;

    const uint32_t *row_src = src + (size_t)crop_top * src_width + crop_left;

    /* After transposition the destination footprint is
       height = source width, width = source height. Fit it in the frame. */
    unsigned dst_h = row_pixels;   /* source width  -> dest rows    */
    unsigned dst_w = rows;         /* source height -> dest columns */
    if (dst_w > fb_w) dst_w = fb_w;
    if (dst_h > fb_h) dst_h = fb_h;

    unsigned off_x = (fb_w - dst_w) / 2u;
    unsigned off_y = (fb_h - dst_h) / 2u;

    /* Clear entire framebuffer to black first */
    memset(g_top_fb, 0, (size_t)fb_w * fb_h * sizeof(uint16_t));

    /* Place the landscape source into the portrait framebuffer.

       Orientation was settled empirically, not guessed: a pixel dump of the
       real device (SELECT -> sdmc:/dbg_fb.raw, converted by tools/raw2png.py)
       was replayed offline through every candidate mapping. The LCD presents
       the portrait framebuffer rotated 90 deg counter-clockwise (gfx.h:9), so
       the correct payload to store is the source rotated 90 deg CLOCKWISE:

           rotate CW:  source (x, y)  ->  dest (rows - 1 - y, x)

       That yields a destination footprint of (source height) wide by
       (source width) tall, which is centred in the portrait framebuffer.
       Composed with the panel's own rotation this produces the upright title
       screen; the alternative direction renders it upside-down. */

    for (unsigned dy = 0; dy < dst_h; dy++) {
        for (unsigned dx = 0; dx < dst_w; dx++) {
            /* dest column dx tracks a source row, dest row dy a source column */
            unsigned sy = dx;
            unsigned sx = dy;
            if (sy >= rows)   sy = rows - 1u;
            if (sx >= row_pixels) sx = row_pixels - 1u;
            uint32_t pixel = row_src[(size_t)sy * row_pixels + sx];
            g_top_fb[(size_t)(off_y + dy) * fb_w + (off_x + dx)] =
                bgra_to_rgb565(pixel);
        }
    }

    /* Flush once after the transpose is complete. The buffer swap is NOT
       done here: gfxSwapBuffers() swaps BOTH screens and must be called once
       per VBlank from the main loop (gfx.h:173). Swapping inside the render
       call swapped buffers mid-frame and, together with the console living on
       the bottom screen, produced the 3x/4x duplicated image. */
    gfxFlushBuffers();
}

/* ── ds3_render_hud() ──────────────────────────────────────────────── */

void ds3_render_hud(uint64_t instruction_count,
                    uint64_t master_clock,
                    uint32_t frame_num,
                    const char *error_msg) {
    if (!g_bottom_fb) return;

    /* Clear to black */
    memset(g_bottom_fb, 0, g_bottom_width * g_bottom_height * sizeof(uint16_t));

    /* Draw HUD text via the bottom-screen console (consoleInit(GFX_BOTTOM)).
       The console renders its own font over the framebuffer each VBlank. */
    printf("\x1b[2J\x1b[H");          /* clear + home */
    printf("SimCity 3DS\n");
    printf("Frame: %u\n", (unsigned)frame_num);
    printf("Instructions: %llu\n", (unsigned long long)instruction_count);
    printf("Master clock: %llu\n", (unsigned long long)master_clock);
    if (error_msg && error_msg[0]) {
        printf("Error: %s\n", error_msg);
    }
}

/* ── ds3_render_clear() ────────────────────────────────────────────── */

void ds3_render_clear(void) {
    if (g_top_fb) memset(g_top_fb, 0, g_top_width * g_top_height * sizeof(uint16_t));
    if (g_bottom_fb) memset(g_bottom_fb, 0, g_bottom_width * g_bottom_height * sizeof(uint16_t));
}