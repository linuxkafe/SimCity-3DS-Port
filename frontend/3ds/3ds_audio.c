/*
 * 3ds_audio.c — 3DS audio output using NDSP (modern replacement for CSND).
 *
 * The static-recomp core outputs interleaved stereo PCM at 32040 Hz.
 * NDSP plays it directly at the native rate — no resampling needed.
 *
 * NDSP uses a wave-buffer queue: we fill two buffers and alternate them
 * so the hardware always has audio to play while we refill the other.
 */

#include "3ds_audio.h"

#include <3ds.h>
#include <string.h>

#define DS3_AUDIO_FREQ 32040u

/* Each NDSP buffer holds 4 frames of audio (4 * 32040/60 = 2136 stereo pairs).
   Two buffers are queued for double-buffering. */
#define AUDIO_BUF_FRAMES (32040u / 60u * 4)
#define AUDIO_BUF_SAMPLES (AUDIO_BUF_FRAMES * 2)  /* interleaved L,R */

/* NDSP wave buffers — double-buffered */
static int16_t g_ndsp_buf_data0[AUDIO_BUF_SAMPLES];
static int16_t g_ndsp_buf_data1[AUDIO_BUF_SAMPLES];
static ndspWaveBuf g_ndsp_wave0;
static ndspWaveBuf g_ndsp_wave1;
static int g_ndsp_buf_idx = 0;

void ds3_audio_init(void) {
    ndspInit();
    ndspChnSetRate(0, DS3_AUDIO_FREQ);
    ndspChnSetInterp(0, NDSP_INTERP_NONE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    /* Output mode is stereo by default with NDSP_FORMAT_STEREO_PCM16. */
    ndspChnReset(0);

    /* Pre-fill both wave buffers as empty so NDSP has something to play. */
    memset(g_ndsp_buf_data0, 0, sizeof(g_ndsp_buf_data0));
    memset(g_ndsp_buf_data1, 0, sizeof(g_ndsp_buf_data1));
    g_ndsp_wave0.data_pcm16 = g_ndsp_buf_data0;
    g_ndsp_wave0.nsamples   = AUDIO_BUF_FRAMES;
    g_ndsp_wave0.looping    = false;
    g_ndsp_wave1.data_pcm16 = g_ndsp_buf_data1;
    g_ndsp_wave1.nsamples   = AUDIO_BUF_FRAMES;
    g_ndsp_wave1.looping    = false;
    ndspChnWaveBufAdd(0, &g_ndsp_wave0);
    ndspChnWaveBufAdd(0, &g_ndsp_wave1);
}

void ds3_audio_exit(void) {
    ndspExit();
}

int ds3_audio_pump(void) {
    /* Pull AUDIO_BUF_FRAMES stereo pairs from the ring buffer.
       If not enough samples are available, skip this frame. */
    extern int16_t g_audio_ring[];
    extern size_t  g_audio_write_pos;
    extern size_t  g_audio_read_pos;

    size_t needed = AUDIO_BUF_FRAMES * 2;  /* interleaved L,R samples */
    size_t available = g_audio_write_pos - g_audio_read_pos;
    if (available < needed) return 0;

    /* Select the buffer that NDSP has finished playing (or will play next). */
    int16_t *dst = g_ndsp_buf_idx == 0 ? g_ndsp_buf_data0 : g_ndsp_buf_data1;
    ndspWaveBuf *wave = g_ndsp_buf_idx == 0 ? &g_ndsp_wave0 : &g_ndsp_wave1;

    /* Pull samples from ring buffer into the NDSP buffer. */
    size_t src_pos = g_audio_read_pos % AUDIO_RING_SIZE;
    for (size_t i = 0; i < needed; i++) {
        dst[i] = g_audio_ring[src_pos];
        src_pos++;
        if (src_pos >= AUDIO_RING_SIZE) src_pos = 0;
    }
    g_audio_read_pos += needed;

    /* Flush data cache so the NDSP CPU sees our writes. */
    CSND_FlushDataCache(dst, needed * sizeof(int16_t));

    /* Re-queue the buffer. NDSP will play it when its turn comes. */
    wave->data_pcm16 = dst;
    wave->nsamples   = AUDIO_BUF_FRAMES;
    wave->looping    = false;
    ndspChnWaveBufAdd(0, wave);

    /* Toggle buffer index for next frame. */
    g_ndsp_buf_idx ^= 1;

    return 1;
}
