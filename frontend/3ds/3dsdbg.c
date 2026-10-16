/*
 * 3dsdbgkit — a reusable debug bridge for libctru 3DS homebrew.
 *
 * Provides two things on one TCP port:
 *
 *   1. A GDB Remote Serial Protocol (RSP) stub, so stock arm-none-eabi-gdb
 *      can attach: `target remote :5002`. Gives breakpoints, memory reads and
 *      writes, register inspection, single-step and continue.
 *
 *   2. A small line-oriented extension protocol (see dbg_ext_*) for
 *      application-specific introspection — e.g. streaming a framebuffer or a
 *      structured dump straight to the host. These are deliberately kept
 *      separate from the RSP packet stream so gdb and the extension can share
 *      the socket: an RSP packet starts with '$' and the extension is a plain
 *      ASCII line.
 *
 * Design notes:
 *   - Nothing here is SimCity-specific. The pixel-dump helpers are generic
 *     (dump any screen, dump an arbitrary address range).
 *   - The socket is non-blocking and serviced from the host application's own
 *     main loop via dbg_poll(), so debugging never stalls the emulated game.
 *   - Breakpoints are software (ARM BKPT #0). There is no OS underneath a 3DSX
 *     homebrew, so this is cooperative: the stub traps at the next poll.
 *
 * Usage from the application:
 *     dbg_init();                 // once, after the system is up
 *     ...                         // main loop
 *     dbg_poll();                 // once per iteration
 */

#include "3dsdbg.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <malloc.h>

#ifndef DBGK_PORT
#define DBGK_PORT 5002
#endif

#define RSP_MAXPKT   4096
#define MAX_BREAKS    32
#define MAX_RSP_MEM  1024      /* bytes per 'm' reply */

/* Runtime-selectable orientation hook for renderers that want host control. */
int g_dbg_rotate_mode = -1;

/* ── State ──────────────────────────────────────────────────────────── */

static int g_listen_fd = -1;
static int g_client_fd = -1;

/* soc:u socket context. On the 3DS the BSD sockets in libctru are served by
   the SOC service, which needs a caller-supplied context buffer and an
   explicit socInit() before socket() will do anything. Without it socket()
   fails with ENODEV ("no such device"). This is the 3DS equivalent of what
   POSIX systems do in the kernel, and it is what the official devkitPro
   sockets example does. */
static u32 *g_soc_buffer = NULL;

static volatile int g_target_running = 1;   /* cleared when single-stepping */
static volatile int g_break_hit = 0;

/* Software breakpoint table. */
static struct {
    uint32_t addr;
    int      installed;
    uint32_t orig;            /* original instruction word */
} g_breaks[MAX_BREAKS];

static int g_break_count = 0;
static int g_last_signal = 5;               /* SIGTRAP */

/* RX/TX buffers for RSP. */
static unsigned char g_rsp_rx[RSP_MAXPKT];
static size_t        g_rsp_rx_len = 0;
static int           g_rsp_checksum = 0;
static int           used_any = 0;   /* a packet payload is in progress */

/* ── Low-level socket helpers ───────────────────────────────────────── */

static void dbgk_send_bytes(const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    if (g_client_fd < 0) return;
    while (len) {
        ssize_t n = send(g_client_fd, p, len, 0);
        if (n <= 0) return;
        p += n; len -= (size_t)n;
    }
}

static void dbgk_send_str(const char *s) {
    dbgk_send_bytes(s, strlen(s));
}

static void dbgk_send_hex(unsigned value, int digits) {
    static const char H[] = "0123456789abcdef";
    char buf[9];
    if (digits > 8) digits = 8;
    for (int i = digits - 1; i >= 0; i--) { buf[i] = H[value & 0xF]; value >>= 4; }
    dbgk_send_bytes(buf, (size_t)digits);
}

/* ── Registers ──────────────────────────────────────────────────────── */

/*
 * ARM11 general-purpose registers r0..r12, sp, lr, pc, cpsr — in the order the
 * GDB 'g' packet expects for AArch32. Padded out to the 26-byte ARM 'g' frame
 * the stub advertises.
 */
