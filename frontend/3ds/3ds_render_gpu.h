#ifndef SIMCITY_3DS_RENDER_GPU_H
#define SIMCITY_3DS_RENDER_GPU_H

#include <stdint.h>

void ds3_render_init_gpu(void);
void ds3_render_frame_gpu(const uint16_t *src,
                         unsigned src_width,
                         unsigned src_height,
                         unsigned crop_top,
                         unsigned crop_left,
                         unsigned crop_right);
void ds3_render_exit_gpu(void);

#endif
