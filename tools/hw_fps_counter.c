/* Hardware FPS Counter for 3DS
 * Counts VBlank interrupts over 60 seconds, writes results to SD
 */
#include <3ds.h>
#include <stdio.h>

static volatile u64 vblank_count = 0;

void vblank_irq(void) {
    vblank_count++;
}

int main(int argc, char **argv) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    
    printf("3DS Hardware FPS Counter\n");
    printf("========================\n\n");
    
    // Register VBlank interrupt
    irqSet(IRQ_VBLANK, vblank_irq);
    irqEnable(IRQ_VBLANK);
    
    printf("Starting 60-second measurement...\n");
    printf("Do not close lid or press HOME.\n\n");
    
    u64 start_ticks = svcGetSystemTick();
    u64 frames_at_start = vblank_count;
    
    // Wait 60 seconds
    for (int i = 60; i > 0 && aptMainLoop(); i--) {
        printf("\rTime remaining: %2d sec  Frames: %llu", i, vblank_count);
        gspWaitForVBlank(); // Wait for next VBlank
        for (int j = 0; j < 59; j++) gspWaitForVBlank();
    }
    
    u64 end_ticks = svcGetSystemTick();
    u64 elapsed_frames = vblank_count;
    double elapsed_sec = (svcGetSystemTick() - start_ticks) / 1e9;
    double fps = vblank_count / ((svcGetSystemTick() - start_ticks) / 1e9);
    
    printf("\n\nResults:\n");
    printf("  Elapsed time: %.3f sec\n", (svcGetSystemTick() - start_ticks) / 1e9);
    printf("  Frame count: %llu\n", vblank_count);
    printf("  FPS: %.3f\n", fps);
    
    // Write results to SD card
    FILE *f = fopen("/fps_result.txt", "w");
    if (f) {
        fprintf(f, "Hardware FPS Measurement\n");
        fprintf(f, "Elapsed: %.3f sec\n", (svcGetSystemTick() - start_ticks) / 1e9);
        fprintf(f, "Frames: %llu\n", vblank_count);
        fprintf(f, "FPS: %.3f\n", fps);
        fclose(f);
        printf("\nResults written to /fps_result.txt\n");
    } else {
        printf("\nFailed to write results to SD\n");
    }
    
    printf("\nPress START to exit\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    
    gfxExit();
    return 0;
}
