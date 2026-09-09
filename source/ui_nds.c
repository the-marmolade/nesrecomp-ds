/* ui_nds.c — touch save/load for the bottom screen.
 *
 * Replaces the R+X / R+Y button combos, which were a developer convenience:
 * undiscoverable, and a mistimed press destroyed a save with no warning.
 * Both actions now confirm first, and confirming pauses the game rather than
 * letting it run underneath a dialog.
 *
 * Drawn as text on the console the status bar already shares, so this needs
 * no extra VRAM. The console window starts at screen row 4 (rows 0-3 are the
 * HUD), so console row N is screen row N+4 - hence the offset in ROW_TO_Y.
 */
#include <nds.h>
#include <stdio.h>

#include <stdint.h>
#include "display_mode.h"

extern int  nes_save_state(void);
extern int  nes_load_state(void);
extern void apu_silence(void);
extern uint8_t g_ram[];

/* How the NES frame is presented. 240 lines cannot fit 192 without losing
 * something, and the two ways of losing it suit different tastes, so the
 * choice is the player's:
 *
 *   MODE_ARRANGED  playfield on the top screen 1:1, HUD on the sub screen.
 *                  Nothing is lost and nothing is scaled - the DS has two
 *                  screens, so use them.
 *   MODE_SCALED    the whole frame on one screen, playfield squeezed to 3/4.
 *                  What nesDS does: you see everything, slightly soft.
 *
 * A third option that cropped the sky instead of scaling was tried and cut:
 * pixel-perfect, but losing the clouds was a worse trade than softness.
 *
 * Anything non-zero means "original": no save states, no progress bar. */
int g_original_mode;

#define CONSOLE_TOP_ROW 4                   /* HUD occupies rows 0-3 */
#define ROW_TO_Y(r)     (((r) + CONSOLE_TOP_ROW) * 8)

/* Buttons sit low on the screen, clear of the HUD and the debug overlay. */
#define BTN_ROW      16
#define BTN_H        24
#define SAVE_X0      16
#define SAVE_X1     112
#define LOAD_X0     144
#define LOAD_X1     240

#define CONFIRM_ROW  10
#define YES_X0       16
#define YES_X1      112
#define NO_X0       144
#define NO_X1       240

static int s_drawn;

/* ---- progress bar -------------------------------------------------------
 *
 * $006D Player_PageLoc and $0086 Player_X_Position give the player's position
 * as page * 256 + x. $0770 OperMode is 1 during gameplay.
 *
 * Level length is the awkward part: SMB has no single "how long is this
 * level" value in RAM - the area data is a stream of objects terminated by a
 * loop command, and working out the end means walking it. Most levels finish
 * between pages 12 and 14, so the bar is scaled to a nominal length and
 * clamped. That makes it a sense of progress rather than a measurement, which
 * is what a progress bar is for; a level that runs long simply sits at full
 * for its last stretch. */
#define BAR_ROW        8
#define BAR_W         26
#define NOMINAL_PAGES 13

static int s_last_fill = -1;

static void draw_progress(void) {
    if (g_ram[0x0770] != 1) {          /* not in gameplay */
        if (s_last_fill != -1) {
            iprintf("\x1b[%d;0H                                ", BAR_ROW);
            s_last_fill = -1;
        }
        return;
    }

    int pos   = g_ram[0x006D] * 256 + g_ram[0x0086];
    int total = NOMINAL_PAGES * 256;
    int fill  = pos * BAR_W / total;
    if (fill < 0)     fill = 0;
    if (fill > BAR_W) fill = BAR_W;

    if (fill == s_last_fill) return;   /* console writes are not free */
    s_last_fill = fill;

    char bar[BAR_W + 3];
    bar[0] = '[';
    for (int i = 0; i < BAR_W; i++) bar[i + 1] = (i < fill) ? '=' : '.';
    bar[BAR_W + 1] = ']';
    bar[BAR_W + 2] = 0;
    iprintf("\x1b[%d;2H%s", BAR_ROW, bar);
}

static int hit(int px, int py, int x0, int x1, int row) {
    int y0 = ROW_TO_Y(row);
    return px >= x0 && px <= x1 && py >= y0 && py <= y0 + BTN_H;
}

static void draw_buttons(void) {
    iprintf("\x1b[%d;2H[   SAVE   ]", BTN_ROW);
    iprintf("\x1b[%d;18H[   LOAD   ]", BTN_ROW);
}