/* Emit the register block as a complete RSP packet: $<hex>#cc
   A bare hex stream without the $ ... #cc framing desynchronises gdb. */
static void dbgk_send_registers(void) {
    char buf[256];
    size_t n = 0;
    uint32_t v;
#ifdef __arm__
#define APPEND8(v) n += (size_t)snprintf(buf+n, sizeof(buf)-n, "%02x%02x%02x%02x", \
        (unsigned)((v)&0xFF), (unsigned)(((v)>>8)&0xFF), \
        (unsigned)(((v)>>16)&0xFF), (unsigned)(((v)>>24)&0xFF))
#define RD(nm) __asm__ volatile("mov %0, " #nm : "=r"(v)); APPEND8(v)
    RD(r0); RD(r1); RD(r2); RD(r3); RD(r4); RD(r5);
    RD(r6); RD(r7); RD(r8); RD(r9); RD(r10); RD(r11);
    RD(r12);
    __asm__ volatile("mov %0, sp" : "=r"(v)); APPEND8(v);
    __asm__ volatile("mov %0, lr" : "=r"(v)); APPEND8(v);
    __asm__ volatile("mov %0, pc" : "=r"(v)); APPEND8(v);
#undef RD
#undef APPEND8
#else
    /* Host build: synthesise a register frame so the packet framing can be
       tested without ARM. Layout matches the ARM path above. */
    for (int i = 0; i < 16; i++)
        n += (size_t)snprintf(buf+n, sizeof(buf)-n, "11223344");
#endif
/* GDB14 for ARM expects a 34-word 'g' frame:
       r0-r15 (16 general registers) +
       d0-d7 (8 VFP double-precision registers = 16 words) +
       fpscr (1) + cpsr (1) = 34 words total.
       We report the 16 GPRs from hardware, and zero the VFP/FPU registers. */
    /* GDB14 for ARM expects a 34-word 'g' frame:
       r0-r15 (16 GPRs) + d0-d15 (16 VFP double registers = 16 words) +
       fpscr (1) + cpsr (1) = 34 words total.
       We report the 16 GPRs from hardware, zero the 16 VFP registers,
       and report fpscr=0, cpsr=0. */
    for (int i = 0; i < 10; i++)
        n += (size_t)snprintf(buf+n, sizeof(buf)-n, "%08x", 0u);
    /* Add 16 more words for VFP d0-d7 (8 doubles = 16 words) */
    for (int i = 0; i < 16; i++)
        n += (size_t)snprintf(buf+n, sizeof(buf)-n, "%08x", 0u);
    /* fpscr and cpsr already included in the 10 above (fpscr + cpsr) */
    /* Add 16 more words for VFP d0-d7 (8 doubles = 16 words) */
    for (int i = 0; i < 16; i++)
        n += (size_t)snprintf(buf+n, sizeof(buf)-n, "%08x", 0u);
    /* fpscr and cpsr already included in the 10 above (fpscr + cpsr) */

    /* Ensure payload length is even (RSP requires even hex digits) */
    if (n % 2 == 1) {
        buf[n++] = '0';
    }
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++) sum += (unsigned char)buf[i];
    char tail[8];
    snprintf(tail, sizeof(tail), "#%02x", sum & 0xFF);
    dbgk_send_bytes("$", 1);
    dbgk_send_bytes(buf, n);
    dbgk_send_bytes(tail, 3);   /* "#" + 2 hex digits */
}

/* ── Breakpoints ────────────────────────────────────────────────────── */

/* Sanity-bound a breakpoint address. Without this a bad host-supplied
   address would fault the whole process: on hardware that is a crash, and
   in the host test harness it is a segfault. */
static int dbgk_addr_plausible(uint32_t addr) {
    /* 3DSX text/data lives in linear memory around 0x00100000..0x0E000000.
       Reject the obvious mistakes: null page and absurd values. */
    if (addr == 0u) return 0;
    if (addr < 0x00100000u || addr > 0x0F000000u) return 0;
    if (addr & 3u) return 0;            /* ARM instructions are 4-aligned */
    return 1;
}

