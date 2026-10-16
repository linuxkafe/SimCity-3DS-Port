/*
 * 3dsdbg.h — reusable debug bridge for libctru 3DS homebrew.
 *
 * Talks the GDB Remote Serial Protocol on a TCP port so stock arm-none-eabi-gdb
 * can attach, plus a small ASCII extension for application-specific dumps.
 * Application-agnostic: nothing here refers to any particular game.
 */

#ifndef THREEDS_DEBUGKIT_H
#define THREEDS_DEBUGKIT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime-selectable orientation hook; -1 = let the renderer use its own
   default. A renderer can read this each frame to let the host flip output. */
extern int g_dbg_rotate_mode;

/* Supplies the text the `state` verb returns. The bridge itself knows nothing
   about the game, so the application registers a formatter once at startup:
     dbgk_set_state_fn(my_state_fn);
   It should write key=value pairs, e.g. "frame=12 src=0x0012abcd". Returning
   nothing is allowed; the verb then answers ERR nostate. */
typedef void (*dbgk_state_fn)(char *buf, unsigned capacity);
void dbgk_set_state_fn(dbgk_state_fn fn);

/* Host-driven emulation control.
 *
 * g_dbg_pause is -1 when the host is not driving (the application's own
 * START-to-pause stays in charge). The host sets it to 0 or 1 via the `run` and
 * `pause` verbs. This matters more than it looks: with emulation running at
 * 60 fps, reading the framebuffer over a socket takes ~190 round trips and the
 * pixels move underneath the reader, so any measurement of what is on screen is
 * meaningless unless the image is held still first.
 *
 * g_dbg_step counts frames the host has asked to advance; the loop decrements
 * it one frame at a time so "advance N, then look" is deterministic. */
extern volatile int g_dbg_pause;
extern volatile int g_dbg_step;

/* Bring up the debug server (non-blocking TCP listen). Call once at startup. */
void dbgk_init(void);

/* Service the server: accept a client, read commands, reply. Non-blocking;
   call once per iteration of the host application's main loop. */
void dbgk_poll(void);

/* Called from the main loop after each chunk of work to service breakpoints. */
void dbgk_break_check(void);

/* Write a raw screen framebuffer (w,h as u16 LE, then RGB565 pixels) to path. */
void dbgk_dump_screen(const char *path, int screen);

/* Run one extension command line as if it had arrived on the socket. */
void dbgk_exec_ext(const char *line);

#ifdef __cplusplus
}
#endif

#endif /* THREEDS_DEBUGKIT_H */