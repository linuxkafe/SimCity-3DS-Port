/*
 * ds_main.c — Nintendo DS entry point (libnds / NitroSDK).
 *
 * Main loop:
 *   1. Initialise libnds (video, sound, touch).
 *   2. Load ROM from filesystem (first time only).
 *   3. Per-frame: read input → advance core → render frame → drain audio.
 *   4. On reset (SELECT+START+L+R), unload and reload ROM.
 */

#include "simcity_static_recomp.h"

#include "ds_main.h"
#include "ds_input.h"
#include "ds_render.h"

#include <nds.h>
#include <nds/arm9/video.h>
#include <nds/arm9/audio.h>
#include <nds/arm9/input.h>
#include <nds/arm9/touch.h>
#include <nds/arm9/fifo.h>
#include <nds/arm9/interupt.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Constants ─────────────────────────────────────────────────────── */

#define ROM_PATH "rom:/SimCity(USA).sfc"

/* Top screen bitmap mode 4 (16bpp): 256x192 = 49152 bytes per page */
#define DS_TOP_SCREEN_WIDTH  256u
#define DS_TOP_SCREEN_HEIGHT 192u
#define TOP_SCREEN_PIXELS (TOP_SCREEN_WIDTH * TOP_SCREEN_HEIGHT)

/* Core frame geometry. The SNES frame is 256x239; we crop top 0 + bottom 47
   to fit 192 visible lines on the top screen. Horizontal is 256 → 256. */
#define CORE_FRAME_CROP_TOP   24u   /* skip first 24 scanlines (title bar) */
#define CORE_FRAME_CROP_LEFT  0u
#define CORE_FRAME_CROP_RIGHT 0u

/* Bottom screen: 256x192 bitmap for HUD / debug text. */
#define DS_BOTTOM_SCREEN_WIDTH  256u
#define DS_BOTTOM_SCREEN_HEIGHT 192u

/* Audio: target 22050 Hz mono from 32040 Hz stereo core output.
   Downsample by ratio 32040/22050 ≈ 1.453 — use nearest-neighbor for now. */
#define DS_AUDIO_RATE     22050u
#define DS_AUDIO_CHANNELS 1u
#define DS_AUDIO_BITS     16u
#define AUDIO_RING_SIZE   (DS_AUDIO_RATE * 2 / DS_AUDIO_CHANNELS)

/* ── State ─────────────────────────────────────────────────────────── */

static SimCityRecomp *g_recomp = NULL;
static uint8_t        g_rom_loaded = 0;
static char           g_last_error[256] = "";

/* Soft pointer state (mimics Linux frontend's soft_mouse) */
static int g_soft_pointer = 1;
static int g_sp_x = 128, g_sp_y = 96;
static int g_sp_dx = 0, g_sp_dy = 0;
static int g_sp_valid = 0;

/* Audio ring buffer (simplified: just accumulate and play) */
static int16_t g_audio_ring[AUDIO_RING_SIZE];
static size_t  g_audio_write_pos = 0;
static size_t  g_audio_read_pos  = 0;

/* ── Audio sink callback ───────────────────────────────────────────── */
/* The core calls this for every produced sample pair. We accumulate into
   a ring buffer and let the DS audio system pull from it. */

static void ds_audio_sink(void *context, int16_t left, int16_t right) {
    /* Downsample stereo→mono by averaging. Ring buffer wraps around. */
    int16_t mono = (int16_t)((left + right) >> 1);
    size_t pos = g_audio_write_pos % AUDIO_RING_SIZE;
    g_audio_ring[pos] = mono;
    g_audio_write_pos++;
}

/* ── Core init / shutdown ─────────────────────────────────────────── */

