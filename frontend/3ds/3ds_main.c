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
#include "ds3ftp.h"

#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* ── Constants ─────────────────────────────────────────────────────── */

#define ROM_PATH "sdmc:/simcity.sfc"

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

/* Per-frame tracing is OFF by default. Each ds3_trace() call does an
   fprintf + fflush to a file on the SD card, and two of them run every single
   emulated frame. Profiling put that at 81.8 million ticks — by far the
   largest single cost in the loop — for a diagnostic added to localise the T012
   data abort, which was fixed in 384592a. The comment at the trace block still
   says "remove once the data abort is fixed".
   The `trace` verb turns it back on when a crash actually needs localising. */
#define g_trace_perframe g_dbg_trace_perframe

/* Lines emitted before the trace file is open are held here and flushed when
   it opens. The ROM read must happen with no other handle open on the SD
   card (see ds3_init), so the trace file is opened after it. */
#define DS3_PENDING_LINES 24
static char g_pending[DS3_PENDING_LINES][256];
static int  g_pending_count = 0;

static void ds3_trace_open(void) {
    g_trace = fopen(TRACE_PATH, "w");
    if (g_trace) {
        fprintf(g_trace, "=== SimCity 3DS trace start ===\n");
        fflush(g_trace);
        for (int i = 0; i < g_pending_count; i++) {
            fprintf(g_trace, "%s\n", g_pending[i]);
        }
        if (g_pending_count)
            fflush(g_trace);
    } else {
        printf("[trace] could not open %s\n", TRACE_PATH);
    }
    g_pending_count = 0;
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
    } else if (g_pending_count < DS3_PENDING_LINES) {
        /* Held until the trace file opens. See g_pending. */
        snprintf(g_pending[g_pending_count], sizeof(g_pending[0]), "%s", line);
        g_pending_count++;
    }
    if (g_trace_screen_countdown) {
        g_trace_screen_countdown--;
        ds3_trace_screen("%s", line);
    }
}

#define DS3_TRACE_SCREEN(n) (g_trace_screen_countdown = (n))

/* ── Init tracing (T038) ──────────────────────────────────────────────
 *
 * These markers are written through immediately, with an fflush after each
 * one. That is deliberate: the whole point of the trace file is to name the
 * call that is stuck, and a marker that is still sitting in a buffer when
 * the app dies says nothing. A boot writes a handful of lines, so the
 * per-line flush cost is irrelevant next to the diagnostic value.
 *
 * The 512 KB ROM read is chunked (see ds3_init) because a single large
 * fread() on the emulated SD card does not return. */
static void ds3_trace_mark(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_trace) {
        fprintf(g_trace, "%s\n", line);
        fflush(g_trace);
    } else if (g_pending_count < DS3_PENDING_LINES) {
        snprintf(g_pending[g_pending_count], sizeof(g_pending[0]), "%s", line);
        g_pending_count++;
    }
}

/* Top screen: 256x240 bitmap mode (handled by citro2d) */
#define TOP_SCREEN_WIDTH  256u
#define TOP_SCREEN_HEIGHT 240u

/* Bottom screen: 256x240 for HUD / debug text.
   These are also defined by CMake as -DBOTTOM_SCREEN_WIDTH=256 etc. */

/* Audio: core outputs 32040 Hz stereo; CSND plays at native rate. */

/* ── Phase timing ─────────────────────────────────────────────────── */

/* svcGetSystemTick() counts at the ARM11 clock, 26827953 Hz -- 26.83 ticks per
 * microsecond, NOT one tick per microsecond. The array this feeds has always
 * been called g_dbg_phase_us[], which is a claim about units that was false:
 * it stored raw tick deltas under a name that says microseconds.
 *
 * That is not a naming quibble. A reader who trusts the name reads
 * loop=138000000 and concludes 138 seconds per frame; the true figure is
 * 0.514 s. This exact confusion cost this session a full round of "why is a
 * frame taking two minutes" before the discrepancy with observed fps was
 * chased down. The hardware run that settled it read loop=138910548 ticks at
 * 268.28 MHz -- 1.94 fps, matching what the screen showed.
 *
 * Converted at the point of measurement, once, so every reader downstream --
 * `prof`, `state`, any future harness -- gets the same units the name claims.
 * Eight 64-bit multiply-divides per frame is not a cost worth discussing.
 *
 * The result is deliberately kept in uint32: the longest frame observed is
 * ~20 s, five orders of magnitude below the wrap point. */
