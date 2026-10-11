/*
 * 3ds_render.h — top/bottom screen rendering helpers using libctru legacy framebuffer.
 */

#ifndef SIMCITY_3DS_RENDER_H
#define SIMCITY_3DS_RENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the citro2d rendering system.
 * Must be called before any rendering.
 */
void ds3_render_init(void);

/*
 * Shutdown the citro2d rendering system.
 */
void ds3_render_exit(void);

/*
 * Render one frame to the 3DS top screen.
 *
 * src: core-emitted 32-bit BGRA pixels, width × height.
 * src_width, src_height: dimensions of the frame buffer.
 * crop_top: number of scanlines to skip from the top (to fit 240px height).
 * crop_left, crop_right: horizontal margin to crop on each side (for aspect
 *                        ratio correction; 0 for pixel-perfect).
 */
void ds3_render_frame(const uint32_t *src,
                      unsigned src_width,
                      unsigned src_height,
                      unsigned crop_top,
                      unsigned crop_left,
                      unsigned crop_right);

/*
 * Draw diagnostic / HUD info to the bottom screen.
 */
void ds3_render_hud(uint64_t instruction_count,
                    uint64_t master_clock,
                    uint32_t frame_num,
                    const char *error_msg);

/*
 * Clear both screens to black.
 */
void ds3_render_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* SIMCITY_3DS_RENDER_H */