int ds_init(const char *rom_path, char *out_error, size_t error_capacity) {
    if (!rom_path || !out_error) return 0;

    /* Load ROM from SD card */
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        snprintf(out_error, error_capacity,
                 "Cannot open ROM: %s", rom_path);
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long rom_size = ftell(f);
    rewind(f);

    if (rom_size <= 0 || rom_size > 0x100000) {
        /* SimCity SNES is 512 KB (0x80000); allow up to 1 MB. */
        fclose(f);
        snprintf(out_error, error_capacity, "Invalid ROM size: %ld", rom_size);
        return 0;
    }

    uint8_t *rom_data = (uint8_t *)malloc((size_t)rom_size);
    if (!rom_data) {
        fclose(f);
        snprintf(out_error, error_capacity, "Out of memory for ROM (%ld bytes)", rom_size);
        return 0;
    }
    size_t bytes_read = fread(rom_data, 1, (size_t)rom_size, f);
    fclose(f);
    if (bytes_read != (size_t)rom_size) {
        free(rom_data);
        snprintf(out_error, error_capacity, "Incomplete ROM read: %zu/%ld",
                 bytes_read, rom_size);
        return 0;
    }

    /* Create the static recompilation instance */
    char init_error[256];
    int rc = simcity_recomp_create(&g_recomp, rom_data, (size_t)rom_size,
                                    init_error, sizeof(init_error));
    if (rc != 1 || !g_recomp) {
        free(rom_data);
        snprintf(out_error, error_capacity, "simcity_recomp_create failed: %s",
                 init_error);
        return 0;
    }

    /* Register audio sink */
    simcity_recomp_set_audio_sink(g_recomp, ds_audio_sink, NULL);

    /* Open log if requested */
    simcity_recomp_log_open("rom:/simcity-ds.log", NULL, 0);

    /* Reset to cold boot state */
    char reset_err[256];
    if (!simcity_recomp_reset(g_recomp, reset_err, sizeof(reset_err))) {
        free(rom_data);
        snprintf(out_error, error_capacity, "simcity_recomp_reset failed: %s",
                 reset_err);
        return 0;
    }

    /* Free the ROM copy — the core already owns it. */
    free(rom_data);

    g_rom_loaded = 1;
    snprintf(out_error, error_capacity, "");
    return 1;
}

void ds_shutdown(void) {
    if (g_recomp) {
        simcity_recomp_log_close();
        simcity_recomp_destroy(g_recomp);
        g_recomp = NULL;
        g_rom_loaded = 0;
    }
}

const SimCityRecomp *ds_get_instance(void) {
    return g_recomp;
}

/* ── Input handling ────────────────────────────────────────────────── */

uint16_t ds_input_read(void) {
    swScanKeys();
    uint16_t mask = 0;

    if (swKeysHeld() & KEY_UP)    mask |= SIMCITY_INPUT_UP;
    if (swKeysHeld() & KEY_DOWN)  mask |= SIMCITY_INPUT_DOWN;
    if (swKeysHeld() & KEY_LEFT)  mask |= SIMCITY_INPUT_LEFT;
    if (swKeysHeld() & KEY_RIGHT) mask |= SIMCITY_INPUT_RIGHT;
    if (swKeysHeld() & KEY_A)     mask |= SIMCITY_INPUT_A;
    if (swKeysHeld() & KEY_B)     mask |= SIMCITY_INPUT_B;
    if (swKeysHeld() & KEY_X)     mask |= SIMCITY_INPUT_X;
    if (swKeysHeld() & KEY_Y)     mask |= SIMCITY_INPUT_Y;
    if (swKeysHeld() & KEY_L)     mask |= SIMCITY_INPUT_L;
    if (swKeysHeld() & KEY_R)     mask |= SIMCITY_INPUT_R;
    if (swKeysHeld() & KEY_START) mask |= SIMCITY_INPUT_START;
    if (swKeysHeld() & KEY_SELECT)mask |= SIMCITY_INPUT_SELECT;

    /* Reset hotkey: SELECT+START+L+R */
    if ((swKeysHeld() & (KEY_SELECT | KEY_START | KEY_L | KEY_R)) ==
        (KEY_SELECT | KEY_START | KEY_L | KEY_R)) {
        ds_shutdown();
        char dummy[256];
        ds_init(ROM_PATH, dummy, sizeof(dummy));
        g_soft_pointer = 1;
    }

    return mask;
}

void ds_input_set_soft_pointer(int enabled) {
    g_soft_pointer = enabled;
}

int ds_input_soft_pointer_enabled(void) {
    return g_soft_pointer;
}

