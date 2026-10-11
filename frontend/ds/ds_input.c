/*
 * ds_input.c — DS button + touch input mapping.
 */

#include "ds_input.h"
#include "ds_main.h"

#include <nds.h>
#include <nds/arm9/input.h>
#include <nds/arm9/touch.h>
#include <string.h>

/* ── External state (from ds_main.c) ─────────────────────────────── */
extern int  g_soft_pointer;
extern int  g_sp_x, g_sp_y, g_sp_dx, g_sp_dy;
extern int  g_sp_valid;

/* ── ds_input_read() ─────────────────────────────────────────────── */

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

    /* Hotkey: SELECT+START+L+R → soft reset */
    if ((swKeysHeld() & (KEY_SELECT | KEY_START | KEY_L | KEY_R)) ==
        (KEY_SELECT | KEY_START | KEY_L | KEY_R)) {
        /* Reset via fifo to avoid stack overflow */
        fifoQueueValue32(FIFO_USER_01, 1);
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

    int tx = (int)touch.px;
    int ty = (int)touch.py;

    /* Clamp to top screen area (0..255, 0..191) */
    if (tx < 0) tx = 0; else if (tx >= 256) tx = 255;
    if (ty < 0) ty = 0; else if (ty >= 192) ty = 191;

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