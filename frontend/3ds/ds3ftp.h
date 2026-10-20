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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Public state the bridge needs. */
extern int g_ftp_listen_fd;

/* Initialise the FTP server (non-blocking). Returns true on success. */
bool ds3ftp_init(void);
void ds3ftp_exit(void);
void ds3ftp_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* SIMCITY_DS3FTP_H */