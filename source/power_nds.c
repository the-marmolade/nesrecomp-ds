/* power_nds.c — sleep when the lid closes.
 *
 * Shipped DS titles were required to sleep on lid close: the screens and
 * sound go off, the ARM9 stops, and the console draws almost nothing until
 * the lid opens again. Homebrew that skips this drains a battery in a bag.
 *
 * libnds exposes the lid as an interrupt (IRQ_LID) and provides
 * systemSleep(), which handles powering the panels and backlights down and
 * bringing them back. What it does NOT do is deal with audio: channels left
 * running keep the sound hardware powered and, worse, resume mid-note on
 * wake. So the channels are silenced going down and left for the APU to
 * reprogram on the next frame after waking.
 */
#include <nds.h>
#include "nes_runtime.h"

extern void apu_silence(void);

/* The lid used to be available as IRQ_LID, but that aliases IRQ_HINGE, which
 * the calico rework of libnds removed - the same change that stopped DeSmuME
 * running new homebrew. The lid state is still exposed through the key bits,
 * so poll it once per frame instead. A frame of latency closing the lid is
 * imperceptible. */
#ifndef KEY_LID
#define KEY_LID KEY_HINGE
#endif

void power_init(void) { /* nothing to set up when polling */ }

/* Called once per frame from the runtime. Returns immediately unless the lid
 * has been closed, in which case it sleeps and only returns once the lid is
 * open again — so the caller simply loses time, which is what a paused
 * handheld should look like from the game's point of view. */
void power_check_lid(void) {
    /* poll_input() has already called scanKeys() this frame. */
    if (!(keysHeld() & KEY_LID)) return;

    /* Leaving channels running keeps the sound hardware awake through the
     * sleep, and they resume mid-note on wake. */
    apu_silence();

    systemSleep();

    /* Back from sleep. The APU reprograms frequency and volume from the
     * game's register state on the next frame, so nothing needs restoring
     * here. Re-read the keys so the now-open lid isn't seen as still shut. */
    scanKeys();
}
