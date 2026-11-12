/*
 * 3ds_render_native.c — Native GPU tile-based rendering for SimCity 3DS.
 *
 * Uses a tile atlas (Zelda3-style) to cache decoded 8x8 tiles.  The CPU
 * assembles the full frame into a texture using the atlas for fast tile
 * lookup, then the GPU renders a single fullscreen quad.  This keeps the
 * simple, crash-free fullscreen-quad path while gaining the atlas cache
 * (avoids re-decoding identical tiles every frame).
 */

#include "3ds_render_native.h"
#include "simcity_static_recomp.h"
#include "3dsdbg.h"

#include <3ds.h>
#include <citro3d.h>
#include "vshader_shbin.h"
#include <3ds/gfx.h>
#include <string.h>
#include <stdio.h>

/* ── Constants ── */
#define OUTPUT_WIDTH       256u
#define OUTPUT_HEIGHT      239u
#define TEX_WIDTH          256u
#define TEX_HEIGHT         256u
#define ATLAS_SIZE         128u   /* 16 tiles x 8 px */
#define ATLAS_TILES        16u
#define ATLAS_SLOTS        256u
#define ATLAS_HASH         2048u
#define SC_VRAM_SIZE       65536u
#define SC_CGRAM_SIZE      512u
#define SC_CITY_MAP_WIDTH  120u
#define SC_CITY_MAP_HEIGHT 100u
#define SC_CITY_VIEW_X_OFFSET  0x01bdu
#define SC_CITY_VIEW_Y_OFFSET  0x01bfu
#define SC_CITY_FINE_X_OFFSET  0x0139u
#define SC_CITY_FINE_Y_OFFSET  0x0137u

#define MMIO_INIDISP  0x00u
#define MMIO_BG1SC    0x07u
#define MMIO_BG2SC    0x08u
#define MMIO_BG12NB   0x0Bu
#define MMIO_TM       0x2Cu

/* ── Atlas ── */
typedef struct {
    uint32_t key;
    uint16_t pixels[64];
    bool     valid;
    uint32_t used;
} AtlasTile;

typedef struct {
    AtlasTile tiles[ATLAS_SLOTS];
    uint16_t  hash[ATLAS_HASH];
    uint32_t  frame;
} Atlas;

static Atlas s_atlas;

/* ── GPU state ── */
static C3D_RenderTarget *s_target = NULL;
static C3D_Tex           s_frame_tex;
static uint16_t         *s_tex_buf = NULL;
static bool              s_gpu_ready = false;
static DVLB_s           *s_shader = NULL;
static shaderProgram_s   s_shaderProgram;
static C3D_AttrInfo     s_attrInfo;
static int              s_projLocation = -1;
extern SimCityRecomp    *g_recomp;

/* ── Helpers ── */
static inline uint8_t vram_read8(const uint8_t *vram, uint32_t byte_addr) {
    return vram[byte_addr & (SC_VRAM_SIZE - 1u)];
}
static uint16_t vram_read16(const uint8_t *vram, uint32_t word_addr) {
    uint32_t ba = word_addr << 1;
    return (uint16_t)(vram[ba & (SC_VRAM_SIZE - 1u)] |
                      (vram[(ba + 1u) & (SC_VRAM_SIZE - 1u)] << 8));
}
static inline uint8_t decode_2bpp(const uint8_t *planes, unsigned bit) {
    return (uint8_t)(((planes[0] >> bit) & 1u) |
                     (((planes[1] >> bit) & 1u) << 1));
}
static const uint8_t g_widen6[32] = {
      0,   2,   4,   6,   8,  10,  12,  14,  16,  18,  20,  22,  24,  26,
     28,  30,  32,  34,  37,  39,  41,  43,  45,  47,  49,  51,  53,  55,
     57,  59,  61,  63
};

