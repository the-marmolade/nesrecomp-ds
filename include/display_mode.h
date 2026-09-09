/* display_mode.h — how the NES frame is fitted onto the DS screens. */
#pragma once

#define MODE_ARRANGED 0   /* playfield top screen 1:1, HUD on the sub screen */
#define MODE_SCALED   1   /* whole frame on one screen, playfield at 3/4     */

extern int g_original_mode;   /* one of the above; non-zero means "original" */
