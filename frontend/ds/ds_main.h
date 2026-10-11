#ifndef SIMCITY_DS_MAIN_H
#define SIMCITY_DS_MAIN_H

/*
 * ds_main.h — Nintendo DS frontend entry point.
 *
 * Uses libnds (NitroSDK) for video, audio, input, and sound.
 * The top screen shows the game frame (256x192 bitmap); the bottom screen
 * shows a simple HUD / diagnostic output.
 */

#include "simcity_static_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called once at boot. Loads the ROM from filesystem and initialises the
   core. Returns 1 on success, 0 on failure (writes error to out_error). */
int ds_init(const char *rom_path, char *out_error, size_t error_capacity);

/* Called each frame. Advances the core by one SNES frame, renders to the
   top screen, and reads input from the DS pad. Returns the result struct. */
SimCityRecompFrameResult ds_advance_frame(void);

/* Called at shutdown. Releases all resources. */
void ds_shutdown(void);

/* Return the current instance for diagnostic access. */
const SimCityRecomp *ds_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif /* SIMCITY_DS_MAIN_H */