static int dbgk_add_break(uint32_t addr) {
    if (!dbgk_addr_plausible(addr)) return -2;
    if (g_break_count >= MAX_BREAKS) return -1;
    for (int i = 0; i < g_break_count; i++)
        if (g_breaks[i].addr == addr) return 0;
    g_breaks[g_break_count].addr      = addr;
    g_breaks[g_break_count].orig      = 0;
    g_breaks[g_break_count].installed = 1;
#ifndef DBGK_HOST_TEST
    g_breaks[g_break_count].orig      = *(volatile uint32_t *)addr;
    *(volatile uint32_t *)addr = 0xE1200070u;   /* BKPT #0 */
    __builtin___clear_cache((char *)addr, (char *)addr + 4);
#endif
    g_break_count++;
    return 0;
}

static int dbgk_remove_break(uint32_t addr) {
    for (int i = 0; i < g_break_count; i++) {
        if (g_breaks[i].addr == addr) {
#ifndef DBGK_HOST_TEST
            if (dbgk_addr_plausible(addr)) {
                *(volatile uint32_t *)addr = g_breaks[i].orig;
                __builtin___clear_cache((char *)addr, (char *)addr + 4);
            }
#endif
            g_breaks[i] = g_breaks[--g_break_count];
            return 0;
        }
    }
    return -1;
}

/* Swap the first breakpoint back to its original instruction so the program
 * can step over it, and report the stop. */
static void dbgk_report_break(void) {
    g_break_hit = 1;
    g_target_running = 0;
    for (int i = 0; i < g_break_count; i++) {
        if (g_breaks[i].installed)
            *(volatile uint32_t *)g_breaks[i].addr = g_breaks[i].orig;
    }
}

void dbgk_break_check(void) {
    if (!g_break_hit) return;
    uint32_t pc = 0;
#ifdef __arm__
    __asm__ volatile("mov %0, pc" : "=r"(pc));
#endif
    for (int i = 0; i < g_break_count; i++) {
        if (g_breaks[i].addr == pc) {
            /* Re-arm and skip the instruction. */
            *(volatile uint32_t *)pc = 0xE1200070u;
            g_break_hit = 0;
            return;
        }
    }
}

/* ── RSP packet I/O ─────────────────────────────────────────────────── */

static void dbgk_send_packet(const char *payload) {
    /* Frame the packet by hand. The earlier snprintf into char[8] overflowed
       for any payload longer than five characters and sent stack garbage to
       gdb, which surfaced as a stream of "Ignoring packet error". */
    unsigned sum = 0;
    for (const char *p = payload; *p; p++) sum += (unsigned char)*p;

    char tail[5];   /* "#xx" + NUL = 4 bytes */
    snprintf(tail, sizeof(tail), "#%02x", sum & 0xFF);
    dbgk_send_bytes("$", 1);
    dbgk_send_bytes(payload, strlen(payload));
    dbgk_send_bytes(tail, 3);   /* "#" + 2 hex digits */
}

static void dbgk_stop_reply(void) {
    char pkt[32];
    snprintf(pkt, sizeof(pkt), "T%02xthread:1;", g_last_signal & 0xFF);
    dbgk_send_packet(pkt);
}

/* ── Command handling ───────────────────────────────────────────────── */