#define DS3_TICK_HZ 26827953ull

/* Network debug bridge (TCP on DBGK_PORT plus the in-app FTP server).
   Default on. Build with -DDS3_NET_BRIDGE=0 to compile the whole surface out.

   This exists to make one question answerable with a single variable: whether the
   recurring svc=0x32 fault and the stall after advance #4 belong to the network
   stack or to the port. Oct-10 build: 0 faults, dies earlier at ptm:sysm.
   Oct-18 build: 85 faults, stalls. The stack is new in that window, so it is the
   first thing to hold constant -- see docs/DECK-RUN-2026-10-19.md.

   It is a measurement switch, not a feature flag. Nothing else should read it. */
#ifndef DS3_NET_BRIDGE
#define DS3_NET_BRIDGE 1
#endif

static uint32_t ds3_ticks_to_us(uint64_t ticks) {
    return (uint32_t)((ticks * 1000000ull) / DS3_TICK_HZ);
}

/* ── State ─────────────────────────────────────────────────────────── */

SimCityRecomp *g_recomp = NULL;
uint8_t        g_rom_loaded = 0;
char           g_last_error[256] = "";
/* Last mask handed to simcity_recomp_advance(), surfaced through `state` so an
   injected press can be confirmed to have reached the guest. */
static volatile uint16_t g_last_input = 0;


/* Soft pointer state (mimics Linux frontend's soft_mouse) */
int g_soft_pointer = 1;
int g_sp_x = 128, g_sp_y = 120;
int g_sp_dx = 0, g_sp_dy = 0;
int g_sp_valid = 0;

/* Audio ring buffer (simplified: just accumulate and play) */
int16_t g_audio_ring[AUDIO_RING_SIZE];
size_t  g_audio_write_pos = 0;
size_t  g_audio_read_pos  = 0;

/* Profiling / satellite mode state */
volatile int g_profiling_mode = 0;        /* 0=normal, 1=profile, 2=satellite */
volatile int g_frame_limit = 0;           /* 0 = unlimited, N = max frames */
volatile int g_frame_count = 0;           /* frames advanced this session */
volatile int g_headless_mode = 0;         /* 1 = skip render/audio for profiling */

/* Per-frame instruction counting (T025). Cumulative delta tells us how many
   SNES instructions each emulated frame consumes. If it grows over time the
   recompiler is re-translating; if stable the interpreter is consistent. */
static uint64_t g_last_instr_count = 0;
static uint64_t g_instr_per_frame  = 0;

/* Satellite / failsafe debug state */
volatile int g_satellite_mode = 0;       /* 1 = offline logging only, no TCP */
volatile int g_debug_transport = 0;      /* 0=TCP, 1=UDP, 2=file, 3=multi */
volatile uint32_t g_last_heartbeat = 0;  /* last successful heartbeat */
volatile int g_connection_lost = 0;      /* 1 = connection lost, needs reconnect */

/* ── Core init / shutdown ─────────────────────────────────────────── */

/* Candidate ROM locations, in order of preference.
 *
 * The Deck has TWO sdmc: directories and only one of them is mounted by the
 * running emulator: the app's own trace file is created in the media root
 * (/run/media/.../Emulation/storage/azahar/sdmc), so that is the mounted
 * root. A symlink of the same name placed in the *other* directory is never
 * seen by the emulator -- which is why a build compiled against
 * "sdmc:/simcity.sfc" failed to open it even though a symlink of that name
 * existed elsewhere (T038 Fault A).
 *
 * So the candidates are plain names in the mounted root, and the first one
 * that opens wins. Each attempt is traced, so the trace file always says
 * which path was tried and which one worked. */
static const char *const k_rom_candidates[] = {
    "sdmc:/simcity.sfc",
    "sdmc:/ROM.SFC",
    "sdmc:/SimCity(USA).sfc",
};
#define DS3_ROM_CANDIDATES (sizeof(k_rom_candidates) / sizeof(k_rom_candidates[0]))

#define DS3_ROM_READ_CHUNK 4096u

