/* debug_nds.c — test-suite aids: level select and an extended state readout.
 *
 * None of this is meant to ship. It exists because several cases in the test
 * suite are otherwise impractical: reaching 8-4 legitimately takes an hour per
 * attempt, and "does the progress bar restart in a pipe" cannot be judged from
 * where a bar happens to draw.
 *
 * Open with L + R. The game pauses while the menu is up.
 */
#include <nds.h>
#include <stdio.h>
#include <stdint.h>

extern uint8_t g_ram[];
extern uint8_t nes_read(uint16_t addr);
extern void    apu_silence(void);
extern void    power_check_lid(void);
extern void    card_check(void);

int g_debug_extra;          /* extra state line in the overlay */

/* SMB level state. Setting all three is what the upstream test scripts do:
 * the HUD reads world and level, but the area actually loaded comes from
 * AreaPointer, and setting only the first two leaves you in the wrong place
 * with the right label.
 *
 *   $075F WorldNumber   0-based
 *   $075C LevelNumber   0-based
 *   $0750 AreaPointer   which area data to load
 *
 * Area index tracks level for most stages but not all - worlds with a bonus
 * sub-area shift the numbering, which is why 1-4 is area 4 rather than 3. The
 * +1 above level 1 matches what the upstream scripts use and lands correctly
 * on the levels checked so far. If a castle loads the wrong room, this is the
 * line to adjust. */
static void warp_to(int world, int level) {
    g_ram[0x075F] = (uint8_t)world;
    g_ram[0x075C] = (uint8_t)level;
    g_ram[0x0750] = (uint8_t)(level >= 2 ? level + 1 : level);

    /* Nudge the game into reloading. OperMode_Task 0 sends it back through
     * level entry, which picks the values up. */
    g_ram[0x0772] = 0;
}

static int hit(int px, int py, int x0, int x1, int y0, int y1) {
    return px >= x0 && px <= x1 && py >= y0 && py <= y1;
}

static void draw_menu(void) {
    consoleClear();
    iprintf("\x1b[1;7HLEVEL SELECT\n");
    iprintf("\x1b[3;2HW\\L    1    2    3    4\n");
    for (int w = 0; w < 8; w++)
        iprintf("\x1b[%d;2H%d     []   []   []   []\n", 5 + w * 2, w + 1);
    iprintf("\x1b[22;2HB or L+R to cancel\n");
}

/* Blocks until a level is chosen or the menu is cancelled. The game is
 * stopped meanwhile, so the lid and card still need servicing here - the
 * frame loop that normally does it is not running. */
void debug_level_select(void) {
    apu_silence();
    draw_menu();

    for (;;) {
        swiWaitForVBlank();
        scanKeys();
        power_check_lid();
        card_check();

        int keys = keysDown();
        if (keys & KEY_B) break;

        if (keys & KEY_TOUCH) {
            touchPosition t;
            touchRead(&t);

            for (int w = 0; w < 8; w++) {
                int y0 = (5 + w * 2) * 8;
                if (t.py < y0 || t.py > y0 + 15) continue;
                for (int l = 0; l < 4; l++) {
                    int x0 = (6 + l * 5) * 8;
                    if (hit(t.px, t.py, x0, x0 + 15, y0, y0 + 15)) {
                        warp_to(w, l);
                        consoleClear();
                        iprintf("-> world %d-%d\n", w + 1, l + 1);
                        return;
                    }
                }
            }
        }
    }
    consoleClear();
}

/* Extra state for the cases that cannot be judged by eye: which area is
 * loaded, where the player is, and where the checkpoint should be. */
void debug_state_line(void) {
    uint8_t world = g_ram[0x075F];
    uint8_t level = g_ram[0x075C];
    uint8_t half  = 0;

    if (world < 8) {
        uint8_t b = nes_read((uint16_t)(0x91BD + world * 2 + (level >> 1)));
        half = (level & 1) ? (b & 0x0F) : (b >> 4);
    }

    iprintf("area=%02X pg=%02X half=%02X w%d-%d  \n",
            g_ram[0x0750], g_ram[0x006D], half, world + 1, level + 1);
}
