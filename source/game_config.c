/* game_config.c — per-game settings.
 *
 * One entry per supported game. Selected at build time for now; when more
 * than a couple of games exist this should be picked from the ROM's header
 * hash or passed in by the build.
 */
#include "game_config.h"

static const GameConfig SMB = {
    .name          = "Super Mario Bros.",
    /* Status bar occupies NES lines 0-31, so the playfield starts at 32 and
     * lines 32..223 land on the top screen 1:1 with no scaling. */
    .top_line      = 32,
    .hud_lines     = 32,
    .hud_nametable = 0,
    /* Sprite0Clr at $813D waits for bit 6 low, Sprite0Hit at $8150 waits for
     * it high; the split is at scanline 32. */
    .sprite0_cycle = 5910,
};

const GameConfig *g_game = &SMB;
