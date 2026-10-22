/*
 * 3ds_render_native.c — Native GPU tile-based rendering for SimCity 3DS.
 *
 * Replaces the CPU-side sc_v28_render() pixel loop with a GPU-accelerated
 * pipeline: the CPU pre-assembles visible tilemap pixels into a texture
 * (simple tile lookup + palette indexing, no per-pixel priority/color-math),
 * then the GPU renders it as a fullscreen quad. This eliminates the dominant
 * per-pixel PPU emulation cost.
 */

#include "3ds_render_native.h"
#include "simcity_static_recomp.h"

#include <3ds.h>
#include <citro3d.h>
#include <string.h>
#include <stdio.h>

/* SNES PPU constants */
#define SC_VRAM_SIZE       65536u
#define SC_CGRAM_SIZE      512u
#define SC_OAM_SIZE        544u
#define SC_CITY_MAP_WIDTH  120u
#define SC_CITY_MAP_HEIGHT 100u
#define SC_CITY_VIEW_X_OFFSET  0x01bdu
#define SC_CITY_VIEW_Y_OFFSET  0x01bfu
#define SC_CITY_FINE_X_OFFSET  0x0139u
#define SC_CITY_FINE_Y_OFFSET  0x0137u

/* Render geometry — match the 3DS top screen (256×240 native, but SNES
   visible area is 256×224 with borders). */
#define OUTPUT_WIDTH       256u
#define OUTPUT_HEIGHT      239u

/* Texture must be power-of-two on PICA200 */
#define TEX_WIDTH          256u
#define TEX_HEIGHT         256u

/* MMIO register offsets (relative to SC_MMIO_BASE = 0x2100) */
#define MMIO_INIDISP  0x00u
#define MMIO_BGMODE   0x05u
#define MMIO_BG1SC    0x07u
#define MMIO_BG2SC    0x08u
#define MMIO_BG3SC    0x09u
#define MMIO_BG4SC    0x0Au
#define MMIO_BG12NB   0x0Bu
#define MMIO_BG34NB   0x0Cu
#define MMIO_TM       0x2Cu
#define MMIO_TS       0x2Du
#define MMIO_W34SEL   0x2Fu
#define MMIO_CGWSel   0x30u
#define MMIO_CGADSUB  0x31u

/* Forward declaration — defined in 3ds_main.c */
extern SimCityRecomp *g_recomp;

/* ── Module state ── */
static C3D_RenderTarget *s_target = NULL;
static C3D_Tex s_frame_tex;
static bool s_gpu_ready = false;

/* VRAM read (word-addressed) */
static inline uint8_t vram_read8(const uint8_t *vram, uint32_t byte_addr) {
    return vram[byte_addr & (SC_VRAM_SIZE - 1u)];
}

static uint16_t vram_read16(const uint8_t *vram, uint32_t word_addr) {
    uint32_t byte_addr = word_addr << 1;
    return (uint16_t)(vram[byte_addr & (SC_VRAM_SIZE - 1u)] |
                      (vram[(byte_addr + 1u) & (SC_VRAM_SIZE - 1u)] << 8));
}

/* Decode 2bpp pixel from 2 plane bytes */
static inline uint8_t decode_2bpp(const uint8_t *planes, unsigned bit) {
    return (uint8_t)(((planes[0] >> bit) & 1u) |
                     (((planes[1] >> bit) & 1u) << 1));
}

/* Decode 4bpp pixel from 4 plane bytes */
static inline uint8_t decode_4bpp(const uint8_t *planes, unsigned bit) {
    return (uint8_t)(((planes[0] >> bit) & 1u) |
                     (((planes[1] >> bit) & 1u) << 1) |
                     (((planes[2] >> bit) & 1u) << 2) |
                     (((planes[3] >> bit) & 1u) << 3));
}

/* Convert SNES CGRAM color (9-bit BBGR) to RGB565 */
static uint16_t cgram_to_rgb565(uint16_t raw) {
    uint8_t r =  raw & 0x1Fu;
    uint8_t g = (raw >> 5) & 0x1Fu;
    uint8_t b = (raw >> 10) & 0x1Fu;
    /* Expand 5-bit to 5-bit (same), 5-bit green to 6-bit */
    uint8_t g6 = (g << 1) | (g >> 4);
    return (uint16_t)((r << 11) | (g6 << 5) | b);
}

/* ── GPU init ── */