int ds3_init(const char *rom_path, char *out_error, size_t error_capacity) {
    if (!out_error) return 0;

    /* The requested path is always tried first, so a caller that passes an
       explicit path still gets exactly that path and nothing else. */
    const char *candidates[DS3_ROM_CANDIDATES + 1];
    size_t n_candidates = 0;
    if (rom_path) candidates[n_candidates++] = rom_path;
    for (size_t i = 0; i < DS3_ROM_CANDIDATES; i++) {
        if (rom_path && strcmp(rom_path, k_rom_candidates[i]) == 0) continue;
        candidates[n_candidates++] = k_rom_candidates[i];
    }

    /* Load ROM from SD card. */
    FILE *f = NULL;
    const char *opened = NULL;
    for (size_t i = 0; i < n_candidates; i++) {
        ds3_trace_mark("[T0] fopen: %s", candidates[i]);
        f = fopen(candidates[i], "rb");
        if (f) { opened = candidates[i]; break; }
        ds3_trace_mark("[T0] fopen failed: %s (errno=%d)", candidates[i], errno);
    }
    if (!f) {
        ds3_trace_mark("[T0] FAILED: no candidate path opened a ROM");
        snprintf(out_error, error_capacity,
                 "Cannot open ROM: tried %zu path(s), none readable",
                 n_candidates);
        return 0;
    }
    ds3_trace_mark("[T0] fopen: OK (%s)", opened);

    ds3_trace_mark("[T0] fseek END");
    fseek(f, 0, SEEK_END);
    ds3_trace_mark("[T0] ftell");
    long rom_size = ftell(f);
    ds3_trace_mark("[T0] rewind");
    rewind(f);
    ds3_trace_mark("[T0] rom_size: %ld", rom_size);

    if (rom_size <= 0 || rom_size > 0x100000) {
        /* SimCity SNES is 512 KB (0x80000); allow up to 1 MB. */
        fclose(f);
        ds3_trace_mark("[T0] FAILED: invalid ROM size %ld", rom_size);
        snprintf(out_error, error_capacity, "Invalid ROM size: %ld", rom_size);
        return 0;
    }

    uint8_t *rom_data = (uint8_t *)malloc((size_t)rom_size);
    if (!rom_data) {
        fclose(f);
        ds3_trace_mark("[T0] FAILED: out of memory for %ld bytes", rom_size);
        snprintf(out_error, error_capacity, "Out of memory for ROM (%ld bytes)", rom_size);
        return 0;
    }
    ds3_trace_mark("[T0] malloc: OK (%ld bytes)", rom_size);

    /* Read the ROM on the handle that is ALREADY open.
     *
     * Measured on the Deck (T038), in order:
     *   1. A single 512 KB fread() never returns.
     *   2. 16 KB fread()s: the first returns, the second never does.
     *   3. 4 KB fread()s: twelve return (48 KB), the thirteenth never does.
     *   4. Reopening the file for each chunk hangs in open() itself.
     *
     * (2), (3) and (4) share one fact: the second operation on the file
     * wedges. (3) reaches 48 KB because each small read leaves the file
     * position in a state the next one can advance from, until it cannot.
     * (4) wedges in open() because the file was already opened once.
     *
     * So the constraint is on the FILE, not on the byte count: the emulated
     * SD card's FAT driver serves one open-and-read sequence per file and
     * then stops. The ROM must therefore be read in ONE pass on ONE handle.
     */
    /* Diagnostic: read in 4096-byte pieces so progress is visible in the
       trace, then probe a DIFFERENT file to see whether the wedge is
       per-file or per-card. */
    size_t bytes_read = 0;
    while (bytes_read < (size_t)rom_size) {
        size_t chunk = (size_t)rom_size - bytes_read;
        if (chunk > DS3_ROM_READ_CHUNK) chunk = DS3_ROM_READ_CHUNK;
        ds3_trace_mark("[T0] read at %zu, %zu bytes", bytes_read, chunk);
        size_t got = fread(rom_data + bytes_read, 1, chunk, f);
        ds3_trace_mark("[T0] got %zu", got);
        if (got == 0) break;
        bytes_read += got;
    }
    fclose(f);
    ds3_trace_mark("[T0] fread: %zu/%ld bytes", bytes_read, rom_size);

    /* Probe a different, small file. If this succeeds, the wedge is
       per-file and the ROM can be split across several files. */
    {
        ds3_trace_mark("[T0] probe: opening sm_max_frames");
        FILE *probe = fopen("sdmc:/sm_max_frames", "rb");
        ds3_trace_mark("[T0] probe: open -> %d", probe ? 1 : 0);
        if (probe) {
            char buf[64];
            size_t n = fread(buf, 1, sizeof(buf) - 1, probe);
            buf[n] = 0;
            ds3_trace_mark("[T0] probe: read %zu bytes [%s]", n, buf);
            fclose(probe);
            ds3_trace_mark("[T0] probe: closed");
        }
    }
    if (bytes_read != (size_t)rom_size) {
        free(rom_data);
        ds3_trace_mark("[T0] FAILED: incomplete ROM read %zu/%ld",
                       bytes_read, rom_size);
        snprintf(out_error, error_capacity, "Incomplete ROM read: %zu/%ld",
                 bytes_read, rom_size);
        return 0;
    }
    ds3_trace_mark("[T0] ROM loaded OK");

    /* Create the static recompilation instance */
    char init_error[256];
    ds3_trace("[T1] create: begin");
    int rc = simcity_recomp_create(&g_recomp, rom_data, (size_t)rom_size,
                                    init_error, sizeof(init_error));
    ds3_trace("[T1] create: rc=%d inst=%p", rc, (void *)g_recomp);
    if (rc != 1 || !g_recomp) {
        free(rom_data);
        ds3_trace("[T1] FAILED: simcity_recomp_create: %s", init_error);
        snprintf(out_error, error_capacity, "simcity_recomp_create failed: %s",
                 init_error);
        return 0;
    }

    /* The core log is OFF by default, and this used to be a silent 14.5% tax.

       sc_core_logging_enabled() is `g_core_log != NULL`, and render_lines_impl
       gates two SHA-256 passes behind it: 122368 bytes of out555 plus 244736
       of out32. With this log open, both ran every frame -- 367 KB hashed per
       frame to fill two report fields the 3DS frontend never reads.

       Measured on device, 12 interleaved pairs, same build, same ROM:
           mean  +21 307 873 ticks  (+14.5% of the frame)
           range +18.2M .. +23.6M, every pair positive
       The interleaving matters: `core` also varies by up to 42% with scene
       content, so measuring the two arms minutes apart gave a bimodal set
       whose average said the opposite. See aes/tickets/T028.

       Nothing is lost. The core log carried "create", "cold-reset-ready",
       "route-failed" and "frame" events; this frontend has its own ds3_trace
       for all of that, and `cclog 1` reopens the core log on demand for when
       the upstream events are actually wanted. */
    ds3_trace("[T2] core log: off by default (cclog 1 to enable; sha256 costs ~14.5%)");

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

    uint64_t t_core = svcGetSystemTick();
    uint16_t input = ds3_input_read();
    ds3_input_update_soft_pointer();

    /* Record what was actually handed to the core, and retire a `tap` pulse
       once it has spanned enough frames to have been sampled. The host reads
       this back through `state`, so an injected press is *verified* to have
       reached the guest rather than assumed from a zero exit code. */
    g_last_input = input;
    if (g_dbg_input_frames > 0) {
        g_dbg_input_frames--;
        if (g_dbg_input_frames == 0) g_dbg_input = 0;
    }

    if (g_trace_perframe)
        ds3_trace("[T5] advance_frame: entering simcity_recomp_advance");
    /* Advance one SNES frame.
     *
     * mode 1 defers the expensive host frame conversion to
     * simcity_recomp_render_current_frame(), which the core exposes precisely
     * for hosts that do not need every frame converted. Everything else —
     * CPU, PPU timing, S-SMP, S-DSP, controller history — advances identically,
     * so the A/B is sound and only the conversion moves. */
    int rc = (g_dbg_advance_mode == 1)
           ? simcity_recomp_advance_headless(g_recomp, input, 1u, &result)
           : simcity_recomp_advance(g_recomp, input, 1u, &result);
    if (g_trace_perframe)
        ds3_trace("[T5] simcity_recomp_advance returned rc=%d", rc);

    /* Per-frame instruction count (T025 step 1). Cumulative delta tells us
       how many SNES instructions each emulated frame consumes. If it grows
       over time the recompiler is re-translating; if stable the interpreter
       is consistent. Logged every 10 frames to avoid trace log flooding. */
    uint64_t instr_now = simcity_recomp_instruction_count(g_recomp);
    if (g_frame_count > 0) {
        g_instr_per_frame = instr_now - g_last_instr_count;
        if (g_frame_count % 10 == 0)
            ds3_trace("[T5] instr/frame #%u: %llu",
                      (unsigned)g_frame_count, (unsigned long long)g_instr_per_frame);
    }
    g_last_instr_count = instr_now;
    g_frame_count++;

    g_dbg_phase_us[DBGK_PHASE_CORE] =
        ds3_ticks_to_us(svcGetSystemTick() - t_core);
    uint64_t t_render = svcGetSystemTick();
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

    /* In headless mode the frame has not been converted yet. */
    if (g_dbg_advance_mode == 1) {
        char rerr[128];
        if (simcity_recomp_render_current_frame(g_recomp, rerr, sizeof(rerr)) != 1) {
            ds3_trace("[T7] render_current_frame failed: %s", rerr);
        }
    }

    /* Render the current frame.
       frame_bgr555, not frame_bgra: the core already composited this frame in
       15-bit BGR555 (patches/0003), so asking for the 32-bit buffer only to
       narrow it again was a second pass over every pixel to throw away the
       widening. The conversion in ds3_render_frame is bit-identical to the one
       it replaces -- see the proof in tools/check_rgb565_exact.c. */
    const uint16_t *frame = simcity_recomp_frame_bgr555(g_recomp);
    unsigned fw = simcity_recomp_frame_width(g_recomp);
    unsigned fh = SIMCITY_RECOMP_FRAME_HEIGHT;

    /* Skip frame conversion in headless/satellite mode for performance */
    if (!g_headless_mode && !g_satellite_mode) {
        if (frame && fw > 0 && fh > 0 && result.frame_rendered) {
            ds3_render_frame(frame, fw, fh,
                             CORE_FRAME_CROP_TOP,
                             CORE_FRAME_CROP_LEFT,
                             CORE_FRAME_CROP_RIGHT);
        }
    }

    g_dbg_phase_us[DBGK_PHASE_RENDER] =
        ds3_ticks_to_us(svcGetSystemTick() - t_render);
    uint64_t t_hud = svcGetSystemTick();

    /* Render HUD on bottom screen. Gated: by default this clears the console
       and rewrites the whole bottom framebuffer every single frame, which both
       costs time and wipes any diagnostic printed at boot. */
    if (g_dbg_hud) {
    uint64_t instr_count = simcity_recomp_instruction_count(g_recomp);
    uint64_t master_clk  = simcity_recomp_master_clock(g_recomp);
    uint32_t frame_num   = simcity_recomp_current_frame(g_recomp);
    ds3_render_hud(instr_count, master_clk, frame_num, result.renderer_error);
    g_dbg_phase_us[DBGK_PHASE_HUD] =
        ds3_ticks_to_us(svcGetSystemTick() - t_hud);
    }

    return result;
}