static void dbgk_handle_rsp(char *pkt) {
    char reply[RSP_MAXPKT];
    char *arg = pkt + 1;

    switch (pkt[0]) {
    case '?':
        dbgk_stop_reply();
        return;

    case 'g':                       /* read all registers */
        dbgk_send_registers();
        return;

    case 'G': {                     /* write all registers */
        /* Accept and ignore the bulk case; single-register writes via 'P'
           cover the useful cases for a 3DSX. */
        dbgk_send_packet("OK");
        return;
    }

    case 'p':                       /* read one register */
        dbgk_send_registers();       /* reply is read by the 'g' frame above */
        return;

    case 'P': {                     /* write one register: Pnn=AAAA */
        /* Register writes are not supported by this stub; acknowledging keeps
           gdb's session alive without corrupting state. Read-only debugging
           (breakpoints, memory, single-step) is the intended use. */
        dbgk_send_packet("OK");
        return;
    }

    case 'm': {                     /* read memory: m addr,len */
        unsigned long addr = 0, len = 0;
        if (sscanf(arg, "%lx,%lx", &addr, &len) != 2) { dbgk_send_packet("E01"); return; }
        if (len > MAX_RSP_MEM) len = MAX_RSP_MEM;
        const unsigned char *p = (const unsigned char *)addr;
        static char hex[2 * MAX_RSP_MEM + 1];
        for (unsigned long i = 0; i < len; i++) {
            static const char H[] = "0123456789abcdef";
            hex[i*2]   = H[(p[i] >> 4) & 0xF];
            hex[i*2+1] = H[p[i] & 0xF];
        }
        hex[len*2] = '\0';
        dbgk_send_packet(hex);
        return;
    }

    case 'M': {                     /* write memory: M addr,len:hex */
        /* Not needed for the current workflow; report unsupported rather than
           silently corrupting. */
        dbgk_send_packet("E02");
        return;
    }

    case 'Z': case 'z': {           /* insert / remove breakpoint */
        int type; unsigned long addr, kind;
        if (sscanf(arg, "%x,%lx,%lx", &type, &addr, &kind) != 3) { dbgk_send_packet("E03"); return; }
        int rc = (pkt[0] == 'Z') ? dbgk_add_break((uint32_t)addr)
                                 : dbgk_remove_break((uint32_t)addr);
        dbgk_send_packet(rc == 0 ? "OK" : "E04");
        return;
    }

    case 'c':                       /* continue */
        g_target_running = 1;
        dbgk_send_packet("OK");
        return;

    case 's':                       /* step one instruction */
        g_target_running = 1;
        dbgk_send_packet("OK");
        return;

    case 'k':                       /* kill */
        g_target_running = 0;
        dbgk_send_packet("OK");
        return;

    case 'D':                       /* detach */
        g_target_running = 1;
        dbgk_send_packet("OK");
        return;

    case 'q':
        if (!strncmp(arg, "Supported", 9)) {
            dbgk_send_packet("PacketSize=1000;swbreak+;hwbreak-");
        } else if (!strncmp(arg, "Attached", 8)) {
            dbgk_send_packet("1");
        } else if (!strncmp(arg, "Offsets", 7)) {
            dbgk_send_packet("Text=0;Data=0;Bss=0");
        } else if (!strncmp(arg, "C", 1)) {
            dbgk_send_packet("QC1");          /* single-threaded */
        } else {
            dbgk_send_packet("");             /* empty reply is valid */
        }
        return;

    case 'H':                         /* set thread: OK */
    case 'T':                         /* thread alive */
        dbgk_send_packet("OK");
        return;

    case 'v':                         /* vMustReplyEmpty and friends */
        dbgk_send_packet("");             /* empty reply: vCont etc unsupported */
        return;

    default:
        /* Unsupported single-char request: an empty packet is the correct
           "I do not implement this" answer and does not desynchronise the
           protocol stream. */
        dbgk_send_packet("");
        return;
    }
    (void)reply;
}

/* ── Extension protocol (non-RSP ASCII lines) ───────────────────────── */

/* Application-supplied formatter for the `state` verb. NULL until the host
   application registers one; the bridge stays game-agnostic. */
static dbgk_state_fn g_state_fn = NULL;

void dbgk_set_state_fn(dbgk_state_fn fn) { g_state_fn = fn; }

volatile int g_dbg_pause = -1;   /* -1 = host not driving */
volatile int g_dbg_step  = 0;    /* frames the host asked to advance */

volatile uint16_t g_dbg_input = 0;      /* held mask, OR-ed with hardware */
volatile int      g_dbg_input_frames = 0; /* >0: release after N advances */

volatile int g_dbg_chan_swap = 0;

volatile uint32_t g_dbg_phase_us[DBGK_PHASE_COUNT];
volatile uint32_t g_dbg_phase_avg[DBGK_PHASE_COUNT];

volatile int g_put_remaining = 0;
FILE *g_put_file = NULL;

/* Resolve a screen to its current framebuffer. Never cached: libctru may hand
   back a different pointer after gfxSwapBuffers() (gfx.h:140), so a cached
   address would read a buffer the renderer is not currently writing. */