int ds3_render_native_init(void) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    s_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGB565, GPU_RB_DEPTH24_STENCIL8);
    if (!s_target) {
        printf("[native] RenderTargetCreate failed\n");
        return -1;
    }
    C3D_RenderTargetSetOutput(s_target, GFX_TOP, GFX_LEFT,
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

    if (!C3D_TexInit(&s_frame_tex, TEX_WIDTH, TEX_HEIGHT, GPU_RGB565)) {
        printf("[native] TexInit failed\n");
        return -1;
    }
    C3D_TexSetFilter(&s_frame_tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&s_frame_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    s_gpu_ready = true;
    printf("[native] GPU tile-render initialized (%ux%u)\n", OUTPUT_WIDTH, OUTPUT_HEIGHT);
    return 0;
}

/* ── Pre-assemble frame ── */

/* Access the machine via the public accessor */
struct SCMachine;  /* forward */

static void assemble_frame_rgb565(const void *machine_ptr, uint16_t *out) {
    /* The machine pointer is SCMachine* from the upstream core.
     * We cast to a local struct definition matching the fields we need. */
    struct {
        uint32_t _pad0;
        /* We need: vram[65536], cgram[512], oam[544], mmio[64], wram[131072] */
        /* Offsets depend on SCMachine layout. Use accessor functions instead. */
    } *m = (void *)machine_ptr;

    /* Use public accessors for WRAM (camera) and raw access for the rest.
     * Since the 3DS port includes the internal headers, we can cast properly. */
    (void)m;

    /* Read WRAM for camera */
    int16_t city_view_x, city_view_y, fine_x, fine_y;

    uint8_t wram_x[2], wram_y[2], wram_fx[1], wram_fy[1];
    simcity_recomp_read_wram(g_recomp, SC_CITY_VIEW_X_OFFSET, wram_x, 2);
    simcity_recomp_read_wram(g_recomp, SC_CITY_VIEW_Y_OFFSET, wram_y, 2);
    simcity_recomp_read_wram(g_recomp, SC_CITY_FINE_X_OFFSET, &wram_fx, 1);
    simcity_recomp_read_wram(g_recomp, SC_CITY_FINE_Y_OFFSET, &wram_fy, 1);

    city_view_x = (int16_t)(wram_x[0] | (wram_x[1] << 8));
    city_view_y = (int16_t)(wram_y[0] | (wram_y[1] << 8));
    fine_x = wram_fx[0] & 7;
    fine_y = wram_fy[0] & 7;

    int native_cam_x = (int)city_view_x * 8 + (int)fine_x;
    int native_cam_y = (int)city_view_y * 8 + (int)fine_y;

    /* Read PPU registers */
    uint8_t mmio[64];
    simcity_recomp_read_mmio(g_recomp, 0, mmio, 64);

    uint8_t bgmode_reg   = mmio[MMIO_BGMODE];
    uint8_t bg1sc        = mmio[MMIO_BG1SC];
    uint8_t bg2sc        = mmio[MMIO_BG2SC];
    uint16_t bg12nba;
    {
        uint8_t tmp[2];
        simcity_recomp_read_mmio(g_recomp, MMIO_BG12NB, tmp, 2);
        bg12nba = (uint16_t)(tmp[0] | (tmp[1] << 8));
    }
    uint8_t tm           = mmio[MMIO_TM];
    uint8_t inidisp      = mmio[MMIO_INIDISP];

    unsigned mode = bgmode_reg & 7u;
    (void)mode;  /* SimCity always uses mode 0 or 1 */

    uint8_t bg3high = bgmode_reg & 8u;
    (void)bg3high;

    /* Tilemap base addresses (VRAM word indices) */
    uint32_t bg1_tm_base = (uint32_t)(bg1sc & 0x03u) << 10u;  /* ×0x400 bytes = ×0x800 words */
    uint32_t bg2_tm_base = (uint32_t)(bg2sc & 0x03u) << 10u;

    /* Pattern base addresses */
    uint32_t bg1_char_base = (uint32_t)(bg12nba & 0x0Fu) << 12u;  /* <<13 bytes = <<12 words */
    uint32_t bg2_char_base = (uint32_t)((bg12nba >> 4u) & 0x0Fu) << 12u;

    /* Read VRAM + CGRAM into local buffers (one bulk read) */
    uint8_t vram[SC_VRAM_SIZE];
    uint8_t cgram[SC_CGRAM_SIZE];
    simcity_recomp_read_vram(g_recomp, 0, vram, SC_VRAM_SIZE);
    simcity_recomp_read_cgram(g_recomp, 0, cgram, SC_CGRAM_SIZE);

    /* Build palette LUT: 128 entries from CGRAM (2 bytes each) */
    uint16_t palette_lut[128];
    unsigned pi;
    for (pi = 0; pi < 128; ++pi) {
        palette_lut[pi] = cgram_to_rgb565((uint16_t)(cgram[pi*2] | (cgram[pi*2+1] << 8)));
    }

    /* For each output pixel, decode the tile pixel from the correct layer.
     * BG2 (terrain) is drawn first, BG1 (buildings) on top. */
    unsigned y, x;
    int max_map_x = SC_CITY_MAP_WIDTH - 1;
    int max_map_y = SC_CITY_MAP_HEIGHT - 1;

    for (y = 0; y < OUTPUT_HEIGHT; ++y) {
        uint16_t *row = &out[y * TEX_WIDTH];
        int world_y = native_cam_y + (int)y;
        int map_y_raw = world_y >> 3;
        int eff_cell_y = world_y & 7;

        for (x = 0; x < OUTPUT_WIDTH; ++x) {
            int world_x = native_cam_x + (int)x;
            int map_x = world_x >> 3;
            int cell_x = world_x & 7;

            uint16_t pixel = 0;  /* default: color 0 (black/transparent) */

            /* BG2 (city terrain) */
            if (tm & 0x04u) {
                if (map_x >= 0 && map_x <= max_map_x &&
                    map_y_raw >= 0 && map_y_raw <= max_map_y) {

                    /* Read tilemap entry (16-bit) */
                    uint32_t tm_addr = bg2_tm_base + ((uint32_t)map_y_raw * 64u + (uint32_t)(unsigned)map_x) * 2u;
                    uint16_t entry = vram_read16(vram, tm_addr >> 1);
                    uint16_t tile = entry & 0x03FFu;
                    uint8_t pal_group = (uint8_t)((entry >> 10) & 7u);
                    uint8_t flip_x = (entry >> 14) & 1u;
                    uint8_t flip_y = (entry >> 15) & 1u;

                    int eff_px = flip_x ? (7 - cell_x) : cell_x;
                    int eff_py = flip_y ? (7 - eff_cell_y) : eff_cell_y;

                    /* 2bpp tile: 2 bytes per row */
                    uint32_t row_addr = bg2_char_base + tile * 16u + (uint32_t)eff_py * 2u;
                    uint8_t planes[2] = { vram_read8(vram, row_addr),
                                          vram_read8(vram, row_addr + 1u) };
                    uint8_t color = decode_2bpp(planes, 7u - (unsigned)eff_px);

                    if (color) {
                        unsigned pal_idx = pal_group * 4u + color;
                        pixel = palette_lut[pal_idx & 127u];
                    }
                }
            }

            /* BG1 (city buildings/overlay) — drawn on top */
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

                    uint32_t row_addr = bg1_char_base + tile * 16u + (uint32_t)eff_py * 2u;
                    uint8_t planes[2] = { vram_read8(vram, row_addr),
                                          vram_read8(vram, row_addr + 1u) };
                    uint8_t color = decode_2bpp(planes, 7u - (unsigned)eff_px);

                    if (color) {
                        unsigned pal_idx = pal_group * 4u + color;
                        pixel = palette_lut[pal_idx & 127u];
                    }
                }
            }

            row[x] = pixel;
        }
    }

    /* Handle forced blank */
    if (inidisp & 0x80u) {
        for (y = 0; y < OUTPUT_HEIGHT; ++y)
            for (x = 0; x < OUTPUT_WIDTH; ++x)
                out[y * TEX_WIDTH + x] = 0;
    }
}