/* ── Main entry point ─────────────────────────────────────────────── */

/* ── Host debug state ──────────────────────────────────────────────── */

/* Feeds the `state` verb on the debug port. The bridge is game-agnostic, so
   the port cannot read these symbols itself — this is how the host learns where
   the core's frame lives so it can pull the pixels and compare them against the
   framebuffer. Everything the host needs to localise a render bug is here. */
/* Returns the composited frame digest, or NULL if there is no instance.
   STALE unless the core log is open -- the core only recomputes it behind
   sc_core_logging_enabled(). That is why the harness must `cclog 1` first. */
static const char *ds3_dbg_hash(void) {
    return g_recomp ? simcity_recomp_frame_bgr555_sha256(g_recomp) : NULL;
}

/* Cold reset to the ROM's boot state, for determinism runs. */
static int ds3_dbg_reset(char *err, unsigned capacity) {
    if (!g_recomp) {
        snprintf(err, capacity, "no-instance");
        return 0;
    }
    return simcity_recomp_reset(g_recomp, err, capacity);
}

static void ds3_dbg_state(char *buf, unsigned capacity) {
    /* Every value is cast to the exact type the format string promises.
     * uint32_t is `unsigned long` on ARM32, so passing it straight to %u is a
     * format mismatch — and a debug tool that reports the wrong number is worse
     * than no debug tool at all. */
    unsigned        frame = 0u, fw = 0u, fh = 0u;
    unsigned long long instr = 0ull, master = 0ull;
    const void     *src = NULL;
    const char     *err = "no-instance";
    /* First 16 hex chars of the frame digest. Named fhash, not fh: fh is the
       frame height two declarations up. Truncated to 16 because `state` is a
       one-line human readout and the full 64 are available from `hash`. */
    char            fhash[17] = "----------------";

    if (g_recomp) {
        frame  = (unsigned)simcity_recomp_current_frame(g_recomp);
        fw     = (unsigned)simcity_recomp_frame_width(g_recomp);
        fh     = (unsigned)SIMCITY_RECOMP_FRAME_HEIGHT;
        src    = (const void *)simcity_recomp_frame_bgra(g_recomp);
        instr  = (unsigned long long)simcity_recomp_instruction_count(g_recomp);
        master = (unsigned long long)simcity_recomp_master_clock(g_recomp);
        err    = simcity_recomp_last_error(g_recomp);
        {
            const char *h = simcity_recomp_frame_bgr555_sha256(g_recomp);
            /* All zeros means no frame has been hashed yet, or logging is off
               and this is the previous frame's value. Showing the dashes
               instead makes that visible rather than printing a plausible-
               looking digest that means nothing. */
            if (h && h[0] != '0') { memcpy(fhash, h, 16); fhash[16] = '\0'; }
        }
    }

    snprintf(buf, capacity,
             "frame=%u src=%p src_w=%u src_h=%u instructions=%llu master=%llu "
             "input=0x%04X injected=0x%04X chan=%d rot=%d "
             "build=" DS3_BUILD_ID " mode=%d hud=%d adv=%u core=%u render=%u hudus=%u "
             "audio=%u dbg=%u present=%u loop=%u ftp=%d fh=%.16s "
             "instr_frame=%llu last_error=%s",
             frame, src, fw, fh, instr, master,
             (unsigned)g_last_input, (unsigned)g_dbg_input,
             g_dbg_chan_swap, g_dbg_rotate_mode, g_dbg_advance_mode, g_dbg_hud,
             (unsigned)g_dbg_phase_us[DBGK_PHASE_ADVANCE],
             (unsigned)g_dbg_phase_us[DBGK_PHASE_CORE],
             (unsigned)g_dbg_phase_us[DBGK_PHASE_RENDER],
             (unsigned)g_dbg_phase_us[DBGK_PHASE_HUD],
             (unsigned)g_dbg_phase_us[DBGK_PHASE_AUDIO],
             (unsigned)g_dbg_phase_us[DBGK_PHASE_DEBUG],
             (unsigned)g_dbg_phase_us[DBGK_PHASE_PRESENT],
             (unsigned)g_dbg_phase_us[DBGK_PHASE_LOOP], g_ftp_listen_fd, fhash,
             (unsigned long long)g_instr_per_frame, err);
}

