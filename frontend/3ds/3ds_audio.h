/*
 * 3ds_audio.h — 3DS audio output path (CSND hardware stereo DAC).
 *
 * The core produces 32,040 Hz stereo 16-bit PCM.
 * We feed it directly to CSND at the native rate — no resampling needed.
 */

#ifndef SIMCITY_3DS_AUDIO_H
#define SIMCITY_3DS_AUDIO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio constants — shared between main.c and audio.c */
#define DS3_AUDIO_FREQ     32040u
#define DS3_AUDIO_CHANNELS 2u
#define AUDIO_RING_SIZE    (DS3_AUDIO_FREQ * 2 / DS3_AUDIO_CHANNELS)

/* Initialise the 3DS audio subsystem. */
void ds3_audio_init(void);

/* Shutdown the 3DS audio subsystem. */
void ds3_audio_exit(void);

/* Drain the ring buffer into the 3DS audio hardware.
   Called once per frame from ds3_advance_frame() or main().
   Returns 1 if samples were played, 0 if the buffer is empty. */
int ds3_audio_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* SIMCITY_3DS_AUDIO_H */
