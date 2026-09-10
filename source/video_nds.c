/* video_nds.c — NES PPU mapped onto NDS 2D hardware.
 *
 * BG:      NES nametable -> NDS text BG map (512x256 covers both nametables).
 * Tiles:   NES 2bpp planar CHR -> NDS 4bpp linear, converted once at load.
 * Sprites: NES OAM -> NDS OAM, 1:1 (NES has 64, NDS has 128).
 *
 * Vertical: NES is 240 lines, DS is 192. We show NES lines 32..223, i.e. the
 * playfield below the status bar. The HUD needs the mid-frame scroll split
 * before it can go on the sub screen; not done yet.
 */
#include <nds.h>
#include <string.h>
#include "nes_runtime.h"
#include "game_config.h"
#include "display_mode.h"


/* Original mode has to fit 240 NES lines onto a 192-line panel, and there is
 * no framebuffer to scale. Instead an HBlank interrupt advances the
 * background's vertical scroll by one line every four scanlines, so every
 * fifth NES line is skipped as the screen draws: 240 * 4/5 = 192 exactly.
 *
 * That costs one line in five, which is visible on fine detail - the trade
 * for showing the frame the way the NES did, HUD and all. Arranged mode
 * keeps every pixel by putting the HUD on the sub screen instead. */
/* The title screen has content from line 8 (the status bar text) down to 224
 * (the ground) - 216 lines against the top screen's 192, so something has to
 * give. Splitting it the same way as gameplay is the least-bad option, just
 * with the boundary moved:
 *
 *   lines   0..23   sub screen: the status bar text, 3 rows
 *   lines  24..215  top screen: the logo, menu and ground
 *
 * Starting at 24 rather than 32 recovers the top of the SUPER logo, which was
 * being sliced off, and nothing is duplicated between the screens. */
#define TITLE_TOP_LINE 24
#define TITLE_HUD_ROWS 3

static int s_scroll_y_base;
static int s_scroll_x;          /* playfield scroll, set once per frame */

static void hblank_squeeze(void) {
    int v = REG_VCOUNT;
    if (v >= 192) return;

    /* Vertical: 240 NES lines cannot fit 192 without loss, so the question is
     * only what to give up. Scaling the background costs sharpness AND leaves
     * sprites the wrong size, because OAM cannot scale a multi-tile character
     * without tearing it apart. Cropping costs visible area but nothing else:
     * every pixel that IS shown is exactly what the NES drew.
     *
     *   lines   0..31    HUD, 1:1
     *   lines  80..239   playfield, 1:1 - the top 48 lines of sky are cut
     *
     * SMB puts nothing but sky and the occasional cloud up there, and Mario's
     * jump apex stays below it, so the crop is rarely noticed. */
    /* The HUD's 32 lines are always shown 1:1 - it is nearly all text, which
     * is exactly what line-dropping ruins - and the loss falls on the
     * playfield below it, one way or the other.
     *
     * 208 playfield lines become 156: an exact 3/4, so every 16-pixel block
     * loses the same 4 lines and they all come out matching. An uneven ratio
     * (160/208) left blocks at different heights, which looked worse than the
     * 4 blank lines this leaves at the bottom of the screen. */
    int off = (v < 32) ? 0 : (v - 32) / 3;
    REG_BG0VOFS = (u16)(s_scroll_y_base + off);

    /* Horizontal: this is the mid-frame scroll split. SMB's status bar does
     * not scroll but the playfield does, and on the NES the game changes the
     * scroll partway down the frame at the sprite-0 hit. Arranged mode dodges
     * this by putting the HUD on the other screen; here both share one
     * background, so the split has to happen per scanline or the HUD slides
     * around with the level. */
    REG_BG0HOFS = (u16)(v < 32 ? 0 : s_scroll_x);
}


static int      s_bg;
static u16     *s_map;
static int      s_bg_hud;        /* sub-screen BG holding the NES status bar */
static u16     *s_map_hud;
static int      s_inited;

/* Debug: hide every sprite. Corrupt tiles that vanish with this are sprites;
 * anything that remains is background. Those have entirely different causes,
 * and guessing which is which wastes more time than the toggle costs. */
int g_hide_sprites;