static void clear_rows(int row, int count) {
    for (int i = 0; i < count; i++)
        iprintf("\x1b[%d;0H                                ", row + i);
}

/* Modal prompt. Blocks until the player answers, which pauses the game: the
 * caller is the frame loop, so not returning means no frames advance. That is
 * the point - a dialog the game runs underneath is worse than no dialog. */
static int confirm(const char *question) {
    /* The frame loop is about to stop, and the sound channels would carry on
     * playing whatever note was in flight. */
    apu_silence();

    clear_rows(CONFIRM_ROW - 2, 10);
    iprintf("\x1b[%d;2H%s", CONFIRM_ROW - 2, question);
    iprintf("\x1b[%d;2H[   YES    ]", CONFIRM_ROW);
    iprintf("\x1b[%d;18H[    NO    ]", CONFIRM_ROW);

    for (;;) {
        swiWaitForVBlank();
        scanKeys();
        int keys = keysDown();

        if (keys & KEY_TOUCH) {
            touchPosition t;
            touchRead(&t);
            if (hit(t.px, t.py, YES_X0, YES_X1, CONFIRM_ROW)) break;
            if (hit(t.px, t.py, NO_X0,  NO_X1,  CONFIRM_ROW)) {
                clear_rows(CONFIRM_ROW - 2, 10);
                s_drawn = 0;
                s_last_fill = -1;
                return 0;
            }
        }
        /* Face buttons as well as touch: quicker mid-game, and it means the
         * dialog is still answerable if the touch screen is miscalibrated. */
        if (keys & KEY_A) break;
        if (keys & KEY_B) {
            clear_rows(CONFIRM_ROW - 2, 10);
            s_drawn = 0;
            s_last_fill = -1;
            return 0;
        }
    }

    clear_rows(CONFIRM_ROW - 2, 10);
    s_drawn = 0;
    s_last_fill = -1;
    return 1;
}

static void report(const char *msg) {
    iprintf("\x1b[%d;2H%s", BTN_ROW - 2, msg);
}

/* Called once per frame from the runtime, after poll_input() has run
 * scanKeys() for this frame. */
/* Boot-time mode picker. Runs before the game starts, so it can block. */
int ui_select_mode(void) {
    consoleClear();
    iprintf("\x1b[2;4HSUPER MARIO BROS.\n");
    iprintf("\x1b[6;2HChoose how to play:\n");
    iprintf("\x1b[10;2H[   ARRANGED   ]");
    iprintf("\x1b[12;2H  two screens, nothing lost");
    iprintf("\x1b[16;2H[   ORIGINAL   ]");
    iprintf("\x1b[18;2H  whole frame, one screen");

    for (;;) {
        swiWaitForVBlank();
        scanKeys();
        int keys = keysDown();

        if (keys & KEY_TOUCH) {
            touchPosition t;
            touchRead(&t);
            if (hit(t.px, t.py, 16, 240, 10)) { g_original_mode = MODE_ARRANGED; break; }
            if (hit(t.px, t.py, 16, 240, 16)) { g_original_mode = MODE_SCALED;   break; }
        }
        if (keys & (KEY_A | KEY_START)) { g_original_mode = MODE_ARRANGED; break; }
        if (keys & KEY_B)               { g_original_mode = MODE_SCALED;   break; }
    }

    consoleClear();
    return g_original_mode;
}

void ui_frame(void) {
    /* Original mode has no save states and no progress bar - the point is a
     * plain NES, so the bottom screen stays empty. */
    if (g_original_mode) return;

    if (!s_drawn) { draw_buttons(); s_drawn = 1; }
    draw_progress();

    if (!(keysDown() & KEY_TOUCH)) return;

    touchPosition t;
    touchRead(&t);

    if (hit(t.px, t.py, SAVE_X0, SAVE_X1, BTN_ROW)) {
        if (confirm("Overwrite saved game?"))
            report(nes_save_state() ? "saved       " : "save failed ");
        else
            report("            ");
    } else if (hit(t.px, t.py, LOAD_X0, LOAD_X1, BTN_ROW)) {
        if (confirm("Load saved game?"))
            report(nes_load_state() ? "loaded      " : "no save     ");
        else
            report("            ");
    }
}