static uint16_t cgram_to_rgb565(uint16_t raw) {
    uint8_t r5 = (uint8_t)(raw & 0x1Fu);         /* core's low field   */
    uint8_t g5 = (uint8_t)((raw >> 5) & 0x1Fu);  /* five bits of green */
    uint8_t b5 = (uint8_t)((raw >> 10) & 0x1Fu); /* core's high field  */
    uint8_t g6 = g_widen6[g5];
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* ── Atlas hash ── */
static uint32_t atlas_hash(uint32_t key) { return (key * 2654435761u) >> 11; }

/* Returns slot index (>=0) or -1 on failure */
static int atlas_decode(Atlas *a, const uint8_t *vram,
                       uint32_t tile_idx, uint8_t pal_group, uint32_t char_base,
                       uint8_t flip_x, uint8_t flip_y,
                       const uint16_t *palette_lut) {
    uint32_t key = tile_idx | ((uint32_t)pal_group << 10) | (char_base >> 8);
    unsigned h = atlas_hash(key);
    while (a->hash[h] && a->tiles[a->hash[h]].key != key)
        h = (h + 1) & (ATLAS_HASH - 1);
    unsigned slot = a->hash[h];

    if (!slot) {
        /* Find free slot (LRU) */
        unsigned best = 1, best_frame = a->tiles[1].used;
        for (unsigned i = 2; i < ATLAS_SLOTS; i++) {
            if (!a->tiles[i].valid) { best = i; break; }
            if (a->tiles[i].used < best_frame) { best = i; best_frame = a->tiles[i].used; }
        }
        slot = best;
        if (a->tiles[slot].valid) {
            uint32_t old_key = a->tiles[slot].key;
            unsigned hh = atlas_hash(old_key);
            while (a->hash[hh] && a->tiles[a->hash[hh]].key != old_key)
                hh = (hh + 1) & (ATLAS_HASH - 1);
            if (a->hash[hh]) a->hash[hh] = 0;
        }
        h = atlas_hash(key);
        while (a->hash[h]) h = (h + 1) & (ATLAS_HASH - 1);
        a->hash[h] = slot;
        a->tiles[slot].valid = false;
        a->tiles[slot].key = key;
    }

    AtlasTile *t = &a->tiles[slot];
    t->used = a->frame;

    if (t->valid) return (int)slot;

    /* Decode 2bpp tile */
    t->valid = true;
    uint32_t row_addr_base = char_base + tile_idx * 16u;
    for (unsigned py = 0; py < 8; py++) {
        unsigned src_py = flip_y ? (7 - py) : py;
        uint32_t row_addr = row_addr_base + src_py * 2u;
        uint8_t planes[2] = { vram_read8(vram, row_addr),
                              vram_read8(vram, row_addr + 1u) };
        for (unsigned px = 0; px < 8; px++) {
            unsigned src_px = flip_x ? (7 - px) : px;
            uint8_t color = decode_2bpp(planes, 7u - src_px);
            unsigned idx = py * 8 + px;
            if (color) {
                unsigned pal_idx = pal_group * 4u + color;
                t->pixels[idx] = palette_lut[pal_idx & 127u];
            } else {
                t->pixels[idx] = 0;
            }
        }
    }
    return (int)slot;
}

/* ── GPU init ── */
int ds3_render_native_init(void) {
    FILE *tf = fopen("sdmc:/native_init_trace.log", "w");
    if (tf) { fprintf(tf, "init: start\n"); fflush(tf); }
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    if (tf) { fprintf(tf, "init: C3D_Init done\n"); fflush(tf); }

    s_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGB565, GPU_RB_DEPTH24_STENCIL8);
    if (!s_target) {
        if (tf) { fprintf(tf, "init: RenderTargetCreate FAILED\n"); fflush(tf); fclose(tf); }
        printf("[atlas] RenderTargetCreate failed\n");
        return -1;
    }
    if (tf) { fprintf(tf, "init: RenderTargetCreate OK\n"); fflush(tf); }
    C3D_RenderTargetSetOutput(s_target, GFX_TOP, GFX_LEFT,
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    if (!C3D_TexInit(&s_frame_tex, TEX_WIDTH, TEX_HEIGHT, GPU_RGB565)) {
        printf("[atlas] TexInit failed\n");
        return -1;
    }
    C3D_TexSetFilter(&s_frame_tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&s_frame_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    /* Load vertex shader */
    s_shader = DVLB_ParseFile((u32 *)vshader_shbin, vshader_shbin_size);
    if (!s_shader || !s_shader->numDVLE) {
        printf("[native] DVLB_ParseFile failed\n");
        return -1;
    }
    if (R_FAILED(shaderProgramInit(&s_shaderProgram))) {
        printf("[native] shaderProgramInit failed\n");
        return -1;
    }
    if (R_FAILED(shaderProgramSetVsh(&s_shaderProgram, &s_shader->DVLE[0]))) {
        printf("[native] shaderProgramSetVsh failed\n");
        return -1;
    }
    s_projLocation = shaderInstanceGetUniformLocation(s_shaderProgram.vertexShader, "projection");
    if (s_projLocation < 0) {
        printf("[native] projection uniform not found\n");
        return -1;
    }

    /* Set up attribute info: position (3 floats) + texcoord (2 floats) */
    AttrInfo_Init(&s_attrInfo);
    AttrInfo_AddLoader(&s_attrInfo, 0, GPU_FLOAT, 3);  /* position */
    AttrInfo_AddLoader(&s_attrInfo, 1, GPU_FLOAT, 2);  /* texcoord */

    s_tex_buf = (uint16_t *)linearAlloc(TEX_WIDTH * TEX_HEIGHT * 2);
    if (!s_tex_buf) {
        printf("[native] linearAlloc failed\n");
        return -1;
    }
    memset(&s_atlas, 0, sizeof(s_atlas));

    s_gpu_ready = true;
    if (tf) { fprintf(tf, "init: SUCCESS, s_gpu_ready=1\n"); fflush(tf); fclose(tf); }
    printf("[atlas] GPU tile-render initialized\n");
    return 0;
}

/* ── Frame render ── */
int ds3_render_native_frame(void) {
    if (!s_gpu_ready || !g_recomp) return -1;

    const uint16_t *frame = simcity_recomp_frame_bgr555(g_recomp);
    unsigned fw = simcity_recomp_frame_width(g_recomp);
    unsigned fh = SIMCITY_RECOMP_FRAME_HEIGHT;

    if (!frame || fw == 0 || fh == 0) return -1;

    u16 fb_w = 0, fb_h = 0;
    uint16_t *fb = (uint16_t *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fb_w, &fb_h);
    if (!fb) return -1;
    if (fb_w == 0) fb_w = 240;
    if (fb_h == 0) fb_h = 400;

    unsigned crop_top = 0;
    unsigned crop_left = 0;
    unsigned crop_right = 0;
    unsigned rows = (crop_top < fh) ? (fh - crop_top) : 0u;
    unsigned row_pixels = (crop_left + crop_right < fw)
                        ? (fw - crop_left - crop_right) : 0u;
    if (rows == 0u || row_pixels == 0u) return -1;

    const uint16_t *row_src = frame + (size_t)crop_top * fw + crop_left;

    unsigned dst_h = row_pixels;
    unsigned dst_w = rows;
    if (dst_w > fb_w) dst_w = fb_w;
    if (dst_h > fb_h) dst_h = fb_h;

    unsigned off_x = (fb_w - dst_w) / 2u;
    unsigned off_y = (fb_h - dst_h) / 2u;

    memset(fb, 0, (size_t)fb_w * fb_h * sizeof(uint16_t));
    memset(s_tex_buf, 0, TEX_WIDTH * TEX_HEIGHT * 2);

    /* 180-degree rotation: flip both axes */
    for (unsigned dy = 0; dy < dst_h; dy++) {
        for (unsigned dx = 0; dx < dst_w; dx++) {
            unsigned sy = rows - 1u - dx;
            unsigned sx = row_pixels - 1u - dy;
            if (sy >= rows)   sy = rows - 1u;
            if (sx >= row_pixels) sx = row_pixels - 1u;
            uint16_t pixel = row_src[(size_t)sy * row_pixels + sx];
            uint8_t lo  = (uint8_t)(pixel & 0x1Fu);
            uint8_t g5  = (uint8_t)((pixel >> 5) & 0x1Fu);
            uint8_t hi  = (uint8_t)((pixel >> 10) & 0x1Fu);
            uint8_t g6  = g_widen6[g5];
            uint16_t rgb565 = (uint16_t)((lo << 11) | (g6 << 5) | hi);
            fb[(size_t)(off_y + dy) * fb_w + (off_x + dx)] = rgb565;
        }
    }

    gfxFlushBuffers();
    return 1;
}

/* ── Exit ── */
void ds3_render_native_exit(void) {
    if (s_tex_buf) linearFree(s_tex_buf);
    if (s_shader) DVLB_Free(s_shader);
    shaderProgramFree(&s_shaderProgram);
    C3D_TexDelete(&s_frame_tex);
    if (s_target) C3D_RenderTargetDelete(s_target);
    C3D_Fini();
    s_gpu_ready = false;
}