/* Brings up the debug bridge and FTP server. Called at boot and then about once
   a second from the main loop until each one is listening, because Wi-Fi may
   not be connected yet at launch (see dbgk_init). Reports each success once. */
static void ds3_net_service_start(int force) {
#if DS3_NET_BRIDGE
    static int      s_dbg_up = 0;
    static int      s_ftp_up = 0;
    static unsigned s_tick   = 0;

    if (!force && (++s_tick % 60u) != 0) return;
    if (s_dbg_up && s_ftp_up) return;

    if (!s_dbg_up && dbgk_init()) {
        s_dbg_up = 1;
        printf("\x1b[1;32m[NET] debug bridge OK, port 5002\x1b[0m\n");
    }
    if (s_dbg_up && !s_ftp_up && ds3ftp_init()) {
        s_ftp_up = 1;
        printf("\x1b[1;32m[NET] FTP OK, port 5000\x1b[0m\n");
    }
#else
    (void)force;
#endif
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
    gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);

    /* Console on bottom screen only — top screen is reserved for the game frame.
       Placing the text console on the top screen overwrote the framebuffer with
       ANSI escape sequences and produced the triplicated garbled text. */
    consoleInit(GFX_BOTTOM, NULL);

    /* Initialize framebuffer pointers for rendering */
    ds3_render_init();

    /* Initialise audio */
    ds3_audio_init();

    dbgk_set_state_fn(ds3_dbg_state);
    /* Exposed for the determinism harness (T032). `reset` is what makes two
       runs comparable: without it the second starts wherever the first ended,
       and comparing frames then proves nothing. */
    dbgk_set_reset_fn(ds3_dbg_reset);
    dbgk_set_hash_fn(ds3_dbg_hash);

    /* Startup banner. */
    ds3_trace_open();
    DS3_TRACE_SCREEN(12);
    ds3_trace("boot: rom_path=%s", ROM_PATH);
    printf("\x1b[1;1;37mSimCity 3DS\x1b[0m\n");
    printf("ROM: %s\n", ROM_PATH);
    printf("Top: 256x240 | Bot: 256x240\n");
    printf("Press A to start. SELECT+START to exit to the Home Menu.\n");
    printf("\n");

    /* Initial core init. */
    char init_err[256];
    int init_ok = ds3_init(ROM_PATH, init_err, sizeof(init_err));

    if (!init_ok) {
        /* The console of a detached launch goes nowhere, so the failure has
           to be in the trace file too. Without this the only surviving
           observable was the last successful [T0] line, which reads like a
           hang -- it is not one; the app is about to block waiting for a
           keypress (T038). */
        ds3_trace("[T2] INIT FAILED: %s", init_err);
        printf("INIT FAILED: %s\n", init_err);
        /* Wait for A press before exiting */
        while (1) { hidScanInput(); if (hidKeysDown() & KEY_A) break; }
        return 0;
    }
    printf("Core initialised.\n");

    /* Bring up the TCP debug server and FTP server. This is deferred until
       after the ROM is loaded: the FTP server roots at sdmc:/ and its
       directory handle would otherwise be open during the ROM read. */
    printf("[NET] waiting for Wi-Fi (debug 5002, FTP 5000)...\n");
    ds3_net_service_start(1);

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
        uint64_t loop_start = svcGetSystemTick();
        uint64_t t_loop = loop_start;
        uint16_t held  = hidKeysHeld();
        uint16_t pressed = hidKeysDown();   /* edge-triggered: fires once per press */

        /* Two ways out, because forcing the operator to hold the power button
           between every test is not a workflow. SELECT+START on the device, or
           the `quit` verb from the host. Both unwind through the same shutdown
           path so the 3DSX loader gets to return to the Home Menu. */
        if (g_dbg_quit ||
            ((held & (KEY_SELECT | KEY_START)) == (KEY_SELECT | KEY_START))) {
            ds3_trace("exit: quit requested (verb=%d)", g_dbg_quit);
            break;
        }

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

        /* Per-phase timing: the port runs at 1.59 fps while the core uses
           0.007% of the CPU, so the cost is somewhere in this loop and only
           measurement can say where. Averaged so one slow iteration lies less. */
        g_dbg_phase_us[DBGK_PHASE_ADVANCE] =
            ds3_ticks_to_us(svcGetSystemTick() - t_loop);
        t_loop = svcGetSystemTick();

        /* Update audio playback — drain ring buffer to hardware */
        ds3_audio_pump();

        g_dbg_phase_us[DBGK_PHASE_AUDIO] =
            ds3_ticks_to_us(svcGetSystemTick() - t_loop);
        t_loop = svcGetSystemTick();

        /* Host debug channel: service the TCP server (non-blocking). Lets the
           host flip rotation and pull live pixels without button presses. */
        ds3_net_service_start(0);
        dbgk_poll();
        dbgk_break_check();
