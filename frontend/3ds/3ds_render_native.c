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
static C3D_Tex           s_target_tex;
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
static uint16_t cgram_to_rgb565(uint16_t raw) {
    uint8_t r =  raw & 0x1Fu;
    uint8_t g = (raw >> 5) & 0x1Fu;
    uint8_t b = (raw >> 10) & 0x1Fu;
    uint8_t g6 = (g << 1) | (g >> 4);
    return (uint16_t)((r << 11) | (g6 << 5) | b);
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
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    if (!C3D_TexInitVRAM(&s_target_tex, 240, 400, GPU_RGB565)) { printf("[native] target TexInitVRAM failed\n"); return -1; }
    C3D_TexSetFilter(&s_target_tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&s_target_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    s_target = C3D_RenderTargetCreateFromTex(&s_target_tex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
    if (!s_target) { printf("[native] RenderTargetCreateFromTex failed\n"); return -1; }
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
    printf("[atlas] GPU tile-render initialized\n");
    return 0;
}

/* ── Frame render ── */
int ds3_render_native_frame(void) {
    if (!s_gpu_ready || !g_recomp) return -1;

    /* Read camera */
    uint8_t wram_x[2], wram_y[2], wram_fx[1], wram_fy[1];
    simcity_recomp_read_wram(g_recomp, SC_CITY_VIEW_X_OFFSET, wram_x, 2);
    simcity_recomp_read_wram(g_recomp, SC_CITY_VIEW_Y_OFFSET, wram_y, 2);
    simcity_recomp_read_wram(g_recomp, SC_CITY_FINE_X_OFFSET, wram_fx, 1);
    simcity_recomp_read_wram(g_recomp, SC_CITY_FINE_Y_OFFSET, wram_fy, 1);

    int city_view_x = (int16_t)(wram_x[0] | (wram_x[1] << 8));
    int city_view_y = (int16_t)(wram_y[0] | (wram_y[1] << 8));
    int fine_x = wram_fx[0] & 7;
    int fine_y = wram_fy[0] & 7;

    int native_cam_x = city_view_x * 8 + fine_x;
    int native_cam_y = city_view_y * 8 + fine_y;

    /* Read PPU registers */
    uint8_t mmio[64];
    simcity_recomp_read_mmio(g_recomp, 0, mmio, 64);

    uint8_t bg1sc = mmio[MMIO_BG1SC];
    uint8_t bg2sc = mmio[MMIO_BG2SC];
    uint8_t tm    = mmio[MMIO_TM];
    uint8_t inidisp = mmio[MMIO_INIDISP];

    uint16_t bg12nba;
    {
        uint8_t tmp[2];
        simcity_recomp_read_mmio(g_recomp, MMIO_BG12NB, tmp, 2);
        bg12nba = (uint16_t)(tmp[0] | (tmp[1] << 8));
    }

    uint32_t bg1_tm_base   = (uint32_t)(bg1sc & 0x03u) << 10u;
    uint32_t bg2_tm_base   = (uint32_t)(bg2sc & 0x03u) << 10u;
    uint32_t bg1_char_base = (uint32_t)(bg12nba & 0x0Fu) << 12u;
    uint32_t bg2_char_base = (uint32_t)((bg12nba >> 4u) & 0x0Fu) << 12u;

    /* Read VRAM + CGRAM */
    static uint8_t vram[SC_VRAM_SIZE];
    static uint8_t cgram[SC_CGRAM_SIZE];
    simcity_recomp_read_vram(g_recomp, 0, vram, SC_VRAM_SIZE);
    simcity_recomp_read_cgram(g_recomp, 0, cgram, SC_CGRAM_SIZE);

    /* Build palette LUT */
    static uint16_t palette_lut[128];
    for (unsigned pi = 0; pi < 128; ++pi)
        palette_lut[pi] = cgram_to_rgb565((uint16_t)(cgram[pi*2] | (cgram[pi*2+1] << 8)));

    s_atlas.frame++;

    /* Forced blank */
    if (inidisp & 0x80u) {
        memset(s_tex_buf, 0, TEX_WIDTH * TEX_HEIGHT * 2);
        memcpy(s_frame_tex.data, s_tex_buf, TEX_WIDTH * TEX_HEIGHT * 2);
        C3D_TexUpload(&s_frame_tex, s_tex_buf);
        C3D_FrameBegin(0);
        C3D_FrameDrawOn(s_target);
        C3D_RenderTargetClear(s_target, C3D_CLEAR_COLOR, 0, 0);
        /* render tex as fullscreen quad */
        C3D_TexBind(0, &s_frame_tex);
        C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
        C3D_ImmSendAttrib(-1.0f, -1.0f, 0.5f, 0.0f);
        C3D_ImmSendAttrib(0.0f, 0.0f, 0.0f, 0.0f);
        C3D_ImmSendAttrib( 1.0f, -1.0f, 0.5f, 1.0f);
        C3D_ImmSendAttrib(1.0f, 0.0f, 0.0f, 0.0f);
        C3D_ImmSendAttrib(-1.0f,  1.0f, 0.5f, 0.0f);
        C3D_ImmSendAttrib(0.0f, 1.0f, 0.0f, 0.0f);
        C3D_ImmSendAttrib( 1.0f,  1.0f, 0.5f, 1.0f);
        C3D_ImmSendAttrib(1.0f, 1.0f, 0.0f, 0.0f);
        C3D_ImmDrawEnd();
        C3D_FrameEnd(GX_CMDLIST_FLUSH);
        gfxFlushBuffers();
        return 1;
    }

    /* Pre-assemble frame into texture */
    memset(s_tex_buf, 0, TEX_WIDTH * TEX_HEIGHT * 2);

    int max_map_x = SC_CITY_MAP_WIDTH - 1;
    int max_map_y = SC_CITY_MAP_HEIGHT - 1;

    for (unsigned y = 0; y < OUTPUT_HEIGHT; ++y) {
        uint16_t *row = &s_tex_buf[y * TEX_WIDTH];
        int world_y = native_cam_y + (int)y;
        int map_y_raw = world_y >> 3;
        int eff_cell_y = world_y & 7;

        for (unsigned x = 0; x < OUTPUT_WIDTH; ++x) {
            int world_x = native_cam_x + (int)x;
            int map_x = world_x >> 3;
            int cell_x = world_x & 7;

            uint16_t pixel = 0;

            /* BG2 (terrain) */
            if (tm & 0x04u) {
                if (map_x >= 0 && map_x <= max_map_x &&
                    map_y_raw >= 0 && map_y_raw <= max_map_y) {
                    uint32_t tm_addr = bg2_tm_base + ((uint32_t)map_y_raw * 64u + (uint32_t)(unsigned)map_x) * 2u;
                    uint16_t entry = vram_read16(vram, tm_addr >> 1);
                    uint16_t tile = entry & 0x03FFu;
                    uint8_t pal_group = (uint8_t)((entry >> 10) & 7u);
                    uint8_t flip_x = (entry >> 14) & 1u;
                    uint8_t flip_y = (entry >> 15) & 1u;

                    int eff_px = flip_x ? (7 - cell_x) : cell_x;
                    int eff_py = flip_y ? (7 - eff_cell_y) : eff_cell_y;

                    /* Direct tile decode (no atlas cache) */
                    {
                        uint32_t row_addr_base = bg2_char_base + tile * 16u;
                        uint8_t planes[2];
                        unsigned src_py = flip_y ? (7 - eff_cell_y) : eff_cell_y;
                        uint32_t row_addr = row_addr_base + src_py * 2u;
                        planes[0] = vram[row_addr & (SC_VRAM_SIZE - 1u)];
                        planes[1] = vram[(row_addr + 1u) & (SC_VRAM_SIZE - 1u)];
                        unsigned src_px = flip_x ? (7 - cell_x) : cell_x;
                        uint8_t color = (uint8_t)(((planes[0] >> (7 - src_px)) & 1u) |
                                                  (((planes[1] >> (7 - src_px)) & 1u) << 1));
                        if (color) {
                            unsigned pal_idx = pal_group * 4u + color;
                            uint16_t tp = palette_lut[pal_idx & 127u];
                            if (tp) pixel = tp;
                        }
                    }
                }
            }

            /* BG1 (buildings) on top */
            if (tm & 0x02u) {
                if (map_x >= 0 && map_x <= max_map_x &&
                    map_y_raw >= 0 && map_y_raw <= max_map_y) {
                    uint32_t tm_addr = bg1_tm_base + ((uint32_t)map_y_raw * 64u + (uint32_t)(unsigned)map_x) * 2u;
                    uint16_t entry = vram_read16(vram, tm_addr >> 1);
                    uint16_t tile = entry & 0x03FFu;
                    uint8_t pal_group = (uint8_t)((entry >> 10) & 7u);
                    uint8_t flip_x = (entry >> 14) & 1u;
                    uint8_t flip_y = (entry >> 15) & 1u;

                    int eff_px = flip_x ? (7 - cell_x) : cell_x;
                    int eff_py = flip_y ? (7 - eff_cell_y) : eff_cell_y;

                    /* Direct tile decode (no atlas cache) */
                    {
                        uint32_t row_addr_base = bg1_char_base + tile * 16u;
                        uint8_t planes[2];
                        unsigned src_py = flip_y ? (7 - eff_cell_y) : eff_cell_y;
                        uint32_t row_addr = row_addr_base + src_py * 2u;
                        planes[0] = vram[row_addr & (SC_VRAM_SIZE - 1u)];
                        planes[1] = vram[(row_addr + 1u) & (SC_VRAM_SIZE - 1u)];
                        unsigned src_px = flip_x ? (7 - cell_x) : cell_x;
                        uint8_t color = (uint8_t)(((planes[0] >> (7 - src_px)) & 1u) |
                                                  (((planes[1] >> (7 - src_px)) & 1u) << 1));
                        if (color) {
                            unsigned pal_idx = pal_group * 4u + color;
                            uint16_t tp = palette_lut[pal_idx & 127u];
                            if (tp) pixel = tp;
                        }
                    }
                }
            }

            row[x] = pixel;
        }
    }

    /* Upload texture */
    memcpy(s_frame_tex.data, s_tex_buf, TEX_WIDTH * TEX_HEIGHT * 2);
    C3D_TexUpload(&s_frame_tex, s_tex_buf);

    /* Render fullscreen quad */
    C3D_FrameBegin(0);
    C3D_FrameDrawOn(s_target);
    C3D_RenderTargetClear(s_target, C3D_CLEAR_COLOR, 0x0000FFFF, 0);  /* Yellow test */

    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    gfxFlushBuffers();
    return 1;
}

/* ── Exit ── */
void ds3_render_native_exit(void) {
    if (s_tex_buf) linearFree(s_tex_buf);
    if (s_shader) DVLB_Free(s_shader);
    shaderProgramFree(&s_shaderProgram);
    C3D_TexDelete(&s_target_tex);
    C3D_TexDelete(&s_frame_tex);
    if (s_target) C3D_RenderTargetDelete(s_target);
    C3D_Fini();
    s_gpu_ready = false;
}
