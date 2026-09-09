/* game_config.c — per-game settings.
 *
 * Pick with `make GAME=dk`. The Makefile turns that into -DGAME_DK.
 * Everything else about the runner is game-independent; if you find yourself
 * needing a fifth field here, that is a sign the abstraction is wrong rather
 * than that the table needs extending.
 */
#include "game_config.h"

#if defined(GAME_DK)

static const GameConfig CFG = {
    .name          = "Donkey Kong",
    .rom_file      = "dk.nes",
    /* Score and bonus occupy the top of the screen. Starting the top screen
     * at 32 mirrors the SMB arrangement: playfield unscaled on top, status
     * on the sub screen. If DK's HUD turns out to be a different height this
     * is the number to change. */
    .top_line      = 32,
    .hud_lines     = 32,
    .hud_nametable = 0,
    /* DK does not do a mid-frame scroll split, so nothing polls sprite 0.
     * Zero disables the approximation entirely. */
    .sprite0_cycle = 0,
};

#else   /* default: GAME_SMB */

static const GameConfig CFG = {
    .name          = "Super Mario Bros.",
    .rom_file      = "smb.nes",
    /* Status bar is NES lines 0-31, so the playfield starts at 32 and lines
     * 32..223 land on the top screen 1:1 with no scaling. */
    .top_line      = 32,
    .hud_lines     = 32,
    .hud_nametable = 0,
    /* Sprite0Clr at $813D waits for bit 6 low, Sprite0Hit at $8150 for it
     * high; the split is at scanline 32 -> 2273 + 32 * 113.67. */
    .sprite0_cycle = 5910,
};

#endif

const GameConfig *g_game = &CFG;
