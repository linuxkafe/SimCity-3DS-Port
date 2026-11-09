/*
 * 3ds_render_gpu.c — GPU-accelerated rendering using citro3d.
 *
 * Uploads the software-rendered BGR555 frame as an RGB565 texture and
 * renders a fullscreen quad. This eliminates the per-pixel framebuffer
 * write from the CPU, but the PPU emulation (sc_v28_render) still runs
 * on the CPU. Full GPU render (tile-based shader) is a future optimization.
 *
 * NOTE: Requires shader compilation (vshader.v.pica -> vshader.shbin).
 * Add to Makefile/CMakeLists: picasso -o vshader.shbin vshader.v.pica
 */

#include "3ds_render.h"
#include "3dsdbg.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

/* Shader binary (compiled from vshader.v.pica) */
#include "vshader_shbin.h"

static C3D_RenderTarget *s_target = NULL;
static C3D_Tex s_tex;
static DVLB_s *s_vshader_dvlb = NULL;
static shaderProgram_s s_program;
static int s_uLoc_projection;
static C3D_Mtx s_projection;
static bool s_gpu_ready = false;

static const float s_quad[] = {
    /* x, y, z, u, v */
    -1.0f, -1.0f, 0.5f, 0.0f, 1.0f,
     1.0f, -1.0f, 0.5f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.5f, 0.0f, 0.0f,
     1.0f,  1.0f, 0.5f, 1.0f, 0.0f,
};

static void gpu_init(void) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    s_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(s_target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    if (!C3D_TexInit(&s_tex, 256, 240, GPU_RGB565)) {
        printf("[gpu] TexInit failed\n");
        return;
    }
    C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_NEAREST);

    s_vshader_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
    if (!s_vshader_dvlb) {
        printf("[gpu] shader parse failed\n");
        return;
    }
    shaderProgramInit(&s_program);
    shaderProgramSetVsh(&s_program, &s_vshader_dvlb->DVLE[0]);
    C3D_BindProgram(&s_program);

    // AttrInfo: v0=position (3 floats), v1=texcoord (2 floats)
    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);

    Mtx_OrthoTilt(&s_projection, -1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f, true);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

    s_uLoc_projection = shaderInstanceGetUniformLocation(s_program.vertexShader, "projection");

    s_gpu_ready = true;
    printf("[gpu] initialized\n");
}

void ds3_render_init_gpu(void) {
    gpu_init();
}

void ds3_render_frame_gpu(const uint16_t *src,
                         unsigned src_width,
                         unsigned src_height,
                         unsigned crop_top,
                         unsigned crop_left,
                         unsigned crop_right) {
    if (!s_gpu_ready || !src) return;

    // Convert BGR555 -> RGB565 into temp buffer
    static uint16_t rgb_buf[256 * 240];
    unsigned i, n = src_width * src_height;
    if (n > 256 * 240) n = 256 * 240;
    for (i = 0; i < n; i++) {
        uint16_t c = src[i];
        uint8_t r5 = (uint8_t)(c & 0x1Fu);
        uint8_t g6 = (uint8_t)(((c >> 5) & 0x1Fu) * 63u / 31u);
        uint8_t b5 = (uint8_t)((c >> 10) & 0x1Fu);
        rgb_buf[i] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }

    // Upload to texture
    C3D_TexUpload(&s_tex, rgb_buf);
    C3D_TexBind(0, &s_tex);

    // Render
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(s_target);

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uLoc_projection, &s_projection);

    C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
    for (i = 0; i < 4; i++) {
        C3D_ImmSendAttrib(s_quad[i*5+0], s_quad[i*5+1], s_quad[i*5+2], 0.0f);
        C3D_ImmSendAttrib(s_quad[i*5+3], s_quad[i*5+4], 0.0f, 0.0f);
    }
    C3D_ImmDrawEnd();

    C3D_FrameEnd(0);
}

void ds3_render_exit_gpu(void) {
    if (s_vshader_dvlb) {
        shaderProgramFree(&s_program);
        DVLB_Free(s_vshader_dvlb);
    }
    C3D_TexDelete(&s_tex);
    if (s_target) C3D_RenderTargetDelete(s_target);
    C3D_Fini();
}