/* Standard NES system palette, RGB888. */
static const u8 s_nes_rgb[64][3] = {
    { 84, 84, 84},{  0, 30,116},{  8, 16,144},{ 48,  0,136},{ 68,  0,100},{ 92,  0, 48},{ 84,  4,  0},{ 60, 24,  0},
    { 32, 42,  0},{  8, 58,  0},{  0, 64,  0},{  0, 60,  0},{  0, 50, 60},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0},
    {152,150,152},{  8, 76,196},{ 48, 50,236},{ 92, 30,228},{136, 20,176},{160, 20,100},{152, 34, 32},{120, 60,  0},
    { 84, 90,  0},{ 40,114,  0},{  8,124,  0},{  0,118, 40},{  0,102,120},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0},
    {236,238,236},{ 76,154,236},{120,124,236},{176, 98,236},{228, 84,236},{236, 88,180},{236,106,100},{212,136, 32},
    {160,170,  0},{116,196,  0},{ 76,208, 32},{ 56,204,108},{ 56,180,204},{ 60, 60, 60},{  0,  0,  0},{  0,  0,  0},
    {236,238,236},{168,204,236},{188,188,236},{212,178,236},{236,174,236},{236,174,212},{236,180,176},{228,196,144},
    {204,210,120},{180,222,120},{168,226,144},{152,226,180},{160,214,228},{160,162,160},{  0,  0,  0},{  0,  0,  0},
};

static inline u16 nes_color(u8 idx) {
    const u8 *c = s_nes_rgb[idx & 0x3F];
    return ARGB16(1, c[0] >> 3, c[1] >> 3, c[2] >> 3);
}

/* NES CHR is 2bpp planar: 8 bytes of plane 0, then 8 bytes of plane 1.
 * NDS 4bpp is linear: one byte per two pixels, low nibble = left pixel. */
static void convert_chr(u16 *dst_bg, u16 *dst_spr) {
    for (int t = 0; t < 512; t++) {
        const u8 *src = &g_chr_ram[t * 16];
        u8 tile[32];
        for (int y = 0; y < 8; y++) {
            u8 p0 = src[y], p1 = src[y + 8];
            for (int x = 0; x < 8; x += 2) {
                u8 lo = (((p0 >> (7 - x)) & 1) | (((p1 >> (7 - x)) & 1) << 1));
                u8 hi = (((p0 >> (6 - x)) & 1) | (((p1 >> (6 - x)) & 1) << 1));
                tile[y * 4 + x / 2] = (u8)(lo | (hi << 4));
            }
        }
        memcpy((u8 *)dst_bg  + t * 32, tile, 32);
        memcpy((u8 *)dst_spr + t * 32, tile, 32);
    }
}

static void convert_chr_bg(u16 *dst) {
    for (int t = 0; t < 512; t++) {
        const u8 *src = &g_chr_ram[t * 16];
        u8 tile[32];
        for (int y = 0; y < 8; y++) {
            u8 p0 = src[y], p1 = src[y + 8];
            for (int x = 0; x < 8; x += 2) {
                u8 lo = (((p0 >> (7 - x)) & 1) | (((p1 >> (7 - x)) & 1) << 1));
                u8 hi = (((p0 >> (6 - x)) & 1) | (((p1 >> (6 - x)) & 1) << 1));
                tile[y * 4 + x / 2] = (u8)(lo | (hi << 4));
            }
        }
        memcpy((u8 *)dst + t * 32, tile, 32);
    }
}

void video_init(void) {
    videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankB(VRAM_B_MAIN_SPRITE);

    s_bg  = bgInit(0, BgType_Text4bpp, BgSize_T_512x256, 0, 2);
    s_map = (u16 *)bgGetMapPtr(s_bg);

    if (g_original_mode) {
        irqSet(IRQ_HBLANK, hblank_squeeze);
        irqEnable(IRQ_HBLANK);
        /* irqEnable registers the handler with the interrupt controller, but
         * the display controller also has to be told to raise HBlank at all.
         * Without this bit the handler is installed and never called - which
         * is exactly what the diagnostic counter showed. */
#ifdef DISP_HBLANK_IRQ
        REG_DISPSTAT |= DISP_HBLANK_IRQ;
#else
        REG_DISPSTAT |= (1 << 4);
#endif
    }

    oamInit(&oamMain, SpriteMapping_1D_32, false);

    /* Sprites are NOT affine-scaled in original mode, though it is tempting.
     * OAM scales each 8x8 sprite about its own centre, and almost every SMB
     * character is built from several sprites side by side - so scaling pulls
     * the halves apart and leaves a seam straight down the middle of Mario.
     * Position-only scaling keeps them whole at the cost of being a line
     * taller than the squeezed background expects. */
    convert_chr((u16 *)bgGetGfxPtr(s_bg), (u16 *)SPRITE_GFX);

    /* Status bar on the sub screen. consoleDemoInit owns sub BG0 with map
     * base 31 and tile base 0. mapBase is 5 bits (0-31) and tileBase 4 bits
     * (0-15), so: map base 30 = 60-62KB, tile base 2 = 32-48KB. Neither
     * collides with the console or with each other. */
    /* Only in arranged mode. Original mode never creates this background, and
     * s_bg_hud is then 0 - which is the console's BG, so setting it up here
     * would write tile data over the console's tile memory and scroll it. */
    if (!g_original_mode) {
        s_bg_hud  = bgInitSub(1, BgType_Text4bpp, BgSize_T_256x256, 30, 2);
        s_map_hud = (u16 *)bgGetMapPtr(s_bg_hud);
        convert_chr_bg((u16 *)bgGetGfxPtr(s_bg_hud));
        bgSetScroll(s_bg_hud, 0, 0);
        bgUpdate();
    }

    /* Drop the console below the 4 HUD rows so both are readable. */
    consoleSetWindow(NULL, 0, 4, 32, 20);

    s_inited = 1;
}