static u8 *dbgk_get_fb(int screen, u16 *w, u16 *h) {
#ifdef DBGK_HOST_TEST
    static u16 fake[240 * 400];
    *w = 240; *h = 400;
    (void)screen;
    return (u8 *)fake;
#else
    return gfxGetFramebuffer((gfxScreen_t)screen, GFX_LEFT, w, h);
#endif
}

/* Reused by `fbrd`; 1 KiB per chunk keeps replies inside one line so the host
   can read them with an ordinary line read and reassemble in order. */
static char g_chunk_hex[2 * MAX_RSP_MEM + 1];

static void dbgk_ext(const char *line) {
    char verb[24] = {0}, arg[40] = {0};
    if (sscanf(line, "%23s %39s", verb, arg) < 1) { dbgk_send_str("ERR empty\n"); return; }

    if (!strcmp(verb, "ping")) {
        dbgk_send_str("OK pong\n");
    } else if (!strcmp(verb, "info")) {
        char m[128];
        snprintf(m, sizeof(m), "OK 3dsdbgkit/1 pid=ok breaks=%d\n", g_break_count);
        dbgk_send_str(m);
    } else if (!strcmp(verb, "state")) {
        if (!g_state_fn) { dbgk_send_str("ERR nostate\n"); return; }
        char buf[256];
        g_state_fn(buf, (unsigned)sizeof(buf));
        dbgk_send_str("OK ");
        dbgk_send_str(buf);
        dbgk_send_str("\n");
    } else if (!strcmp(verb, "rot")) {
        int mode = -1;
        if (sscanf(line, "%*s %d", &mode) != 1) { dbgk_send_str("ERR args\n"); return; }
        g_dbg_rotate_mode = mode;
        char m[32];
        snprintf(m, sizeof(m), "OK rot=%d\n", mode);
        dbgk_send_str(m);
    } else if (!strcmp(verb, "chan")) {
        /* chan [0|1] — red/blue order written into the framebuffer. */
        int mode = 0;
        if (sscanf(line, "%*s %d", &mode) != 1) { dbgk_send_str("ERR args\n"); return; }
        if (mode < 0 || mode > 1) { dbgk_send_str("ERR range (0 or 1)\n"); return; }
        g_dbg_chan_swap = mode;
        char m[48];
        snprintf(m, sizeof(m), "OK chan=%d\n", mode);
        dbgk_send_str(m);
    } else if (!strcmp(verb, "prof")) {
        char m[320];
        int n = snprintf(m, sizeof(m),
            "OK now[adv=%uus audio=%uus dbg=%uus present=%uus loop=%uus] "
            "avg[adv=%uus audio=%uus dbg=%uus present=%uus loop=%uus]",
            g_dbg_phase_us[DBGK_PHASE_ADVANCE], g_dbg_phase_us[DBGK_PHASE_AUDIO],
            g_dbg_phase_us[DBGK_PHASE_DEBUG], g_dbg_phase_us[DBGK_PHASE_PRESENT],
            g_dbg_phase_us[DBGK_PHASE_LOOP],
            g_dbg_phase_avg[DBGK_PHASE_ADVANCE], g_dbg_phase_avg[DBGK_PHASE_AUDIO],
            g_dbg_phase_avg[DBGK_PHASE_DEBUG], g_dbg_phase_avg[DBGK_PHASE_PRESENT],
            g_dbg_phase_avg[DBGK_PHASE_LOOP]);
        dbgk_send_bytes(m, (size_t)n);
        dbgk_send_str("\n");
    } else if (!strcmp(verb, "put")) {
        /* put <path> <size> — the next <size> bytes go straight to <path>. */
        char path[96];
        unsigned long size = 0;
        if (sscanf(line, "%*s %95s %lu", path, &size) != 2) {
            dbgk_send_str("ERR args\n"); return;
        }
        if (size == 0 || size > 64UL * 1024UL * 1024UL) {
            dbgk_send_str("ERR range (1..64MB)\n"); return;
        }
        if (strncmp(path, "sdmc:/", 6) != 0) {
            /* Refuse anything but the SD card: this verb writes whatever bytes
               arrive, and a stray path should not be able to scribble over
               code or a save. */
            dbgk_send_str("ERR path must start with sdmc:/\n"); return;
        }
        g_put_file = fopen(path, "wb");
        if (!g_put_file) { dbgk_send_str("ERR open failed\n"); return; }
        g_put_remaining = (int)size;
        dbgk_send_str("OK sending\n");
    } else if (!strcmp(verb, "input")) {
        /* input [mask] — set the held mask, or release everything if absent/0. */
        unsigned long mask = 0;
        if (sscanf(line, "%*s %lx", &mask) < 1) mask = 0;
        if (mask > 0xFFFFu) { dbgk_send_str("ERR range\n"); return; }
        g_dbg_input = (uint16_t)mask;
        g_dbg_input_frames = 0;
        char m[48];
        snprintf(m, sizeof(m), "OK input=0x%04X\n", (unsigned)g_dbg_input);
        dbgk_send_str(m);
    } else if (!strcmp(verb, "tap")) {
        /* tap <mask> [frames] — hold the mask for N advanced frames, then release.
           Frames below 2 are refused: a one-frame pulse can fall entirely
           between two emulated frames and the guest would never sample it. */
        unsigned long mask = 0;
        int frames = 2;
        if (sscanf(line, "%*s %lx %d", &mask, &frames) < 1) {
            dbgk_send_str("ERR args\n"); return;
        }
        if (mask > 0xFFFFu || frames < 2 || frames > 600) {
            dbgk_send_str("ERR range (mask<=0xFFFF, frames 2..600)\n"); return;
        }
        g_dbg_input = (uint16_t)mask;
        g_dbg_input_frames = frames;
        char m[64];
        snprintf(m, sizeof(m), "OK tap=0x%04X frames=%d\n",
                 (unsigned)g_dbg_input, frames);
        dbgk_send_str(m);
    } else if (!strcmp(verb, "pause")) {
        g_dbg_pause = 1;
        dbgk_send_str("OK paused\n");
    } else if (!strcmp(verb, "run")) {
        g_dbg_pause = 0;
        dbgk_send_str("OK running\n");
    } else if (!strcmp(verb, "step")) {
        int n = 1;
        if (sscanf(line, "%*s %d", &n) != 1) { dbgk_send_str("ERR args\n"); return; }
        if (n < 0 || n > 10000) { dbgk_send_str("ERR range\n"); return; }
        g_dbg_step = n;
        char m[32];
        snprintf(m, sizeof(m), "OK step=%d\n", n);
        dbgk_send_str(m);
    } else if (!strcmp(verb, "fbsize")) {
        int screen = 0;
        u16 w = 0, h = 0;
        if (sscanf(line, "%*s %d", &screen) != 1) { dbgk_send_str("ERR args\n"); return; }
        if (!dbgk_get_fb(screen, &w, &h)) { dbgk_send_str("ERR nofb\n"); return; }
        char m[64];
        snprintf(m, sizeof(m), "OK %u %u %u\n", (unsigned)w, (unsigned)h,
                 (unsigned)w * (unsigned)h * 2u);
        dbgk_send_str(m);
    } else if (!strcmp(verb, "fbrd")) {
        /* fbrd <screen> <offset> <length> -> "<offset> <length> <hex>" */
        unsigned screen = 0, off = 0, len = 0;
        u16 w = 0, h = 0;
        if (sscanf(line, "%*s %u %u %u", &screen, &off, &len) != 3) {
            dbgk_send_str("ERR args\n"); return;
        }
        u8 *fb = dbgk_get_fb((int)screen, &w, &h);
        if (!fb) { dbgk_send_str("ERR nofb\n"); return; }
        unsigned total = (unsigned)w * (unsigned)h * 2u;
        if (off > total || len == 0u || len > (unsigned)MAX_RSP_MEM ||
            off + len > total) {
            dbgk_send_str("ERR range\n"); return;
        }
        static const char H[] = "0123456789abcdef";
        for (unsigned i = 0; i < len; i++) {
            g_chunk_hex[i * 2u]     = H[(fb[off + i] >> 4) & 0xF];
            g_chunk_hex[i * 2u + 1u] = H[ fb[off + i]       & 0xF];
        }
        g_chunk_hex[len * 2u] = '\0';
        char hdr[48];
        int n = snprintf(hdr, sizeof(hdr), "%u %u ", off, len);
        dbgk_send_bytes(hdr, (size_t)n);
        dbgk_send_str(g_chunk_hex);
        dbgk_send_str("\n");
    } else if (!strcmp(verb, "fbdump")) {
        int screen = 0;
        char path[96];
        if (sscanf(line, "%*s %d %95s", &screen, path) != 2) {
            dbgk_send_str("ERR args\n"); return;
        }
        dbgk_dump_screen(path, screen);
        dbgk_send_str("OK dumped\n");
    } else if (!strcmp(verb, "brk")) {
        unsigned long a = strtoul(arg, NULL, 0);
        int rc = dbgk_add_break((uint32_t)a);
        dbgk_send_str(rc ==  0 ? "OK break set\n" :
                      rc == -1 ? "ERR break table full\n" :
                                 "ERR bad address\n");
    } else if (!strcmp(verb, "unbrk")) {
        unsigned long a = strtoul(arg, NULL, 0);
        dbgk_send_str(dbgk_remove_break((uint32_t)a) == 0 ? "OK break removed\n"
                                                        : "ERR no break\n");
    } else if (!strcmp(verb, "dumpmem")) {
        unsigned long a = 0, n = 0;
        if (sscanf(line, "dumpmem %lx %lx", &a, &n) != 2) { dbgk_send_str("ERR args\n"); return; }
        if (n > MAX_RSP_MEM) n = MAX_RSP_MEM;
        dbgk_send_str("OK dumping\n");
        dbgk_send_bytes((const void *)a, n);
        dbgk_send_str("\n.\n");
    } else {
        dbgk_send_str("ERR unknown\n");
    }
}