void ds_input_update_soft_pointer(void) {
    if (!g_soft_pointer) return;

    touchPosition touch;
    swReadTouch(&touch);

    /* Map touch coordinates to the top screen coordinate space.
       The bottom screen is also 256x192; touch X/Y map directly. */
    int tx = (int)touch.px;
    int ty = (int)touch.py;

    if (tx < 0) tx = 0; else if (tx >= (int)DS_TOP_SCREEN_WIDTH) tx = DS_TOP_SCREEN_WIDTH - 1;
    if (ty < 0) ty = 0; else if (ty >= (int)DS_TOP_SCREEN_HEIGHT) ty = DS_TOP_SCREEN_HEIGHT - 1;

    if (!g_sp_valid) {
        g_sp_x = tx;
        g_sp_y = ty;
        g_sp_valid = 1;
        return;
    }

    g_sp_dx = tx - g_sp_x;
    g_sp_dy = ty - g_sp_y;
    g_sp_x  = tx;
    g_sp_y  = ty;
}

/* ── Frame advancement ────────────────────────────────────────────── */

SimCityRecompFrameResult ds_advance_frame(void) {
    SimCityRecompFrameResult result;
    memset(&result, 0, sizeof(result));

    if (!g_recomp) {
        snprintf(result.renderer_error, sizeof(result.renderer_error),
                 "Core not initialised — call ds_init() first.");
        return result;
    }

    uint16_t input = ds_input_read();
    ds_input_update_soft_pointer();

    /* Advance one SNES frame */
    int rc = simcity_recomp_advance(g_recomp, input, 1u, &result);
    if (rc != 1) {
        snprintf(result.renderer_error, sizeof(result.renderer_error),
                 "Advance failed (error=%d). %s", rc,
                 simcity_recomp_last_error(g_recomp));
    }

    /* Drain any available audio */
    size_t avail = simcity_recomp_audio_available(g_recomp);
    int16_t chunk[512];
    while (avail > 0) {
        size_t take = avail < 512u ? avail : 512u;
        size_t n = simcity_recomp_audio_read(g_recomp, chunk, take);
        avail -= n;
        /* Mix stereo→mono and push into ring buffer. */
        for (size_t i = 0; i < n; i++) {
            int16_t left  = (i % 2 == 0) ? chunk[i] : 0;
            int16_t right = (i % 2 == 1) ? chunk[i] : 0;
            int16_t mono = (int16_t)((left + right) >> 1);
            size_t pos = g_audio_write_pos % AUDIO_RING_SIZE;
            g_audio_ring[pos] = mono;
            g_audio_write_pos++;
        }
    }

    /* Render the current frame */
    const uint32_t *frame = simcity_recomp_frame_bgra(g_recomp);
    unsigned fw = simcity_recomp_frame_width(g_recomp);
    unsigned fh = SIMCITY_RECOMP_FRAME_HEIGHT;

    if (frame && fw > 0 && fh > 0 && result.frame_rendered) {
        ds_render_frame(frame, fw, fh,
                        CORE_FRAME_CROP_TOP,
                        CORE_FRAME_CROP_LEFT,
                        CORE_FRAME_CROP_RIGHT);
    }

    /* Render HUD on bottom screen */
    uint64_t instr_count = simcity_recomp_instruction_count(g_recomp);
    uint64_t master_clk  = simcity_recomp_master_clock(g_recomp);
    uint32_t frame_num   = simcity_recomp_current_frame(g_recomp);
    ds_render_hud(instr_count, master_clk, frame_num, result.renderer_error);

    return result;
}

