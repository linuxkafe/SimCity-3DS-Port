/*
 * ds_render.h — top/bottom screen rendering helpers.
 *
 * The core emits 256x239 (or 398x239 widescreen) BGRA frames. The DS top
 * screen is 256x192 bitmap mode 4. We render a cropped view (top-192 lines)
 * with the HUD overlay on the bottom screen.
 */

#ifndef SIMCITY_DS_RENDER_H
#define SIMCITY_DS_RENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Render one frame to the DS top screen.
 *
 * src: core-emitted 32-bit BGRA pixels, width × height.
 * src_width, src_height: dimensions of the frame buffer.
 * crop_top: number of scanlines to skip from the top (to fit 192px height).
 * crop_left, crop_right: horizontal margin to crop on each side (for aspect
 *                        ratio correction; 0 for pixel-perfect).
 */
void ds_render_frame(const uint32_t *src,
                     unsigned src_width,
                     unsigned src_height,
                     unsigned crop_top,
                     unsigned crop_left,
                     unsigned crop_right);

/*
 * Draw diagnostic / HUD info to the bottom screen.
 */
void ds_render_hud(uint64_t instruction_count,
                   uint64_t master_clock,
                   uint32_t frame_num,
                   const char *error_msg);

/*
 * Clear both screens to black.
 */
void ds_render_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* SIMCITY_DS_RENDER_H */