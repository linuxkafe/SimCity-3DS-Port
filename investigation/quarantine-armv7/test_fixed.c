#include <3ds.h>
int main(int argc, char **argv) {
    gfxInitDefault();
    // Don't use console - just clear screens and wait
    uint16_t *top = (uint16_t*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    uint16_t *bot = (uint16_t*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    // Fill with colors to verify
    for(int i=0; i<400*240; i++) {
        top[i] = 0x001F;  // Blue
        bot[i] = 0xF800;  // Red
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
    
    while(1) {
        gspWaitForVBlank();
        hidScanInput();
        if(hidKeysHeld()&KEY_A) break;
    }
    
    gfxExit();
    return 0;
}
