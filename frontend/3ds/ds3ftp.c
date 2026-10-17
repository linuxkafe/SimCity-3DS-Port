/*
 * ds3ftp.c — anonymous FTP server for the SD card, so debugging never needs a
 * second homebrew.
 *
 * Why this exists: the 3DS runs one homebrew at a time. The FTP server the
 * operator used to upload builds was itself a homebrew, so every rebuild meant
 * quit the game, start FTP, upload, relaunch. Hosting FTP inside the running
 * app removes that dance entirely — the debug port (5002) and FTP (5000) are
 * live at the same time, in the same process, against the same card.
 *
 * Deliberately minimal, and deliberately not a general-purpose server:
 *   - anonymous only; there is nothing here worth authenticating
 *   - the root is the SD card and there is no way out of it
 *   - one client at a time
 *   - transfers are chunked per poll so a big file cannot stall the game loop
 *
 * Standard C file I/O is used against sdmc:/ paths, which libctru maps onto
 * the 3DS filesystem. That is the same mechanism dbgk_dump_screen relies on.
 */

#include "ds3ftp.h"
#include "3dsdbg.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>   /* htons/htonl/ntohs */
#include <fcntl.h>      /* O_NONBLOCK, F_SETFL */
#include <errno.h>

#define FTP_PORT        5000
#define FTP_BACKLOG     2
#define FTP_LINE_MAX    1024
#define FTP_XFER_CHUNK  16384   /* bytes moved per poll; bounds loop stall */

static int   g_listen = -1;
static int   g_client = -1;
static FILE  *g_xfer_file = NULL;
static int   g_xfer_send = 0;      /* 1 = RETR, 0 = STOR */
static long  g_xfer_left = 0;
static char  g_cwd[768] = "sdmc:/";
static char  g_line[FTP_LINE_MAX];
static size_t g_line_len = 0;
static char  g_rbuf[FTP_XFER_CHUNK];
static int   g_pasv_port = 0;
static int   g_data_fd = -1;

static void ftp_reply(int fd, const char *s) {
    (void)!send(fd, s, strlen(s), 0);
}

/* ── Path handling ─────────────────────────────────────────────────── */

/* Build an absolute card path from the current directory plus a client-supplied
 * argument. Rejects anything that could climb out of the card: "..", absolute
 * paths, and NUL. Returns 0 on success. */
static int ftp_abspath(const char *arg, char *out, size_t outlen) {
    char tmp[768];

    if (arg && arg[0] == '/') arg++;          /* ignore a leading slash */
    if (strstr(arg, "..") != NULL) return -1;  /* no traversal, no debate */

    if (arg == NULL || arg[0] == '\0' || strcmp(arg, ".") == 0) {
        snprintf(out, outlen, "%s", g_cwd);
        return 0;
    }
    snprintf(tmp, sizeof(tmp), "%s%s", g_cwd, arg);
    /* trim a trailing slash unless it is the root */
    size_t n = strlen(tmp);
    if (n > 6 && tmp[n - 1] == '/') tmp[n - 1] = '\0';
    snprintf(out, outlen, "%s", tmp);
    return 0;
}

/* ── Directory listing ─────────────────────────────────────────────── */

static void ftp_list(int fd) {
    char path[768];
    DIR *d;
    struct dirent *e;

    if (ftp_abspath(NULL, path, sizeof(path)) != 0) return;
    d = opendir(path);
    if (!d) { ftp_reply(fd, "550 no such directory\r\n"); return; }

    ftp_reply(fd, "150 listing\r\n");
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        char full[1024];
        char line[1200];
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(full, sizeof(full), "%s%s", path,
                 (path[strlen(path) - 1] == '/') ? "" : "/");
        strncat(full, e->d_name, sizeof(full) - strlen(full) - 1);
        if (stat(full, &st) != 0) continue;
        snprintf(line, sizeof(line), "%s %8ld %s\r\n",
                 S_ISDIR(st.st_mode) ? "drwxr-xr-x" : "-rw-r--r--",
                 (long)st.st_size, e->d_name);
        (void)!send(fd, line, strlen(line), 0);
    }
    closedir(d);
    ftp_reply(fd, "226 done\r\n");
}

/* ── Commands ──────────────────────────────────────────────────────── */

