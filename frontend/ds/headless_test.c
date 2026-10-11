/*
 * headless_test.c — Headless frame validation for DS target.
 *
 * Mirrors the upstream Linux headless-test but compiled for ARM9.
 * Advances the core for N frames and prints diagnostic info.
 * No graphics/audio dependencies — ideal for CI/container testing.
 */

#include "simcity_static_recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load_rom(const char *path, uint8_t **out_rom, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open ROM: %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size <= 0 || size > 0x100000) {
        fclose(f);
        fprintf(stderr, "Invalid ROM size: %ld\n", size);
        return -1;
    }
    uint8_t *rom = (uint8_t *)malloc((size_t)size);
    if (!rom) {
        fclose(f);
        fprintf(stderr, "Out of memory\n");
        return -1;
    }
    size_t n = fread(rom, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) {
        free(rom);
        fprintf(stderr, "Incomplete read: %zu/%ld\n", n, size);
        return -1;
    }
    *out_rom = rom;
    *out_size = (size_t)size;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <rom.sfc> [frames]\n", argv[0]);
        return 1;
    }
    const char *rom_path = argv[1];
    uint32_t frame_count = argc > 2 ? (uint32_t)atoi(argv[2]) : 300u;

    uint8_t *rom = NULL;
    size_t rom_size = 0;
    if (load_rom(rom_path, &rom, &rom_size) != 0) return 1;

    char err[256];
    SimCityRecomp *recomp = NULL;
    if (simcity_recomp_create(&recomp, rom, rom_size, err, sizeof(err)) != 1) {
        fprintf(stderr, "simcity_recomp_create failed: %s\n", err);
        free(rom);
        return 1;
    }
    free(rom);

    simcity_recomp_log_open("simcity-headless.log", NULL, 0);

    /* Reset to cold boot */
    char reset_err[256];
    if (!simcity_recomp_reset(recomp, reset_err, sizeof(reset_err))) {
        fprintf(stderr, "simcity_recomp_reset failed: %s\n", reset_err);
        simcity_recomp_destroy(recomp);
        return 1;
    }

    printf("ROM loaded: %zu bytes\n", rom_size);
    printf("Advancing %u frames...\n", frame_count);

    for (uint32_t i = 0; i < frame_count; i++) {
        SimCityRecompFrameResult result;
        memset(&result, 0, sizeof(result));
        uint16_t input = 0;
        int rc = simcity_recomp_advance(recomp, input, 1u, &result);
        if (rc != 1) {
            printf("Frame %u: advance failed (rc=%d). %s\n",
                   i, rc, result.renderer_error);
            break;
        }
        if (!result.frame_rendered) {
            printf("Frame %u: not rendered (route=%u)\n", i, result.route_continued);
        }
        if (i % 60u == 0) {
            printf("  frame=%u inst=%llu master=%llu\n",
                   i,
                   (unsigned long long)simcity_recomp_instruction_count(recomp),
                   (unsigned long long)simcity_recomp_master_clock(recomp));
        }
    }

    uint64_t total_inst = simcity_recomp_instruction_count(recomp);
    uint64_t total_clk  = simcity_recomp_master_clock(recomp);
    printf("Done. Total instructions: %llu, master clock: %llu\n",
           (unsigned long long)total_inst, (unsigned long long)total_clk);

    /* Verify no route failure */
    if (simcity_recomp_failed(recomp)) {
        fprintf(stderr, "FAILURE: core entered failed state.\n");
        simcity_recomp_log_close();
        simcity_recomp_destroy(recomp);
        return 1;
    }

    /* Verify instruction count is non-zero */
    if (total_inst == 0) {
        fprintf(stderr, "FAILURE: no instructions executed.\n");
        simcity_recomp_log_close();
        simcity_recomp_destroy(recomp);
        return 1;
    }

    simcity_recomp_log_close();
    simcity_recomp_destroy(recomp);
    printf("PASS\n");
    return 0;
}