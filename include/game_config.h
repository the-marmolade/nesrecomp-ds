/* game_config.h — the parts of the runner that are not game-independent.
 *
 * Everything else in this runner (CPU pump, memory map, controller, PPU
 * register decoding, CHR conversion, APU, frame pacing, save states) works
 * for any NES game. These few values do not, and hardcoding them is what
 * would otherwise make the runner SMB-only.
 *
 * Adding a game should be a matter of adding an entry here, not editing code.
 */
#pragma once
#include <stdint.h>

typedef struct {
    const char *name;

    /* First NES scanline shown on the DS top screen. The NES is 240 lines and
     * the DS is 192, so something has to give. For a game with a fixed status
     * bar at the top, showing lines [top_line, top_line+192) puts the
     * playfield on the top screen unscaled and the HUD goes on the sub screen.
     * SMB: 32. A game with no HUD band wants 24 (centred, dropping overscan). */
    int top_line;

    /* Height in scanlines of the status bar mirrored onto the sub screen.
     * 0 means the game has no fixed HUD and nothing is drawn there. */
    int hud_lines;

    /* Nametable the status bar is drawn into. SMB only ever writes NT0;
     * following the PPUCTRL nametable-select bit makes the HUD blink out on
     * every frame where it points elsewhere. */
    int hud_nametable;

    /* Cycles into the frame at which sprite 0 hit is reported. Real hardware
     * sets it when an opaque sprite-0 pixel overlaps an opaque background
     * pixel; without a dot-clock model we approximate it from position within
     * the frame. Should correspond to the scanline the game splits on:
     *   2273 vblank cycles + scanline * 113.67
     * SMB splits at 32 -> 5910. Games that never poll it can use 0. */
    uint32_t sprite0_cycle;
} GameConfig;

extern const GameConfig *g_game;

/* Mirroring comes from the iNES header via the mapper at load time, not from
 * this table — mapper_get_mirroring() already knows the answer. */
#define MIRROR_HORIZONTAL 3
#define MIRROR_VERTICAL   2
#define MIRROR_ONE_LOWER  0
#define MIRROR_ONE_UPPER  1
