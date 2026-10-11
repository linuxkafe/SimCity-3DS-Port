#include <3ds.h>
#include <stdio.h>
int main(int argc, char **argv) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("Hello v6!\n");
    while(1) { gspWaitForVBlank(); hidScanInput(); if(hidKeysHeld()&KEY_A) break; }
    gfxExit();
    return 0;
}
