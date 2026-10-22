#ifndef SIMCITY_3DS_RENDER_NATIVE_H
#define SIMCITY_3DS_RENDER_NATIVE_H

#include <stdint.h>

/* Native GPU render: reimplements the SNES PPU tile-rendering pipeline
 * entirely on the 3DS GPU (citro3d).  Eliminates the CPU-side
 * sc_v28_render() pixel loop entirely. */

/* Return 0 on success, -1 if GPU init failed. */
int ds3_render_native_init(void);
void ds3_render_native_exit(void);

/* Render one frame from the current PPU state.
 * Called after simcity_recomp_render_current_frame has updated the
 * runtime's VRAM/CGRAM/OAM/MMIO. */
int ds3_render_native_frame(void);

#endif
