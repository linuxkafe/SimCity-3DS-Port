/* check_rgb565_exact.c — prove the new 15-bit conversion matches the old one.
 *
 * This exists because the obvious implementation of "convert BGR555 to RGB565"
 * is wrong. Written from the field layout it disagrees with the code it
 * replaces on 32736 of 32768 possible inputs, and not by a rounding error --
 * the greens come out wrong, because the old path widens 5->8->6 instead of
 * copying the 6-bit field, which is brighter than a straight copy.
 *
 * So "it looks right" is not available as evidence here. This checks all
 * 32768 inputs against the exact arithmetic the old code performed.
 *
 * Build and run:  cc -O2 -o /tmp/chk tools/check_rgb565_exact.c && /tmp/chk
 * Exit status is 0 only if every input matches.
 */
#include <stdint.h>
#include <stdio.h>

/* The core's table, from bgra() (patches/0002). */
static const uint8_t widen5[32] = {
      0,   8,  16,  24,  32,  41,  49,  57,  65,  74,  82,  90,  98, 106,
    115, 123, 131, 139, 148, 156, 164, 172, 180, 189, 197, 205, 213, 222,
    230, 238, 246, 255
};
static uint8_t widen6[32];   /* widen5[v] >> 2 */

/* bgra(): 0xAABBGGRR, widened 5->8 per channel. */
static uint32_t bgra_of(uint16_t c) {
    return 0xFF000000u | widen5[c & 31u]
         | ((uint32_t)widen5[(c >> 5) & 31u] << 8)
         | ((uint32_t)widen5[(c >> 10) & 31u] << 16);
}

/* The path that shipped before patches/0003: frame_to_rgb565(uint32_t).
   Byte 0 is red, not blue -- measured on the device, see 3ds_render.c. */
static uint16_t via_bgra(uint32_t px) {
    uint8_t r = (uint8_t)(px & 0xFFu);
    uint8_t g = (uint8_t)((px >> 8) & 0xFFu);
    uint8_t b = (uint8_t)((px >> 16) & 0xFFu);
    uint8_t r5 = (uint8_t)((r >> 3) & 0x1Fu);
    uint8_t g6 = (uint8_t)((g >> 2) & 0x3Fu);
    uint8_t b5 = (uint8_t)((b >> 3) & 0x1Fu);
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* The path that shipped after patches/0003: frame_to_rgb565(uint16_t). */
static uint16_t direct(uint16_t c) {
    uint8_t r5 = (uint8_t)(c & 31u);
    uint8_t g6 = widen6[(c >> 5) & 31u];
    uint8_t b5 = (uint8_t)((c >> 10) & 31u);
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* What a first-time implementer would write from the field layout. */
static uint16_t naive(uint16_t c) {
    uint8_t b5 = (uint8_t)(c & 31u);
    uint8_t g6 = (uint8_t)((c >> 5) & 63u);
    uint8_t r5 = (uint8_t)((c >> 10) & 31u);
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

int main(void) {
    int i, n_dir = 0, n_naive = 0, identity_ok = 1;
    uint16_t first_bad = 0;

    for (i = 0; i < 32; ++i) widen6[i] = (uint8_t)(widen5[i] >> 2);

    /* The identity that makes the fast path fast: red and blue need no table
       at all, because narrowing the widened byte by 3 recovers the 5-bit input
       exactly. If this ever fails, the direct path needs a table for all three
       channels and the comment in 3ds_render.c is wrong. */
    for (i = 0; i < 32; ++i)
        if ((widen5[i] >> 3) != (uint8_t)i) identity_ok = 0;

    for (i = 0; i < 32768; ++i) {
        uint16_t c   = (uint16_t)i;
        uint16_t ref = via_bgra(bgra_of(c));
        if (ref != direct(c)) { if (!n_dir) first_bad = c; n_dir++; }
        if (ref != naive(c))  n_naive++;
    }

    printf("widen5[v]>>3 == v for all 32 v      : %s\n",
           identity_ok ? "yes" : "NO");
    printf("direct   vs old path, 32768 inputs  : %d divergent\n", n_dir);
    printf("naive    vs old path, 32768 inputs  : %d divergent  <- why not\n",
           n_naive);

    if (n_dir) {
        printf("first divergence at 0x%04x: old=0x%04x new=0x%04x\n",
               first_bad, via_bgra(bgra_of(first_bad)),
               direct(first_bad));
        return 1;
    }
    if (!identity_ok) {
        printf("identity check failed; the direct path needs rethinking\n");
        return 1;
    }
    printf("\nOK: the direct conversion is bit-identical to the old one\n");
    return 0;
}
