#include <nds.h>
#include <fat.h>
#include <filesystem.h>
#include <stdio.h>
#include <stdint.h>
#include "game_config.h"

extern void func_RESET(void);
extern void func_NMI(void);
extern void func_IRQ(void);
extern int  call_by_address(uint16_t addr);
extern int  nes_rom_load(const char *path);
extern void video_init(void);
extern void nes_timing_init(void);
extern void apu_init(void);
extern void power_init(void);
extern int  ui_select_mode(void);
extern int  g_fat_ready;

static void *const g_entry_anchors[] = {
    (void *)func_RESET, (void *)func_NMI,
    (void *)func_IRQ,   (void *)call_by_address,
};

int main(void) {
    consoleDemoInit();

    volatile uintptr_t sink = 0;
    for (unsigned i = 0; i < sizeof(g_entry_anchors)/sizeof(*g_entry_anchors); i++)
        sink += (uintptr_t)g_entry_anchors[i];

    /* NitroFS first: the ROM is embedded in the .nds, so no SD card or DLDI
     * patching is needed and it works identically in DeSmuME, melonDS and on
     * hardware. Falls back to the SD card if the build has no nitrofiles. */
    /* Always bring FAT up, even when the ROM came from NitroFS — save states
     * need somewhere to write. */
    g_fat_ready = fatInitDefault() ? 1 : 0;

    {
        char path[64];
        int loaded = 0;

        siprintf(path, "nitro:/%s", g_game->rom_file);
        if (nitroFSInit(NULL) && nes_rom_load(path)) {
            iprintf("ROM loaded from NitroFS\n");
            loaded = 1;
        }
        if (!loaded && g_fat_ready) {
            siprintf(path, "/%s", g_game->rom_file);
            if (nes_rom_load(path)) loaded = 1;
            if (!loaded) {
                siprintf(path, "fat:/%s", g_game->rom_file);
                if (nes_rom_load(path)) loaded = 1;
            }
            if (loaded) iprintf("ROM loaded from SD\n");
        }
        if (!loaded) {
            iprintf("could not load %s\n", g_game->rom_file);
            while (1) swiWaitForVBlank();
        }
    }
    /* Ask before initialising video: the two modes set the hardware up
     * differently, and original mode never creates the sub-screen HUD. */
    ui_select_mode();

    video_init();
    nes_timing_init();
    apu_init();
    power_init();


    func_RESET();
    iprintf("RESET returned (unexpected)\n");
    while (1) swiWaitForVBlank();
}
