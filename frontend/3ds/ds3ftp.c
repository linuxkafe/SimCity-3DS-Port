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
/* Control-channel output is queued, never sent inline. A blocking send() in
   the main loop is how the app died: a client that stops reading wedges send(),
   the whole loop stalls, and the 3DS watchdog closes the process. The queue is
   drained a bounded amount per poll so one slow reader cannot monopolise the
   frame either. If a client falls far enough behind to fill it, it is dropped —
   a wedged transfer matters less than a wedged game. */
#define FTP_OUT_MAX  8192
static char  g_out[FTP_OUT_MAX];
static size_t g_out_len = 0;

static char  g_cwd[768] = "sdmc:/";
static char  g_line[FTP_LINE_MAX];
static size_t g_line_len = 0;
static char  g_rbuf[FTP_XFER_CHUNK];
static int   g_pasv_fd = -1;   /* passive-mode listening socket */
static int   g_data_fd = -1;     /* accepted data connection */

static void ftp_out(const char *s, size_t n) {
    if (n > FTP_OUT_MAX - g_out_len) {
        g_out_len = 0;   /* client is hopelessly behind; drop its output */
    }
    memcpy(g_out + g_out_len, s, n);
    g_out_len += n;
}

static void ftp_reply(int fd, const char *s) {
    (void)fd;
    ftp_out(s, strlen(s));
}

static void ftp_flush(void) {
    size_t budget = 2048;
    while (g_out_len > 0 && budget > 0) {
        ssize_t n = send(g_client, g_out, g_out_len, 0);
        if (n > 0) {
            memmove(g_out, g_out + n, g_out_len - (size_t)n);
            g_out_len -= (size_t)n;
            budget -= (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return;                      /* peer is behind; try again next poll */
        } else {
            g_out_len = 0;               /* real error: give up on the output */
            return;
        }
    }
}

/* ── Path handling ─────────────────────────────────────────────────── */

/* Build an absolute card path from the current directory plus a client-supplied
 * argument. Rejects anything that could climb out of the card: "..", absolute
 * paths, and NUL. Returns 0 on success. */
/* The 3DS filesystem service is inconsistent about directory paths: fopen on
   "sdmc:/3ds/simcity.3dsx" works, while opendir on "sdmc:/3ds" fails. Trying
   the trailing-slash form first and falling back covers both without having to
   know which way this particular path happens to round-trip. */
static DIR *ftp_opendir(const char *path) {
    char with_slash[800];
    DIR *d = opendir(path);
    if (d) return d;
    snprintf(with_slash, sizeof(with_slash), "%s/", path);
    return opendir(with_slash);
}

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
    d = ftp_opendir(path);
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
        ftp_out(line, strlen(line));
    }
    closedir(d);
    ftp_reply(fd, "226 done\r\n");
}

/* ── Commands ──────────────────────────────────────────────────────── */

/* In a passive transfer the SERVER listens on the data port and the CLIENT
   connects to it. The previous version created a listening socket here and then
   threw it away, and later "connected" a fresh socket to it as though the server
   were the client. curl received 227, dialled the advertised port, and the data
   never arrived because nothing ever accepted the inbound connection.

   So: keep the listening socket, and accept on it (in ds3ftp_poll), not
   connect to it. */
