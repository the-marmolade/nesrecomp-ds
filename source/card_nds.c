/* card_nds.c — halt if the DS card is removed.
 *
 * Retail DS titles were required to stop dead when the card came out and to
 * stay stopped until the console was power-cycled. Homebrew normally does not,
 * because the whole program is already in RAM and nothing needs the card
 * again after boot - which is why this game happily kept playing with the
 * cart in your hand (issue #15).
 *
 * Mimicking it means actively probing the card bus: read the header at boot,
 * re-read it periodically, and halt when the answer changes.
 *
 * Two cautions, both real:
 *
 *  - The flashcart uses this same bus for SD access. Probing while libfat is
 *    mid-operation can collide, so save/load raises card_set_busy() and the
 *    check is skipped until it clears. A false positive here would halt the
 *    game AND corrupt a save at the same time.
 *  - Flashcarts answer raw card commands in their own way. If a DSpico turns
 *    out to report removal spuriously, set CARD_CHECK_ENABLED to 0 rather
 *    than shipping a build that halts on working hardware.
 */
#include <nds.h>
#include <stdio.h>
#include <string.h>

#define CARD_CHECK_ENABLED 1
#define CHECK_INTERVAL     60      /* frames between probes: once a second */

extern void apu_silence(void);

static u32 s_baseline;
static int s_busy;
static int s_ready;
static int s_countdown = CHECK_INTERVAL;

/* Cheap fingerprint of the card header. The exact value does not matter; only
 * whether it keeps coming back the same. */
static u32 header_fingerprint(void) {
    u8 header[512];
    memset(header, 0, sizeof header);
    cardReadHeader(header);

    u32 h = 2166136261u;
    for (unsigned i = 0; i < sizeof header; i++) {
        h ^= header[i];
        h *= 16777619u;
    }
    return h;
}

void card_init(void) {
#if CARD_CHECK_ENABLED
    s_baseline = header_fingerprint();
    s_ready = 1;
#endif
}

void card_set_busy(int busy) { s_busy = busy; }

static void card_halt(void) {
    apu_silence();

    /* The top screen is deliberately left alone. Retail titles freeze on the
     * last frame drawn; they do not blank. videoSetMode(0) disables the
     * display, and a disabled DS screen is white - which looks like a crash
     * rather than a pause. Simply not updating leaves the last frame in VRAM
     * exactly where it was.
     *
     * The message goes on the bottom screen. Retail behaviour is to stay
     * stopped until the console is power-cycled - reinserting does not
     * resume - so this loop never exits. */
    consoleClear();
    consoleSetWindow(NULL, 0, 0, 32, 24);
    iprintf("\x1b[10;6HPLEASE INSERT\n");
    iprintf("\x1b[12;7HTHE DS CARD\n");
    iprintf("\x1b[16;3HThen turn the power off\n");
    iprintf("\x1b[17;5Hand on again.\n");

    for (;;) swiWaitForVBlank();
}

/* Called once per frame from the runtime. */
void card_check(void) {
#if CARD_CHECK_ENABLED
    if (!s_ready || s_busy) return;
    if (--s_countdown > 0) return;
    s_countdown = CHECK_INTERVAL;

    if (header_fingerprint() != s_baseline) card_halt();
#endif
}