/* NES attribute byte packs four 16x16 quadrants; pick this tile's 2 bits. */
static inline u8 attr_for(const u8 *nt, int col, int row) {
    u8 a = nt[0x3C0 + (row >> 2) * 8 + (col >> 2)];
    int shift = ((row & 2) << 1) | (col & 2);
    return (a >> shift) & 3;
}

static void push_nametable(const u8 *nt, u16 *map, int bg_tile_base) {
    for (int row = 0; row < 30; row++) {
        for (int col = 0; col < 32; col++) {
            u8 t = nt[row * 32 + col];
            map[row * 32 + col] = (u16)(bg_tile_base + t) | (u16)(attr_for(nt, col, row) << 12);
        }
    }
    for (int i = 30 * 32; i < 32 * 32; i++) map[i] = 0;
}

/* Shadow buffers built during the frame, DMA'd to VRAM during vblank.
 * Building straight into VRAM overruns the ~1.3ms vblank window (1920 map
 * entries with an attribute lookup each), so the tail of the writes lands
 * mid-display and tears. */
static u16 s_shadow[2048];      /* 512x256 text BG = two 32x32 screenblocks */
static u16 s_shadow_hud[128];   /* 4 rows x 32 cols */
static u16 s_pal_bg[64], s_pal_spr[64], s_pal_sub[128];

static void build_nametable(const u8 *nt, u16 *dst, int bg_tile_base) {
    for (int row = 0; row < 30; row++)
        for (int col = 0; col < 32; col++) {
            u8 t = nt[row * 32 + col];
            dst[row * 32 + col] =
                (u16)(bg_tile_base + t) | (u16)(attr_for(nt, col, row) << 12);
        }
    for (int i = 30 * 32; i < 32 * 32; i++) dst[i] = 0;
}