/* ── Frame render ── */

int ds3_render_native_frame(void) {
    if (!s_gpu_ready || !g_recomp) return -1;

    const void *machine = simcity_recomp_machine(g_recomp);
    if (!machine) return -1;

    /* Pre-assemble frame into texture buffer */
    static uint16_t tex_data[TEX_WIDTH * TEX_HEIGHT];
    memset(tex_data, 0, sizeof(tex_data));
    assemble_frame_rgb565(machine, tex_data);

    /* Upload to GPU */
    C3D_TexUpload(&s_frame_tex, tex_data);
    C3D_TexBind(0, &s_frame_tex);

    /* Render fullscreen quad on the top screen */
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_FrameDrawOn(s_target);

    C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
    /* Vertex: x, y, z | u, v */
    C3D_ImmSendAttrib(-1.0f, -1.0f, 0.5f, 0.0f);
    C3D_ImmSendAttrib(0.0f, 0.0f, 0.0f, 0.0f);
    C3D_ImmSendAttrib( 1.0f, -1.0f, 0.5f, 1.0f);
    C3D_ImmSendAttrib(1.0f, 0.0f, 0.0f, 0.0f);
    C3D_ImmSendAttrib(-1.0f,  1.0f, 0.5f, 0.0f);
    C3D_ImmSendAttrib(0.0f, 1.0f, 0.0f, 0.0f);
    C3D_ImmSendAttrib( 1.0f,  1.0f, 0.5f, 1.0f);
    C3D_ImmSendAttrib(1.0f, 1.0f, 0.0f, 0.0f);
    C3D_ImmDrawEnd();

    C3D_FrameEnd(0);
    return 1;
}

void ds3_render_native_exit(void) {
    C3D_TexDelete(&s_frame_tex);
    if (s_target) C3D_RenderTargetDelete(s_target);
    C3D_Fini();
    s_gpu_ready = false;
}