#if DS3_NET_BRIDGE
        ds3ftp_poll();
#endif

        g_dbg_phase_us[DBGK_PHASE_DEBUG] =
            ds3_ticks_to_us(svcGetSystemTick() - t_loop);
        t_loop = svcGetSystemTick();


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

        g_dbg_phase_us[DBGK_PHASE_PRESENT] =
            ds3_ticks_to_us(svcGetSystemTick() - t_loop);
        g_dbg_phase_us[DBGK_PHASE_LOOP] =
            ds3_ticks_to_us(svcGetSystemTick() - loop_start);
        for (int _p = 0; _p < DBGK_PHASE_COUNT; _p++) {
            g_dbg_phase_avg[_p] =
                (g_dbg_phase_avg[_p] * 7u + g_dbg_phase_us[_p]) / 8u;
        }
    }

    /* Shutdown */
    ds3_trace("shutdown: frames_advanced=%lu", trace_frames);
    ds3_trace_close();

    /* Order matters: the network port is closed before the services it was
       serving come down, and gfx goes last so anything still drawing has a
       framebuffer to draw into. Leaving the socket bound makes the next run
       look like a dead device rather than a closed app. */
    printf("\nShutting down...\n");
    ds3_render_exit();
    ds3_audio_exit();
#if DS3_NET_BRIDGE
    ds3ftp_exit();
#endif
    dbgk_exit();
    ds3_shutdown();
    gfxExit();

    return 0;
}