/* ── Main entry point ─────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* Initialise libnds hardware */
    consoleInit(nullptr, BATT_FILE);

    /* Set up video: top screen mode 4 (16-bit bitmap), bottom screen mode 3 (256x192 bitmap). */
    videoSetMode(MODE_4 | DISPLAY_MAIN);
    videoSetModeSub(MODE_3 | DISPLAY_SUB);

    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);  /* Top screen BG */
    vramSetBankB(VRAM_B_MAIN_BG_0x06200000);  /* Not used yet */
    vramSetBankC(VRAM_C_SUB_BG_0x06400000);  /* Bottom screen BG */
    vramSetBankD(VRAM_D_SUB_BG_0x06500000);  /* Sub screen not needed, but reserve */

    /* Allocate frame buffer (top screen) — 256x192 @ 16bpp = 196608 bytes */
    uint16_t *top_screen = (uint16_t *)malloc(TOP_SCREEN_WIDTH * TOP_SCREEN_HEIGHT * sizeof(uint16_t));
    if (!top_screen) {
        iprintf("Fatal: cannot allocate top screen buffer.\n");
        return 1;
    }
    memset(top_screen, 0, TOP_SCREEN_WIDTH * TOP_SCREEN_HEIGHT * sizeof(uint16_t));

    /* Bottom screen buffer (256x192 @ 16bpp) */
    uint16_t *bottom_screen = (uint16_t *)malloc(BOTTOM_SCREEN_WIDTH * BOTTOM_SCREEN_HEIGHT * sizeof(uint16_t));
    if (!bottom_screen) {
        free(top_screen);
        iprintf("Fatal: cannot allocate bottom screen buffer.\n");
        return 1;
    }
    memset(bottom_screen, 0, BOTTOM_SCREEN_WIDTH * BOTTOM_SCREEN_HEIGHT * sizeof(uint16_t));

    /* Initialise input (key scan) */
    swInit();

    /* Initialise audio — software mixer, mono 22050 Hz */
    soundInit();
    soundSetMixerVolume(0x7F, 0); /* Master volume 0 — user must adjust */

    /* Print startup banner */
    iprintf("\x1b[1;1;37mSimCity DS\x1b[0m\n");
    iprintf("ROM: %s\n", ROM_PATH);
    iprintf("Top: 256x192 | Bot: 256x192\n");
    iprintf("Press A to start, SELECT+START+L+R to reset.\n");
    iprintf("\n");

    /* Initial core init */
    char init_err[256];
    if (!ds_init(ROM_PATH, init_err, sizeof(init_err))) {
        iprintf("INIT FAILED: %s\n", init_err);
        /* Wait for A press before exiting */
        while (1) { swScanKeys(); if (swKeysDown() & KEY_A) break; }
        return 0;
    }
    iprintf("Core initialised.\n");

    /* Main game loop */
    while (1) {
        /* Process input — A starts/steps, B pauses */
        uint16_t held = swKeysHeld();

        if (held & KEY_A) {
            /* Advance one frame */
            SimCityRecompFrameResult res = ds_advance_frame();
            if (res.route_continued == 0) {
                iprintf("\x1b[1;1;31mRoute failed!\x1b[0m\n");
                iprintf("%s\n", res.renderer_error);
                break;
            }
        } else if (held & KEY_B) {
            /* Pause / step mode off */
        }

        /* Update audio playback — drain ring buffer to hardware */
        size_t buf_avail = g_audio_write_pos - g_audio_read_pos;
        if (buf_avail >= DS_AUDIO_RATE / 100u) { /* At least 10 ms of audio */
            size_t to_play = buf_avail < (AUDIO_RING_SIZE / 2) ?
                             buf_avail : (AUDIO_RING_SIZE / 2);
            /* Write to DSP */
            int16_t play_buf[256];
            size_t count = to_play < 256 ? to_play : 256;
            size_t src_pos = g_audio_read_pos % AUDIO_RING_SIZE;
            for (size_t i = 0; i < count; i++) {
                play_buf[i] = g_audio_ring[src_pos];
                src_pos++;
                if (src_pos >= AUDIO_RING_SIZE) src_pos = 0;
            }
            g_audio_read_pos += count;
            /* Note: actual DS audio output requires setting up a DMA buffer.
               For now, this is a placeholder. */
            (void)play_buf;
            (void)count;
        }

        /* Wait for next frame (vsync-like yield) */
        /* DS: use swWaitForVBlank() or just yield */
        swWaitForVBlank();
    }

    /* Shutdown */
    ds_shutdown();
    free(top_screen);
    free(bottom_screen);

    iprintf("\nShutting down.\n");
    return 0;
}