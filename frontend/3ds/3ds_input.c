/*
 * 3ds_input.c — 3DS button + C-Stick + touch input mapping.
 */

#include "3ds_input.h"
#include "3dsdbg.h"
#include "3ds_main.h"

#include <3ds.h>
#include <string.h>

/* ── External state (from 3ds_main.c) ─────────────────────────────── */
extern int  g_soft_pointer;
extern int  g_sp_x, g_sp_y, g_sp_dx, g_sp_dy;
extern int  g_sp_valid;

/* ── ds3_input_read() ─────────────────────────────────────────────── */

uint16_t ds3_input_read(void) {
    hidScanInput();
    uint16_t mask = 0;

    if (hidKeysHeld() & KEY_UP)    mask |= SIMCITY_INPUT_UP;
    if (hidKeysHeld() & KEY_DOWN)  mask |= SIMCITY_INPUT_DOWN;
    if (hidKeysHeld() & KEY_LEFT)  mask |= SIMCITY_INPUT_LEFT;
    if (hidKeysHeld() & KEY_RIGHT) mask |= SIMCITY_INPUT_RIGHT;
    if (hidKeysHeld() & KEY_A)     mask |= SIMCITY_INPUT_A;
    if (hidKeysHeld() & KEY_B)     mask |= SIMCITY_INPUT_B;
    if (hidKeysHeld() & KEY_X)     mask |= SIMCITY_INPUT_X;
    if (hidKeysHeld() & KEY_Y)     mask |= SIMCITY_INPUT_Y;
    if (hidKeysHeld() & KEY_L)     mask |= SIMCITY_INPUT_L;
    if (hidKeysHeld() & KEY_R)     mask |= SIMCITY_INPUT_R;
    if (hidKeysHeld() & KEY_START) mask |= SIMCITY_INPUT_START;
    if (hidKeysHeld() & KEY_SELECT)mask |= SIMCITY_INPUT_SELECT;

    /* Hotkey: SELECT+START+L+R → soft reset */
    if ((hidKeysHeld() & (KEY_SELECT | KEY_START | KEY_L | KEY_R)) ==
        (KEY_SELECT | KEY_START | KEY_L | KEY_R)) {
        /* Trigger soft reset via flag */
        g_soft_pointer = 1;  /* Just a placeholder - actual reset handled in main */
    }

    /* Host-injected buttons, OR-ed in rather than replacing: a test can hold a
       direction while the operator keeps playing, or drive the pad alone. */
    mask |= g_dbg_input;

    return mask;
}

void ds3_input_set_soft_pointer(int enabled) {
    g_soft_pointer = enabled;
}

int ds3_input_soft_pointer_enabled(void) {
    return g_soft_pointer;
}

void ds3_input_update_soft_pointer(void) {
    if (!g_soft_pointer) return;

    touchPosition touch;
    hidTouchRead(&touch);

    int tx = (int)touch.px;
    int ty = (int)touch.py;

    /* Clamp to top screen area (0..255, 0..239) */
    if (tx < 0) tx = 0; else if (tx >= 256) tx = 255;
    if (ty < 0) ty = 0; else if (ty >= 240) ty = 239;

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