static void ftp_open_passive(int fd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int pfd;
    char reply[128];

    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        ftp_reply(fd, "425 cannot determine port\r\n");
        return;
    }
    pfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (pfd < 0) { ftp_reply(fd, "425 cannot open data socket\r\n"); return; }

    setsockopt(pfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

    struct sockaddr_in pa;
    memset(&pa, 0, sizeof(pa));
    pa.sin_family = AF_INET;
    pa.sin_addr = addr.sin_addr;
    pa.sin_port = 0;
    if (bind(pfd, (struct sockaddr *)&pa, sizeof(pa)) != 0 || listen(pfd, 1) != 0) {
        close(pfd);
        ftp_reply(fd, "425 cannot bind data socket\r\n");
        return;
    }
    len = sizeof(pa);
    if (getsockname(pfd, (struct sockaddr *)&pa, &len) != 0) {
        close(pfd);
        ftp_reply(fd, "425 cannot determine data port\r\n");
        return;
    }
    g_pasv_port = ntohs(pa.sin_port);
    snprintf(reply, sizeof(reply), "227 entering passive mode (127,0,0,1,%d,%d)\r\n",
             (g_pasv_port >> 8) & 0xFF, g_pasv_port & 0xFF);
    ftp_reply(fd, reply);
}

static int ftp_connect_data(void) {
    struct sockaddr_in pa;
    int dfd;

    if (g_pasv_port == 0) return -1;
    dfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (dfd < 0) return -1;
    memset(&pa, 0, sizeof(pa));
    pa.sin_family = AF_INET;
    pa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    pa.sin_port = htons((uint16_t)g_pasv_port);
    if (connect(dfd, (struct sockaddr *)&pa, sizeof(pa)) != 0) {
        close(dfd);
        return -1;
    }
    g_pasv_port = 0;
    return dfd;
}

