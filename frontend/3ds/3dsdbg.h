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