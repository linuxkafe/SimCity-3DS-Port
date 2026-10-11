/*
 * 3ds_input.h — 3DS button + C-Stick + touch input mapping.
 *
 * Maps 3DS hardware buttons to the SNES input mask expected by the static
 * recompilation core (simcity_static_recomp.h).
 *
 * 3DS buttons    → SIMCITY_INPUT_* mapping
 * D-pad up       → SIMCITY_INPUT_UP
 * D-pad down     → SIMCITY_INPUT_DOWN
 * D-pad left     → SIMCITY_INPUT_LEFT
 * D-pad right    → SIMCITY_INPUT_RIGHT
 * A (green)      → SIMCITY_INPUT_A (confirmed)
 * B (red)        → SIMCITY_INPUT_B (cancel)
 * X (yellow)     → SIMCITY_INPUT_X
 * Y (blue)       → SIMCITY_INPUT_Y
 * L shoulder     → SIMCITY_INPUT_L
 * R shoulder     → SIMCITY_INPUT_R
 * Start          → SIMCITY_INPUT_START
 * Select         → SIMCITY_INPUT_SELECT
 * C-Stick        → soft-pointer mode (mapped to cursor D-pad by core)
 * Touch          → soft-pointer (alternative to C-Stick)
 */

#ifndef SIMCITY_3DS_INPUT_H
#define SIMCITY_3DS_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read current 3DS input and return the SNES input mask. */
uint16_t ds3_input_read(void);

/* Enable or disable soft-pointer mode. In this mode the touch position is
   converted to a virtual d-pad acceleration for the guest cursor. */
void ds3_input_set_soft_pointer(int enabled);
int  ds3_input_soft_pointer_enabled(void);

/* Called every frame to update soft-pointer state. Must be called after
   ds3_input_read() and before advancing the core. */
void ds3_input_update_soft_pointer(void);

#ifdef __cplusplus
}
#endif

#endif /* SIMCITY_3DS_INPUT_H */