static void ftp_open_passive(int fd) {
    struct sockaddr_in self;
    socklen_t slen = sizeof(self);
    int pfd;
    char reply[128];

    if (g_pasv_fd >= 0) { close(g_pasv_fd); g_pasv_fd = -1; }

    if (getsockname(fd, (struct sockaddr *)&self, &slen) != 0) {
        ftp_reply(fd, "425 cannot determine port\r\n");
        return;
    }
    pfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (pfd < 0) { ftp_reply(fd, "425 cannot open data socket\r\n"); return; }

    /* soc:u will not bind port 0, so use a fixed port above the control port.
       The server is single-client by contract, so a fixed port costs nothing. */
    struct sockaddr_in pa;
    memset(&pa, 0, sizeof(pa));
    pa.sin_family = AF_INET;
    pa.sin_addr.s_addr = htonl(INADDR_ANY);
    pa.sin_port = htons((uint16_t)(FTP_PORT + 1));
    if (bind(pfd, (struct sockaddr *)&pa, sizeof(pa)) != 0 || listen(pfd, 1) != 0) {
        close(pfd);
        ftp_reply(fd, "425 cannot bind data socket\r\n");
        return;
    }
    fcntl(pfd, F_SETFL, fcntl(pfd, F_GETFL, 0) | O_NONBLOCK);

    /* Advertise the address the client can actually reach us on — the address
       of the control connection, not 127.0.0.1. Telling a remote client to
       dial loopback is why the very first PASV attempt produced a connection to
       the wrong place entirely. */
    snprintf(reply, sizeof(reply), "227 entering passive mode (%d,%d,%d,%d,%d,%d)\r\n",
             (unsigned)((self.sin_addr.s_addr >> 24) & 0xFF),
             (unsigned)((self.sin_addr.s_addr >> 16) & 0xFF),
             (unsigned)((self.sin_addr.s_addr >> 8) & 0xFF),
             (unsigned)(self.sin_addr.s_addr & 0xFF),
             (unsigned)((FTP_PORT + 1) >> 8),
             (unsigned)((FTP_PORT + 1) & 0xFF));
    g_pasv_fd = pfd;
    ftp_reply(fd, reply);
}

/* Accept the client's inbound data connection, non-blocking: returns -1 while
   the client has not dialled yet, which is normal and must not be fatal. */
static int ftp_accept_data(void) {
    int c;
    if (g_pasv_fd < 0) return -1;
    c = accept(g_pasv_fd, NULL, NULL);
    if (c < 0) return -1;
    fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) | O_NONBLOCK);
    close(g_pasv_fd);
    g_pasv_fd = -1;
    return c;
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
            DIR *d = ftp_opendir(path);
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

bool ds3ftp_init(void) {
    struct sockaddr_in addr;
    int opt = 1;

    if (g_listen >= 0) return true;

    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (g_listen < 0) {
        g_ftp_last_error = errno;
        printf("[ftp] socket() failed: %d %s\n", errno, strerror(errno));
        return false;
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
        return false;
    }
    g_ftp_listen_fd = g_listen;
    fcntl(g_listen, F_SETFL, fcntl(g_listen, F_GETFL, 0) | O_NONBLOCK);
    printf("[ftp] listening on %d, root sdmc:/\n", FTP_PORT);
    return true;
}

void ds3ftp_exit(void) {
    if (g_xfer_file) { fclose(g_xfer_file); g_xfer_file = NULL; }
    if (g_client >= 0) { close(g_client); g_client = -1; }
    if (g_pasv_fd  >= 0) { close(g_pasv_fd);  g_pasv_fd  = -1; }
    if (g_data_fd  >= 0) { close(g_data_fd);  g_data_fd  = -1; }
    if (g_listen    >= 0) { close(g_listen);    g_listen    = -1; }
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
            g_out_len = 0;
            if (g_pasv_fd >= 0) { close(g_pasv_fd); g_pasv_fd = -1; }
            ftp_reply(c, "220 SimCity 3DS FTP (sd card)\r\n");
            ftp_flush();
        }
        return;
    }
    ftp_flush();

    /* A transfer in flight gets the whole poll, so the game still runs between
       chunks instead of the file blocking everything. */
    if (g_xfer_file) {
        if (g_data_fd < 0) g_data_fd = ftp_accept_data();
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
            ssize_t sent = send(data_fd, g_rbuf, got, 0);
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                /* Would block: leave the bytes in the file and retry. Rewinding
                   is safe because the file offset has not moved past them yet. */
                fseek(g_xfer_file, -(long)got, SEEK_CUR);
                return;
            }
            if (sent <= 0) {
                close(data_fd); g_data_fd = -1;
                fclose(g_xfer_file); g_xfer_file = NULL;
                ftp_reply(g_client, "426 data connection lost\r\n");
                return;
            }
            if ((size_t)sent < got) {
                fseek(g_xfer_file, (long)((size_t)got - (size_t)sent), SEEK_CUR);
            }
            g_xfer_left -= (long)sent;
        } else {
            ssize_t n = recv(data_fd, g_rbuf, sizeof(g_rbuf), 0);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                return;
            }
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
    /* n < 0/EAGAIN just means the client has not typed yet. Closing on it is
       what made every connection die right after the banner. */
    if (n == 0) {
        close(g_client);
        g_client = -1;
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
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
        ftp_flush();
    }
}