/* Phase 1: runs right after the game's NMI handler, outside vblank. */
void video_build(void) {
    if (!s_inited) return;

    s_pal_bg[0] = nes_color(g_ppu_pal[0]);
    for (int p = 0; p < 4; p++)
        for (int c = 1; c < 4; c++)
            s_pal_bg[p * 16 + c] = nes_color(g_ppu_pal[p * 4 + c]);

    for (int p = 0; p < 4; p++) {
        s_pal_spr[p * 16] = 0;
        for (int c = 1; c < 4; c++)
            s_pal_spr[p * 16 + c] = nes_color(g_ppu_pal[0x10 + p * 4 + c]);
    }

    for (int p = 0; p < 4; p++)
        for (int c = 1; c < 4; c++)
            s_pal_sub[(p + 4) * 16 + c] = nes_color(g_ppu_pal[p * 4 + c]);

    /* The two-screen split assumes NES lines 0-31 are a status bar. That is
     * only true during gameplay: on the title screen those lines are the top
     * of the logo, and slicing them off to the sub screen cuts the logo and
     * puts a meaningless score readout under it.
     *
     * $0770 OperMode is 1 during gameplay, 0 on the title/demo. Off the
     * playfield, show the frame from near the top and leave the sub screen
     * empty. */
    int in_game  = (g_ram[0x0770] == 1);
    int top_line = in_game ? g_game->top_line : TITLE_TOP_LINE;
    int hud_rows = in_game ? g_game->hud_lines / 8 : TITLE_HUD_ROWS;

    int bg_base  = (g_ppuctrl & 0x10) ? 256 : 0;
    int spr_base = (g_ppuctrl & 0x08) ? 256 : 0;

    build_nametable(&g_ppu_nt[0x000], s_shadow,        bg_base);
    build_nametable(&g_ppu_nt[0x400], s_shadow + 1024, bg_base);

    /* Status bar, mirrored onto the sub screen and never scrolled. Which
     * nametable it lives in is per-game: SMB only ever writes NT0, and
     * following the PPUCTRL nametable-select bit makes the HUD blink out on
     * every frame where that bit points elsewhere. */
    {
        const u8 *nt = &g_ppu_nt[g_game->hud_nametable ? 0x400 : 0x000];
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 32; col++) {
                /* Rows past hud_rows are blanked rather than left stale - on
                 * the title screen row 3 belongs to the logo, and showing it
                 * here as well would duplicate it on both screens. */
                u8 t = (row < hud_rows) ? nt[row * 32 + col] : 0x24;
                s_shadow_hud[row * 32 + col] =
                    (u16)(bg_base + t) | (u16)((attr_for(nt, col, row) + 4) << 12);
            }
    }

    s_scroll_x = g_ppuscroll_x + ((g_ppuctrl & 1) ? 256 : 0);

    /* The NES vertical scroll was being captured into g_ppuscroll_y and then
     * never read, so anything the game scrolled vertically came out clipped -
     * the sliced top of the title logo in issue #13. SMB uses it on the title
     * screen and around the underground and water transitions.
     *
     * Only 0-239 is a real scroll position. Writing 240-255 does not shift
     * the picture up - on hardware the PPU starts reading attribute bytes as
     * if they were tiles - and SMB writes 248 on the title screen as a
     * scratch value rather than as a scroll. Treating it as -8 moved the
     * status bar onto the top screen, so out-of-range values are ignored. */
    int sy = g_ppuscroll_y;
    if (sy >= 240) sy = 0;
    s_scroll_y_base = (g_original_mode ? 0 : top_line) + sy;

    /* oamSet writes into libnds' RAM copy; oamUpdate DMAs it in vblank. */
    for (int i = 0; i < 64; i++) {
        const u8 *o = &g_ppu_oam[i * 4];
        int ny = o[0] + 1;
        int y;
        if (!g_original_mode)      y = ny - top_line - sy;
        else if (ny < 32) y = ny;                       /* HUD, 1:1 */
        else              y = 32 + (ny - 32) * 3 / 4;   /* playfield, 3/4 */
        int x = o[3];
        u8  tile = o[1], attr = o[2];
        int hflip = (attr & 0x40) != 0;
        int vflip = (attr & 0x80) != 0;
        int hidden = (o[0] >= 0xEF) || (y < -8) || (y > 192) || g_hide_sprites;
        oamSet(&oamMain, i, x, y,
               (attr & 0x20) ? 2 : 0, attr & 3,
               SpriteSize_8x8, SpriteColorFormat_16Color,
               (u8 *)SPRITE_GFX + (spr_base + tile) * 32,
               -1, false, hidden, hflip, vflip, false);
    }
}

/* Phase 2: runs inside vblank. DMA only — no computation. */
void video_flush(void) {
    if (!s_inited) return;

    /* Flush the shadow buffers out of the data cache before DMA reads them.
     *
     * video_build() writes these through the ARM9's cache; dmaCopy reads main
     * RAM directly and does not see cache. Anything still dirty never reaches
     * VRAM, which shows up as stale or garbled tiles in bands - and only on
     * real hardware, because emulators have no cache to be out of sync with.
     * That is exactly the shape of issue #13. */
    DC_FlushRange(s_shadow,     sizeof s_shadow);
    DC_FlushRange(s_shadow_hud, sizeof s_shadow_hud);
    DC_FlushRange(s_pal_bg,     sizeof s_pal_bg);
    DC_FlushRange(s_pal_spr,    sizeof s_pal_spr);
    DC_FlushRange(s_pal_sub,    sizeof s_pal_sub);
    dmaCopy(s_shadow,     s_map,          sizeof s_shadow);
    if (!g_original_mode)
        dmaCopy(s_shadow_hud, s_map_hud, sizeof s_shadow_hud);
    dmaCopy(s_pal_bg,     BG_PALETTE,     sizeof s_pal_bg);
    dmaCopy(s_pal_spr,    SPRITE_PALETTE, sizeof s_pal_spr);
    if (!g_original_mode)
        dmaCopy(s_pal_sub, BG_PALETTE_SUB, sizeof s_pal_sub);
    /* In original mode the HBlank handler drives both scroll registers every
     * scanline; writing them here as well just fights it. */
    if (!g_original_mode) {
        bgSetScroll(s_bg, s_scroll_x, s_scroll_y_base);
        bgUpdate();
    }
    oamUpdate(&oamMain);
}