static void ftp_exec(int fd, char *cmd, char *arg) {
    if (!strcasecmp(cmd, "USER") || !strcasecmp(cmd, "PASS")) {
        ftp_reply(fd, "230 anonymous access granted\r\n");
    } else if (!strcasecmp(cmd, "SYST")) {
        ftp_reply(fd, "215 UNIX Type: L8\r\n");
    } else if (!strcasecmp(cmd, "TYPE")) {
        ftp_reply(fd, "200 type set\r\n");
    } else if (!strcasecmp(cmd, "FEAT")) {
        ftp_reply(fd, "211-Features:\r\n SIZE\r\n PASV\r\n211 End\r\n");
    } else if (!strcasecmp(cmd, "NOOP")) {
        ftp_reply(fd, "200 ok\r\n");
    } else if (!strcasecmp(cmd, "QUIT")) {
        ftp_reply(fd, "221 bye\r\n");
        close(fd);
        g_client = -1;
    } else if (!strcasecmp(cmd, "PWD")) {
        char r[900];
        snprintf(r, sizeof(r), "257 \"%s\"\r\n", g_cwd);
        ftp_reply(fd, r);
    } else if (!strcasecmp(cmd, "CWD")) {
        char path[768];
        if (ftp_abspath(arg, path, sizeof(path)) != 0) {
            ftp_reply(fd, "550 path rejected\r\n");
        } else {
            DIR *d = opendir(path);
            if (!d) {
                ftp_reply(fd, "550 no such directory\r\n");
            } else {
                closedir(d);
                snprintf(g_cwd, sizeof(g_cwd), "%s", path);
                ftp_reply(fd, "250 ok\r\n");
            }
        }
    } else if (!strcasecmp(cmd, "CDUP")) {
        char *slash;
        snprintf(g_cwd, sizeof(g_cwd), "sdmc:/");
        (void)slash;
        ftp_reply(fd, "250 ok\r\n");
    } else if (!strcasecmp(cmd, "PASV")) {
        ftp_open_passive(fd);
    } else if (!strcasecmp(cmd, "LIST") || !strcasecmp(cmd, "NLST")) {
        ftp_list(fd);
    } else if (!strcasecmp(cmd, "SIZE")) {
        char path[768];
        struct stat st;
        char r[128];
        if (!arg || ftp_abspath(arg, path, sizeof(path)) != 0 || stat(path, &st) != 0) {
            ftp_reply(fd, "550 no such file\r\n");
        } else {
            snprintf(r, sizeof(r), "213 %ld\r\n", (long)st.st_size);
            ftp_reply(fd, r);
        }
    } else if (!strcasecmp(cmd, "DELE")) {
        char path[768];
        if (!arg || ftp_abspath(arg, path, sizeof(path)) != 0 || remove(path) != 0) {
            ftp_reply(fd, "550 cannot delete\r\n");
        } else {
            ftp_reply(fd, "250 deleted\r\n");
        }
    } else if (!strcasecmp(cmd, "MKD")) {
        char path[768];
        if (!arg || ftp_abspath(arg, path, sizeof(path)) != 0 || mkdir(path, 0777) != 0) {
            ftp_reply(fd, "550 cannot create\r\n");
        } else {
            ftp_reply(fd, "257 directory created\r\n");
        }
    } else if (!strcasecmp(cmd, "RMD")) {
        char path[768];
        if (!arg || ftp_abspath(arg, path, sizeof(path)) != 0 || rmdir(path) != 0) {
            ftp_reply(fd, "550 cannot remove\r\n");
        } else {
            ftp_reply(fd, "250 removed\r\n");
        }
    } else if (!strcasecmp(cmd, "RETR")) {
        char path[768];
        if (!arg || ftp_abspath(arg, path, sizeof(path)) != 0) {
            ftp_reply(fd, "550 path rejected\r\n");
        } else if (!(g_xfer_file = fopen(path, "rb"))) {
            ftp_reply(fd, "550 no such file\r\n");
        } else {
            fseek(g_xfer_file, 0, SEEK_END);
            g_xfer_left = ftell(g_xfer_file);
            fseek(g_xfer_file, 0, SEEK_SET);
            g_xfer_send = 1;
            ftp_reply(fd, "150 opening data connection\r\n");
        }
    } else if (!strcasecmp(cmd, "STOR")) {
        char path[768];
        if (!arg || ftp_abspath(arg, path, sizeof(path)) != 0) {
            ftp_reply(fd, "550 path rejected\r\n");
        } else if (!(g_xfer_file = fopen(path, "wb"))) {
            ftp_reply(fd, "550 cannot create file\r\n");
        } else {
            g_xfer_left = -1;   /* unknown length until the client closes */
            g_xfer_send = 0;
            ftp_reply(fd, "150 ready for data\r\n");
        }
    } else {
        ftp_reply(fd, "502 command not implemented\r\n");
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

void ds3ftp_init(void) {
    struct sockaddr_in addr;
    int opt = 1;

    if (g_listen >= 0) return;

    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (g_listen < 0) {
        g_ftp_last_error = errno;
        printf("[ftp] socket() failed: %d %s\n", errno, strerror(errno));
        return;
    }
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(FTP_PORT);
    if (bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(g_listen, FTP_BACKLOG) != 0) {
        printf("[ftp] bind/listen on %d failed: %d %s\n",
               FTP_PORT, errno, strerror(errno));
        g_ftp_last_error = errno;
        close(g_listen);
        g_listen = -1;
        return;
    }
    g_ftp_listen_fd = g_listen;
    fcntl(g_listen, F_SETFL, fcntl(g_listen, F_GETFL, 0) | O_NONBLOCK);
    printf("[ftp] listening on %d, root sdmc:/\n", FTP_PORT);
}

void ds3ftp_exit(void) {
    if (g_xfer_file) { fclose(g_xfer_file); g_xfer_file = NULL; }
    if (g_client >= 0) { close(g_client); g_client = -1; }
    if (g_listen >= 0) { close(g_listen); g_listen = -1; }
    g_ftp_listen_fd = -1;
}

void ds3ftp_poll(void) {
    if (g_listen < 0) return;

    if (g_client < 0) {
        int c = accept(g_listen, NULL, NULL);
        if (c >= 0) {
            fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) | O_NONBLOCK);
            g_client = c;
            g_line_len = 0;
            ftp_reply(c, "220 SimCity 3DS FTP (sd card)\r\n");
        }
        return;
    }

    /* A transfer in flight gets the whole poll, so the game still runs between
       chunks instead of the file blocking everything. */
    if (g_xfer_file) {
        if (g_data_fd < 0) g_data_fd = ftp_connect_data();
        int data_fd = g_data_fd;
        if (data_fd < 0) return;

        if (g_xfer_send) {
            size_t want = (size_t)(g_xfer_left < FTP_XFER_CHUNK ? g_xfer_left : FTP_XFER_CHUNK);
            size_t got = fread(g_rbuf, 1, want, g_xfer_file);
            if (got == 0) {
                close(data_fd); data_fd = -1;
                fclose(g_xfer_file); g_xfer_file = NULL;
                ftp_reply(g_client, "226 transfer complete\r\n");
                return;
            }
            send(data_fd, g_rbuf, got, 0);
            g_xfer_left -= (long)got;
        } else {
            ssize_t n = recv(data_fd, g_rbuf, sizeof(g_rbuf), 0);
            if (n <= 0) {
                close(data_fd); data_fd = -1;
                fclose(g_xfer_file); g_xfer_file = NULL;
                ftp_reply(g_client, "226 transfer complete\r\n");
                return;
            }
            fwrite(g_rbuf, 1, (size_t)n, g_xfer_file);
            fflush(g_xfer_file);
        }
        return;
    }

    char tmp[512];
    ssize_t n = recv(g_client, tmp, sizeof(tmp), 0);
    if (n <= 0) {
        close(g_client);
        g_client = -1;
        return;
    }
    for (ssize_t i = 0; i < n; i++) {
        char c = tmp[i];
        if (c == '\r') continue;
        if (c != '\n') {
            if (g_line_len < FTP_LINE_MAX - 1) g_line[g_line_len++] = c;
            continue;
        }
        if (g_line_len == 0) continue;
        g_line[g_line_len] = '\0';

        char *arg = strchr(g_line, ' ');
        if (arg) { *arg = '\0'; arg++; } else { arg = NULL; }

        ftp_exec(g_client, g_line, arg);
        g_line_len = 0;
    }
}