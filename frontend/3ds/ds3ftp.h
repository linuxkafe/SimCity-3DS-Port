/*
 * ds3ftp.h — anonymous FTP server over the SD card, hosted by the running app.
 *
 * Lets a build be uploaded while the game runs, so a rebuild no longer means
 * quitting to a second homebrew. See ds3ftp.c for why.
 */

#ifndef SIMCITY_DS3FTP_H
#define SIMCITY_DS3FTP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bind the FTP control port (5000). Requires dbgk_init() to have brought up
   soc:u first — both servers share that context. */
void ds3ftp_init(void);

/* Service the server. Non-blocking; call once per main-loop iteration, the
   same place dbgk_poll() is called. */
void ds3ftp_poll(void);

/* Close the listener, the client and any transfer in progress. */
void ds3ftp_exit(void);

#ifdef __cplusplus
}
#endif

#endif /* SIMCITY_DS3FTP_H */