/*
 * ds_audio.c — DS audio output (software path).
 *
 * The core produces 32,040 Hz stereo 16-bit PCM.
 * This module mixes down to mono and feeds the DS audio hardware.
 *
 * TODO (T005): Replace with ARM7 co-processor offload for better fidelity.
 */

#include "simcity_static_recomp.h"
#include "ds_audio.h"

#include <nds.h>
#include <nds/arm9/audio.h>
#include <nds/arm9/sound.h>
#include <string.h>

#define DS_AUDIO_FREQ 22050u
#define DS_AUDIO_CHAN 1u
#define AUDIO_BUF_FRAMES (DS_AUDIO_FREQ / 60u * 4)
#define AUDIO_BUF_SAMPLES (AUDIO_BUF_FRAMES * DS_AUDIO_CHAN)

static int16_t g_audio_buf[AUDIO_BUF_SAMPLES];
static size_t  g_buf_write = 0;
static size_t  g_buf_read  = 0;

void ds_audio_push(int16_t left, int16_t right) {
    int16_t mono = (int16_t)((left + right) >> 1);
    size_t pos = g_buf_write % AUDIO_BUF_SAMPLES;
    g_audio_buf[pos] = mono;
    g_buf_write++;
}

int ds_audio_drain(void) {
    size_t available = g_buf_write - g_buf_read;
    if (available < AUDIO_BUF_FRAMES / 4) return 0;

    size_t count = available < AUDIO_BUF_SAMPLES ? available : AUDIO_BUF_SAMPLES;
    size_t src_pos = g_buf_read % AUDIO_BUF_SAMPLES;

    (void)count;
    (void)src_pos;
    /* TODO: Feed g_audio_buf[src_pos..] to sound channel. */
    g_buf_read += count;
    return 1;
}

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