/*
 * 3ds_main.c — Nintendo 3DS entry point (libctru + citro2d).
 *
 * Main loop:
 *   1. Initialise libctru (video, sound, touch, HID).
 *   2. Load ROM from filesystem and initialise the core.
 *   3. Per-frame: read input → advance core → render frame → drain audio.
 *   4. On reset (SELECT+START+L+R), unload and reload ROM.
 */

#include "simcity_static_recomp.h"

#include "3ds_main.h"
#include "3ds_input.h"
#include "3ds_render.h"
#include "3dsdbg.h"
#include "3ds_audio.h"

#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ── Constants ─────────────────────────────────────────────────────── */

#define ROM_PATH "sdmc:/SimCity(USA).sfc"

/* ── Crash-localisation tracing (T012) ─────────────────────────────
 *
 * TEMPORARY diagnostic. Markers are written to sdmc:/simcity-trace.log and
 * fflush()ed after every line, so the record survives a data abort that would
 * otherwise eat buffered writes. The file is retrieved over the same FTP
 * channel that delivered the ROM, so the last line before the crash
 * localises the fault without relying on reading the console.
 *
 * On-screen mirroring is opt-in per marker: the console is only refreshed for
 * phase boundaries, not for every emulated frame.
 * Remove once the data abort is fixed.
 */
#define TRACE_PATH "sdmc:/simcity-trace.log"

static FILE *g_trace = NULL;

static void ds3_trace_open(void) {
    g_trace = fopen(TRACE_PATH, "w");
    if (g_trace) {
        fprintf(g_trace, "=== SimCity 3DS trace start ===\n");
        fflush(g_trace);
    } else {
        printf("[trace] could not open %s\n", TRACE_PATH);
    }
}

static void ds3_trace_close(void) {
    if (g_trace) {
        fprintf(g_trace, "=== normal exit ===\n");
        fflush(g_trace);
        fclose(g_trace);
        g_trace = NULL;
    }
}

/* Screen mirror is throttled to avoid flooding the console on long runs. */
static unsigned g_trace_screen_countdown = 0;

static void ds3_trace_screen(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    gfxFlushBuffers();
    gfxSwapBuffers();
}