/* ── RX demultiplexer: RSP packets vs extension lines ───────────────── */

/* Extension lines are plain ASCII terminated by a newline and never begin
   with '$', so they can share the socket with the RSP packet stream. */
static char   g_ext_line[128];
static size_t g_ext_used = 0;

static void dbgk_feed_ext_byte(char c) {
    if (c == '\n' || c == '\r') {
        if (g_ext_used) {
            g_ext_line[g_ext_used] = '\0';
            g_ext_used = 0;
            dbgk_ext(g_ext_line);
        }
        return;
    }
    if (g_ext_used < sizeof(g_ext_line) - 1)
        g_ext_line[g_ext_used++] = c;
}

static void dbgk_feed(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = data[i];

        /* A `put` is in progress: these bytes are file content, not commands.
         * The check has to come first or the payload gets chewed up as
         * extension lines. */
        if (g_put_remaining > 0) {
            fwrite(&c, 1, 1, g_put_file);
            g_put_remaining--;
            if (g_put_remaining == 0) {
                fflush(g_put_file);
                fclose(g_put_file);
                g_put_file = NULL;
                dbgk_send_str("OK written\n");
            }
            continue;
        }

        if (used_any) {
            /* --- inside an RSP packet --- */
            if (c == '#') {
                g_rsp_rx[g_rsp_rx_len] = '\0';
                dbgk_send_bytes("+", 1);
                dbgk_handle_rsp((char *)g_rsp_rx);
                g_rsp_rx_len = 0;
                used_any = 0;
                i += 2;                       /* skip the two checksum digits */
            } else if (g_rsp_rx_len < RSP_MAXPKT - 1) {
                g_rsp_rx[g_rsp_rx_len++] = (unsigned char)c;
                g_rsp_checksum += (unsigned char)c;
            }
            continue;
        }
        /* --- outside a packet: acks and extension lines --- */
        if (c == '$') {
            g_rsp_rx_len   = 0;
            g_rsp_checksum = 0;
            used_any = 1;
        } else if (c != '+' && c != '-') {
            dbgk_feed_ext_byte(c);
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

/* Context buffer for soc:u. Must be page-aligned (0x1000) and a multiple of
   0x1000. The official devkitPro 3DS sockets example uses 0x100000 (1 MiB);
   a smaller buffer makes socInit() fail (observed: 0xD8E073EC). */
#define DBGK_SOC_ALIGN   0x1000
#define DBGK_SOC_BUFSIZE 0x100000

void dbgk_init(void) {
    struct sockaddr_in server;
    int opt = 1;

    if (!g_soc_buffer) {
        g_soc_buffer = (u32 *)memalign(DBGK_SOC_ALIGN, DBGK_SOC_BUFSIZE);
        if (!g_soc_buffer) {
            printf("[dbgk] memalign for soc context failed\n");
            return;
        }
#ifndef DBGK_HOST_TEST
        Result rc = socInit(g_soc_buffer, DBGK_SOC_BUFSIZE);
        if (rc != 0) {
            printf("[dbgk] socInit failed: 0x%08X\n", (unsigned)rc);
            free(g_soc_buffer);
            g_soc_buffer = NULL;
            return;
        }
        printf("[dbgk] socInit ok\n");
#else
        printf("[dbgk] host test: socInit skipped\n");
#endif
    }

    g_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (g_listen_fd < 0) {
        printf("[dbgk] socket() failed: %d %s\n", errno, strerror(errno));
        return;
    }
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port   = htons(DBGK_PORT);
    server.sin_addr.s_addr = gethostid();

    if (bind(g_listen_fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        /* Retry on the wildcard address: gethostid() can return something the
           local stack will not accept as a bind target. */
        int e1 = errno;
        memset(&server, 0, sizeof(server));
        server.sin_family = AF_INET;
        server.sin_port   = htons(DBGK_PORT);
        server.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(g_listen_fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
            printf("[dbgk] bind() failed: %d %s (any: %d %s)\n",
                   e1, strerror(e1), errno, strerror(errno));
            close(g_listen_fd);
            g_listen_fd = -1;
            return;
        }
        printf("[dbgk] bind fallback INADDR_ANY ok (gethostid bind was %d)\n", e1);
    }

    /* Non-blocking so the host application's loop never stalls on the net. */
    fcntl(g_listen_fd, F_SETFL, fcntl(g_listen_fd, F_GETFL, 0) | O_NONBLOCK);
    if (listen(g_listen_fd, 2) < 0) {
        printf("[dbgk] listen() failed: %d %s\n", errno, strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return;
    }

    /* Announce the address actually bound, so the operator knows where to
       point gdb: target remote <addr>:%d */
    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    if (getsockname(g_listen_fd, (struct sockaddr *)&bound, &blen) == 0)
        printf("[dbgk] listening on %s:%d\n", inet_ntoa(bound.sin_addr), DBGK_PORT);
    else
        printf("[dbgk] listening on :%d\n", DBGK_PORT);
}

void dbgk_poll(void) {
    if (g_listen_fd < 0) return;

    if (g_client_fd < 0) {
        int c = accept(g_listen_fd, NULL, NULL);
        if (c >= 0) {
            fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) | O_NONBLOCK);
            g_client_fd = c;
            g_rsp_rx_len = 0;
            g_ext_used   = 0;
            used_any     = 0;
            /* No greeting: the extension protocol is line oriented and a
               stray '+' would be consumed as the first character of a
               command. */
        }
    }
    if (g_client_fd < 0) return;

    char tmp[512];
    ssize_t n = recv(g_client_fd, tmp, sizeof(tmp), 0);
    if (n == 0) {
        close(g_client_fd);
        g_client_fd = -1;
        return;
    }
    if (n <= 0) return;
    dbgk_feed(tmp, (size_t)n);
}

void dbgk_dump_screen(const char *path, int screen) {
    u16 w = 0, h = 0;
#ifdef DBGK_HOST_TEST
    static u16 fake[240 * 400];
    u8 *fb = (u8 *)fake;
    w = 240; h = 400;
#else
    u8 *fb = gfxGetFramebuffer(screen, GFX_LEFT, &w, &h);
#endif
    FILE *f;
    if (!fb) return;
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(&w, sizeof(w), 1, f);
    fwrite(&h, sizeof(h), 1, f);
    fwrite(fb, 1, (size_t)w * h * 2u, f);
    fflush(f);
    fclose(f);
}

void dbgk_exec_ext(const char *line) {
    dbgk_ext(line);
}