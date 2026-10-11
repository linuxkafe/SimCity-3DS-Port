/*
 * ds_render.c — top/bottom screen rendering helpers using libnds.
 *
 * The core emits 256x239 or 398x239 BGRA frames (32-bit per pixel).
 * The DS top screen is mode 4 (16-bit RGB5551 bitmap). We convert on the fly.
 */

#include "ds_render.h"
#include "simcity_static_recomp.h"

#include <nds.h>
#include <nds/arm9/video.h>
#include <string.h>
#include <stdio.h>

/* ── Helper: convert 32-bit BGRA to 16-bit RGB5551 ───────────────── */

static inline uint16_t bgra_to_rgb5551(uint32_t bgra) {
    /* BGRA layout on x86 is blue, green, red, alpha (little-endian).
       For SNES games, channels are always non-zero (no transparency used). */
    uint8_t b = (uint8_t)(bgra & 0xFFu);
    uint8_t g = (uint8_t)((bgra >> 8) & 0xFFu);
    uint8_t r = (uint8_t)((bgra >> 16) & 0xFFu);
    /* Extract 5 bits from each channel */
    uint8_t r5  = (r >> 3) & 0x1Fu;
    uint8_t g5  = (g >> 3) & 0x1Fu;
    uint8_t b5  = (b >> 3) & 0x1Fu;
    return (uint16_t)((b5 << 10) | (g5 << 5) | r5);
}

/* ── ds_render_frame() ───────────────────────────────────────────── */

void ds_render_frame(const uint32_t *src,
                     unsigned src_width,
                     unsigned src_height,
                     unsigned crop_top,
                     unsigned crop_left,
                     unsigned crop_right) {
    /* Ensure src fits in the top screen (256×192).
       If src_width > 256 (widescreen), crop both sides to 256. */
    if (src_width > DS_TOP_SCREEN_WIDTH) {
        unsigned excess = src_width - DS_TOP_SCREEN_WIDTH;
        crop_left  += excess / 2;
        crop_right += excess - (excess / 2);
        src_width = DS_TOP_SCREEN_WIDTH;
    }

    /* Determine the source row stride (in pixels). The frame buffer is
       contiguous; each row is src_width pixels wide. */
    const uint32_t *row_src = src + (size_t)crop_top * src_width + crop_left;
    unsigned row_pixels = src_width - crop_left - crop_right;
    if (row_pixels > DS_TOP_SCREEN_WIDTH) row_pixels = DS_TOP_SCREEN_WIDTH;
    if (row_pixels == 0) return;

    /* Get the top screen pixel buffer (mode 4, 16 bpp) */
    uint16_t *dst = (uint16_t *)videoGetBufferPtr(0);
    if (!dst) return;

    /* Render each visible row */
    for (unsigned dy = 0; dy < DS_TOP_SCREEN_HEIGHT && (crop_top + dy) < src_height; dy++) {
        const uint32_t *src_row = row_src + (size_t)dy * src_width;
        uint16_t *dst_row = dst + dy * DS_TOP_SCREEN_WIDTH;

        for (unsigned dx = 0; dx < row_pixels; dx++) {
            uint32_t pixel = src_row[dx];
            dst_row[dx] = bgra_to_rgb5551(pixel);
        }
        /* Fill remaining pixels with black */
        memset(dst_row + row_pixels, 0,
               (DS_TOP_SCREEN_WIDTH - row_pixels) * sizeof(uint16_t));
    }
}

/* ── ds_render_hud() ──────────────────────────────────────────────── */

void ds_render_hud(uint64_t instruction_count,
                   uint64_t master_clock,
                   uint32_t frame_num,
                   const char *error_msg) {
    /* Get the bottom screen pixel buffer (mode 3, 256x192 16bpp) */
    uint16_t *buf = (uint16_t *)videoGetBufferPtrSub(0);
    if (!buf) return;

    /* Clear to black */
    memset(buf, 0, DS_BOTTOM_SCREEN_WIDTH * DS_BOTTOM_SCREEN_HEIGHT * sizeof(uint16_t));

    /* Draw simple text using the built-in 5x7 font */
    /* Position: row 0, col 0 — use vramSubSet to write directly to sub VRAM */
    /* Since we don't have printf-on-GFX support, just draw status chars. */

    /* Debug text via console is easier; skip for now and just print status. */
    (void)instruction_count;
    (void)master_clock;
    (void)frame_num;
    (void)error_msg;
}

/* ── ds_render_clear() ────────────────────────────────────────────── */

void ds_render_clear(void) {
    uint16_t *top = (uint16_t *)videoGetBufferPtr(0);
    uint16_t *bot = (uint16_t *)videoGetBufferPtrSub(0);
    if (top) memset(top, 0, DS_TOP_SCREEN_WIDTH * DS_TOP_SCREEN_HEIGHT * sizeof(uint16_t));
    if (bot) memset(bot, 0, DS_BOTTOM_SCREEN_WIDTH * DS_BOTTOM_SCREEN_HEIGHT * sizeof(uint16_t));
}