static void ds3_trace(const char *fmt, ...) {
    char line[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (g_trace) {
        fprintf(g_trace, "%s\n", line);
        fflush(g_trace);
    }
    if (g_trace_screen_countdown) {
        g_trace_screen_countdown--;
        ds3_trace_screen("%s", line);
    }
}

#define DS3_TRACE_SCREEN(n) (g_trace_screen_countdown = (n))

/* Top screen: 256x240 bitmap mode (handled by citro2d) */
#define TOP_SCREEN_WIDTH  256u
#define TOP_SCREEN_HEIGHT 240u

/* Bottom screen: 256x240 for HUD / debug text.
   These are also defined by CMake as -DBOTTOM_SCREEN_WIDTH=256 etc. */

/* Audio: core outputs 32040 Hz stereo; CSND plays at native rate. */

/* ── State ─────────────────────────────────────────────────────────── */

SimCityRecomp *g_recomp = NULL;
uint8_t        g_rom_loaded = 0;
char           g_last_error[256] = "";

/* Soft pointer state (mimics Linux frontend's soft_mouse) */
int g_soft_pointer = 1;
int g_sp_x = 128, g_sp_y = 120;
int g_sp_dx = 0, g_sp_dy = 0;
int g_sp_valid = 0;

/* Audio ring buffer (simplified: just accumulate and play) */
int16_t g_audio_ring[AUDIO_RING_SIZE];
size_t  g_audio_write_pos = 0;
size_t  g_audio_read_pos  = 0;

/* ── Core init / shutdown ─────────────────────────────────────────── */

int ds3_init(const char *rom_path, char *out_error, size_t error_capacity) {
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
    ds3_trace("[T1] create: begin");
    int rc = simcity_recomp_create(&g_recomp, rom_data, (size_t)rom_size,
                                    init_error, sizeof(init_error));
    ds3_trace("[T1] create: rc=%d inst=%p", rc, (void *)g_recomp);
    if (rc != 1 || !g_recomp) {
        free(rom_data);
        snprintf(out_error, error_capacity, "simcity_recomp_create failed: %s",
                 init_error);
        return 0;
    }

    /* Open log if requested */
    ds3_trace("[T2] log_open: begin");
    simcity_recomp_log_open("sdmc:/simcity-3ds.log", NULL, 0);
    ds3_trace("[T2] log_open: done");

    /* Reset to cold boot state */
    char reset_err[256];
    ds3_trace("[T3] reset: begin");
    if (!simcity_recomp_reset(g_recomp, reset_err, sizeof(reset_err))) {
        ds3_trace("[T3] reset: FAILED %s", reset_err);
        free(rom_data);
        snprintf(out_error, error_capacity, "simcity_recomp_reset failed: %s",
                 reset_err);
        return 0;
    }
    ds3_trace("[T3] reset: done");

    /* Free the ROM copy — the core already owns it. */
    free(rom_data);

    g_rom_loaded = 1;
    snprintf(out_error, error_capacity, "");
    return 1;
}

void ds3_shutdown(void) {
    if (g_recomp) {
        simcity_recomp_log_close();
        simcity_recomp_destroy(g_recomp);
        g_recomp = NULL;
        g_rom_loaded = 0;
    }
}

const SimCityRecomp *ds3_get_instance(void) {
    return g_recomp;
}



/* ── Frame advancement ────────────────────────────────────────────── */

SimCityRecompFrameResult ds3_advance_frame(void) {
    SimCityRecompFrameResult result;
    memset(&result, 0, sizeof(result));

    if (!g_recomp) {
        snprintf(result.renderer_error, sizeof(result.renderer_error),
                 "Core not initialised — call ds3_init() first.");
        return result;
    }

    uint16_t input = ds3_input_read();
    ds3_input_update_soft_pointer();

    ds3_trace("[T5] advance_frame: entering simcity_recomp_advance");
    /* Advance one SNES frame */
    int rc = simcity_recomp_advance(g_recomp, input, 1u, &result);
    ds3_trace("[T5] simcity_recomp_advance returned rc=%d", rc);
    if (rc != 1) {
        snprintf(result.renderer_error, sizeof(result.renderer_error),
                 "Advance failed (error=%d). %s", rc,
                 simcity_recomp_last_error(g_recomp));
    }

    /* Drain any available audio into the ring buffer.
       simcity_recomp_audio_read returns interleaved stereo (L,R,L,R,...)
       at SIMCITY_RECOMP_AUDIO_SAMPLE_RATE (32040 Hz).
       ds3_audio_pump() in main() feeds the CSND hardware directly at 32040 Hz. */
    size_t avail = simcity_recomp_audio_available(g_recomp);
    int16_t chunk[1024 * 2];
    while (avail > 0) {
        size_t take = avail < 1024u ? avail : 1024u;
        size_t n = simcity_recomp_audio_read(g_recomp, chunk, take * 2);
        avail -= n;
        /* Write interleaved stereo into ring buffer. */
        for (size_t i = 0; i < n; i += 2) {
            size_t pos = g_audio_write_pos % AUDIO_RING_SIZE;
            g_audio_ring[pos] = chunk[i];
            pos = (g_audio_write_pos + 1) % AUDIO_RING_SIZE;
            g_audio_ring[pos] = (i + 1 < n) ? chunk[i + 1] : 0;
            g_audio_write_pos += 2;
        }
    }

    /* Render the current frame */
    const uint32_t *frame = simcity_recomp_frame_bgra(g_recomp);
    unsigned fw = simcity_recomp_frame_width(g_recomp);
    unsigned fh = SIMCITY_RECOMP_FRAME_HEIGHT;

    if (frame && fw > 0 && fh > 0 && result.frame_rendered) {
        ds3_render_frame(frame, fw, fh,
                         CORE_FRAME_CROP_TOP,
                         CORE_FRAME_CROP_LEFT,
                         CORE_FRAME_CROP_RIGHT);
    }

    /* Render HUD on bottom screen */
    uint64_t instr_count = simcity_recomp_instruction_count(g_recomp);
    uint64_t master_clk  = simcity_recomp_master_clock(g_recomp);
    uint32_t frame_num   = simcity_recomp_current_frame(g_recomp);
    ds3_render_hud(instr_count, master_clk, frame_num, result.renderer_error);

    return result;
}

/* ── Main entry point ─────────────────────────────────────────────── */

/* ── Host debug state ──────────────────────────────────────────────── */

/* Feeds the `state` verb on the debug port. The bridge is game-agnostic, so
   the port cannot read these symbols itself — this is how the host learns where
   the core's frame lives so it can pull the pixels and compare them against the
   framebuffer. Everything the host needs to localise a render bug is here. */
static void ds3_dbg_state(char *buf, unsigned capacity) {
    /* Every value is cast to the exact type the format string promises.
     * uint32_t is `unsigned long` on ARM32, so passing it straight to %u is a
     * format mismatch — and a debug tool that reports the wrong number is worse
     * than no debug tool at all. */
    unsigned        frame = 0u, fw = 0u, fh = 0u;
    unsigned long long instr = 0ull, master = 0ull;
    const void     *src = NULL;
    const char     *err = "no-instance";

    if (g_recomp) {
        frame  = (unsigned)simcity_recomp_current_frame(g_recomp);
        fw     = (unsigned)simcity_recomp_frame_width(g_recomp);
        fh     = (unsigned)SIMCITY_RECOMP_FRAME_HEIGHT;
        src    = (const void *)simcity_recomp_frame_bgra(g_recomp);
        instr  = (unsigned long long)simcity_recomp_instruction_count(g_recomp);
        master = (unsigned long long)simcity_recomp_master_clock(g_recomp);
        err    = simcity_recomp_last_error(g_recomp);
    }

    snprintf(buf, capacity,
             "frame=%u src=%p src_w=%u src_h=%u instructions=%llu master=%llu "
             "last_error=%s",
             frame, src, fw, fh, instr, master, err);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Initialise libctru.
     *
     * gfxInitDefault() is gfxInit(GSP_BGR8_OES, GSP_BGR8_OES, false) — EIGHT
     * bits per channel, THREE bytes per pixel (libctru gfx.h:46). The renderer
     * writes uint16_t RGB565 values into this buffer at 3ds_render.c:128, so
     * the 2-byte writes landed at the wrong stride: every intended pixel was
     * smeared across 1.5 pixels (the reported "repeticoes") and R/G/B landed in
     * the wrong bytes (the reported monochrome).
     *
     * Request RGB565 explicitly so the framebuffer matches the renderer and the
     * debug dump, both of which already speak 2-byte pixels. Reading the format
     * off libctru's own header rather than assuming it is what "default" means.
     * The third argument (false) keeps framebuffers in linear DRAM, not VRAM —
     * gfxInitDefault's behaviour, and correct for a 240x400 double buffer. */
    gfxInitDefault(); /* TEMPORARILY REVERTED FOR COMMIT SPLIT */

    /* Console on bottom screen only — top screen is reserved for the game frame.
       Placing the text console on the top screen overwrote the framebuffer with
       ANSI escape sequences and produced the triplicated garbled text. */
    consoleInit(GFX_BOTTOM, NULL);

    /* Initialize framebuffer pointers for rendering */
    ds3_render_init();

    /* Initialise audio */
    ds3_audio_init();

    /* Bring up the TCP debug server (non-blocking). */
    dbgk_init();
    dbgk_set_state_fn(ds3_dbg_state);

    /* Print startup banner */
    ds3_trace_open();
    DS3_TRACE_SCREEN(12);
    ds3_trace("boot: rom_path=%s", ROM_PATH);
    printf("\x1b[1;1;37mSimCity 3DS\x1b[0m\n");
    printf("ROM: %s\n", ROM_PATH);
    printf("Top: 256x240 | Bot: 256x240\n");
    printf("Press A to start, SELECT+START+L+R to reset.\n");
    printf("\n");

    /* Initial core init */
    char init_err[256];
    if (!ds3_init(ROM_PATH, init_err, sizeof(init_err))) {
        printf("INIT FAILED: %s\n", init_err);
        /* Wait for A press before exiting */
        while (1) { hidScanInput(); if (hidKeysDown() & KEY_A) break; }
        return 0;
    }
    printf("Core initialised.\n");

    /* Main game loop */
    unsigned long trace_frames = 0;
    int paused = 0;
    ds3_trace("[T4] loop: entered, auto-advance on (START toggles pause)");
    DS3_TRACE_SCREEN(4);
    printf("\n[RUNNING] auto-advance on; START pauses.\n");
    while (1) {
        /* The host freezes emulation when it wants to measure the screen. With
           the game advancing at 60 fps, a socket read of the framebuffer spans
           ~190 round trips and the pixels change under the reader — the capture
           comes back a blend of many frames and every conclusion drawn from it
           is worthless. Pausing costs nothing and makes the read atomic.
           This runs BEFORE do_advance is decided so a `pause` or `step` sent
           last iteration takes effect on this one, not the next. */
        if (g_dbg_pause >= 0) paused = g_dbg_pause;
        int dbg_step_now = (g_dbg_step > 0);
        if (dbg_step_now) g_dbg_step--;

        /* Process input — A starts/steps, B pauses */
        hidScanInput();
        uint16_t held  = hidKeysHeld();
        uint16_t pressed = hidKeysDown();   /* edge-triggered: fires once per press */

        /* Auto-advance: run one emulated frame every loop iteration so the
           game plays without any button input. START toggles pause; A still
           advances while paused. One VBlank (ds3_advance_frame is paced by
           the core to a single frame) per loop keeps it near real time. */
        int do_advance = !paused || (pressed & KEY_A) || dbg_step_now;

        /* SELECT dumps the real pixels (framebuffer + core source) to the SD
           card so the output can be analysed on the host instead of guessed
           from a verbal description. */
        if (pressed & KEY_START) {
            paused = !paused;
            DS3_TRACE_SCREEN(3);
            ds3_trace("[T8] pause toggled -> paused=%d", paused);
            printf(paused ? "\n[PAUSED]\n" : "\n[RUNNING]\n");
        }

        if (do_advance) {
            /* Advance one frame */
            if (trace_frames < 5)
                ds3_trace("[T6] advance #%lu: begin", trace_frames);
            SimCityRecompFrameResult res = ds3_advance_frame();
            if (trace_frames < 5)
                ds3_trace("[T6] advance #%lu: done route=%u rendered=%u frames %u..%u",
                          trace_frames, res.route_continued, res.frame_rendered,
                          res.start_frame, res.end_frame);
            trace_frames++;
            if (res.route_continued == 0) {
                ds3_trace("[T5] ROUTE FAILED: %s", res.renderer_error);
                DS3_TRACE_SCREEN(60);
                break;
            }
        }

        /* Update audio playback — drain ring buffer to hardware */
        ds3_audio_pump();

        /* Host debug channel: service the TCP server (non-blocking). Lets the
           host flip rotation and pull live pixels without button presses. */
        dbgk_poll();
        dbgk_break_check();


        /* Present exactly one swap per VBlank. gfxSwapBuffers() rotates both
           screen buffers (gfx.h:173); calling it more than once per displayed
           frame is what duplicated the image.

           While paused, nothing is rendered, so there is nothing new to
           present. Swapping anyway keeps gfxGetFramebuffer() alternating between
           the two buffers, and a host reading the framebuffer gets whichever
           one it happens to land on — two different images for one screen. Skip
           it: the last presented frame stays up, and the back buffer the host
           reads stops moving. */
        if (!paused || dbg_step_now) {
            gspWaitForVBlank();
            gfxSwapBuffers();
        }
    }

    /* Shutdown */
    ds3_trace("shutdown: frames_advanced=%lu", trace_frames);
    ds3_trace_close();
    ds3_shutdown();
    ds3_audio_exit();
    ds3_render_exit();

    printf("\nShutting down.\n");
    return 0;
}