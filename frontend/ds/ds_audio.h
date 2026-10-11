/*
 * ds_audio.c — DS audio output path (software mixer on ARM9).
 *
 * The core produces 32,040 Hz stereo 16-bit PCM. The DS audio hardware
 * supports up to 44,100 Hz mono/stereo via its PSG+PCM channels.
 *
 * Strategy: accumulate core output in a ring buffer, then feed a DMA
 * transfer to the DS audio hardware at a lower sample rate (22050 Hz
 * mono, nearest-neighbor downsample).
 *
 * TODO (T005): Implement proper ARM7 co-processor audio offload for
 * higher fidelity and to free ARM9 cycles for the core.
 */

#include "simcity_static_recomp.h"
#include "ds_audio.h"

#include <nds.h>
#include <nds/arm9/audio.h>
#include <nds/arm9/sound.h>
#include <string.h>

#define DS_AUDIO_FREQ 22050u
#define DS_AUDIO_CHAN 1u   /* mono */
#define AUDIO_BUF_FRAMES (DS_AUDIO_FREQ / 60u * 4) /* 4 frames @ 60Hz */
#define AUDIO_BUF_SAMPLES (AUDIO_BUF_FRAMES * DS_AUDIO_CHAN)

static int16_t g_audio_buf[AUDIO_BUF_SAMPLES];
static size_t  g_buf_write = 0;
static size_t  g_buf_read  = 0;

/* Ring-buffer: push samples from core into local storage. */
void ds_audio_push(int16_t left, int16_t right) {
    /* Nearest-neighbor downsample: stereo → mono, keep every other frame. */
    int16_t mono = (int16_t)((left + right) >> 1);
    size_t pos = g_buf_write % AUDIO_BUF_SAMPLES;
    g_audio_buf[pos] = mono;
    g_buf_write++;
}

/* Drain the ring buffer into the DS audio hardware. Returns 1 if samples
   were available, 0 if the buffer is empty. */
int ds_audio_drain(void) {
    size_t available = g_buf_write - g_buf_read;
    if (available < AUDIO_BUF_FRAMES / 2) return 0; /* Not enough to flush */

    size_t count = available < AUDIO_BUF_SAMPLES ? available : AUDIO_BUF_SAMPLES;
    size_t src_pos = g_buf_read % AUDIO_BUF_SAMPLES;

    /* Feed to sound channel — placeholder, proper implementation in T005 */
    (void)count;
    (void)src_pos;

    g_buf_read += count;
    return 1;
}

/* Called from ds_main.c to pump audio from the core. */
void ds_audio_pump(SimCityRecomp *recomp) {
    if (!recomp) return;
    size_t avail = simcity_recomp_audio_available(recomp);
    int16_t chunk[1024];
    while (avail > 0) {
        size_t take = avail < 1024u ? avail : 1024u;
        size_t n = simcity_recomp_audio_read(recomp, chunk, take);
        avail -= n;
        for (size_t i = 0; i < n; i++) {
            ds_audio_push(chunk[i * 2], chunk[i * 2 + 1]);
        }
    }
    ds_audio_drain();
}