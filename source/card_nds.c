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

/* 0 - the probe halts again. Set to 1 to watch the counters without halting,
 * which is how the async overrun was found: the numbers came back impossible
 * (more completions than starts) and then as 0xFFFFFFFF, which is open-bus
 * data flooding past the buffer. Worth reaching for before theorising if this
 * ever misbehaves again. */
#define CARD_DIAGNOSE 0
/* Frames between probes. The header read is a card-bus transfer that takes
 * long enough to push its frame past the 16.7ms budget, and with vsync that
 * costs a whole frame rather than a fraction - probing every second measured
 * as a steady 59fps instead of 60.
 *
 * The read also varies - sometimes it costs two frames rather than one - so
 * spacing the probes out is the only lever that works. At every 10 seconds
 * the occasional dropped frame is imperceptible, and detection latency of
 * 10-20 seconds is still far quicker than anyone can pull a cart and act on
 * it. A retail cart's detection was not instantaneous either. */
/* With the async probe this is cheap - starting a transfer and polling a
 * register are both trivial - so it can run often again. Kept at 2 seconds
 * rather than every frame because there is no reason to hammer the card bus
 * the flashcart also uses for SD. */
/* The 4-byte probe is cheap enough to run often. Two seconds keeps removal
 * detection prompt (4s worst case with two strikes) without hammering a bus
 * the flashcart also uses for SD. */
#define CHECK_INTERVAL     120

extern void apu_silence(void);

static u32 s_baseline;
static int s_busy;
static int s_ready;
static int s_countdown = CHECK_INTERVAL;
static int s_strikes;

/* Diagnostic counters, read by the debug overlay. */
unsigned g_card_started, g_card_done, g_card_timeout, g_card_mismatch;
unsigned g_card_last_fp, g_card_base_fp, g_card_overrun;

/* Which probe to use. cardReadHeader pulls 512 bytes over the card bus and
 * measured expensive enough to cost one frame, sometimes two - the fps
 * counter went from a steady 60 to dipping at 58. cardReadID is a single
 * command returning 4 bytes.
 *
 * Its flags argument is undocumented for our case, but that does not matter:
 * we never interpret the value, only check that it keeps coming back the
 * same. If a flashcart returns something unstable, set this to 0 and accept
 * the frame cost - or turn the whole check off. */
/* 0. cardReadID(0) hangs on a DSpico - the boot menu draws and then the
 * console goes black, because card_init() never returns. Its flags argument
 * evidently does matter, and there is no documented value for a flashcart.
 * The header read is slower but it works. */
#define PROBE_WITH_ID 0

/* Asynchronous probe.
 *
 * The blocking header read costs a whole frame every time it runs, because
 * with vsync anything over 16.7ms loses a vblank outright. cardStartTransfer
 * hands the read to DMA and returns immediately, so the cost disappears - we
 * start a transfer on one frame and collect it on a later one.
 *
 * The ROMCTRL flags come from the cart header the loader left in RAM
 * (measured 00586000 on a DSpico, the standard normal-command value), which
 * is where retail titles get them too.
 *
 * Completion is polled rather than interrupt-driven: reading REG_ROMCTRL once
 * a frame is free, and it avoids installing another IRQ handler alongside the
 * HBlank one. A transfer that never finishes times out and is abandoned - the
 * check quietly stops rather than hanging the console, which is how
 * cardReadID failed. */
/*
 * Handing the probe to DMA removed the dropped frame, but broke the feature
 * in both directions: a card that was pulled no longer halted the game
 * (#17), because a transfer that never completes was being abandoned rather
 * than treated as the signal it is; and saving or loading halted the game
 * falsely (#16), because a probe in flight when libfat took the bus came
 * back corrupted. Fixing each one individually did not make the pair work.
 *
 * The blocking probe costs one dropped frame per read and passes all three
 * tests - no false halt while playing, no false halt while saving, and a
 * reliable halt on removal. That is the better trade: a dropped frame every
 * ten seconds is imperceptible, a feature that silently stops working is
 * not. */
/* 0 - abandoned. Handing the read to DMA scribbled 0xFF over memory well past
 * the destination buffer: the diagnostic counters came back as impossible
 * values and then as 0xFFFFFFFF outright, which is open-bus data flooding
 * through a 4KB buffer, through a canary behind it, and into whatever came
 * next. Three attempts to tame it failed, and the cost of getting it wrong is
 * silent corruption somewhere unrelated. Left here documented rather than
 * deleted so nobody tries it again without knowing. */
#define PROBE_ASYNC   0

/* The real problem was never that the read blocked - it was that it read 512
 * bytes. cardPolledTransfer takes a length, and the ROMCTRL block-size field
 * has a setting for a 4-byte transfer. Four bytes is microseconds: it fits in
 * the frame's slack without costing a vblank, with no DMA, no completion
 * polling and no buffer to overrun.
 *
 * Removal is still detected: with no card, the bus floats high and the read
 * comes back 0xFFFFFFFF, which is simply a mismatch against the baseline. */
#define PROBE_SMALL   1
#ifndef CARD_BLK_SIZE_4BYTE
#define CARD_BLK_SIZE_4BYTE (7u << 24)
#endif
/* A healthy transfer completes within a frame or two. 20 frames is generous
 * for a working card and keeps removal detection prompt - with two strikes
 * needed, the worst case is about two probe intervals. */
#define ASYNC_TIMEOUT 20
#define ASYNC_DMA_CH  2       /* 3 is libnds' dmaCopy, used by video_flush */

#ifndef CARD_BUSY
#define CARD_BUSY (1u << 31)
#endif
#ifndef CARD_BLK_SIZE
#define CARD_BLK_SIZE(n) ((n) << 24)
#endif

/* Deliberately eight times larger than the 512 bytes a header read should
 * produce, and padded after.
 *
 * The diagnostic counters came back impossible - more completions than
 * starts, more mismatches than completions, and a timeout counter wrapped
 * below zero - which means something was writing past this buffer into the
 * globals that follow it. The only thing writing here is the card DMA, so it
 * is transferring more than was allocated for it. Whether the block size in
 * the ROMCTRL flags means something different to this flashcart or libnds
 * rounds the length up, the fix is the same: give it room it cannot overrun,
 * and only ever fingerprint the first 512 bytes. */
static u32 s_async_buf[4096 / 4] __attribute__((aligned(32)));
static u32 s_async_guard[64];            /* canary: must stay zero */
static int s_async_pending;
static int s_async_wait;

static void async_start(void) {
    u8 cmd[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };   /* 0x00 = read header */
    u32 flags = __NDSHeader->cardControl13 | CARD_BLK_SIZE(1) | CARD_ACTIVATE
              | CARD_nRESET;

    memset(s_async_buf, 0, sizeof s_async_buf);
    memset(s_async_guard, 0, sizeof s_async_guard);
    DC_FlushRange(s_async_buf, sizeof s_async_buf);
    cardStartTransfer(cmd, s_async_buf, ASYNC_DMA_CH, flags);
    s_async_pending = 1;
    s_async_wait = 0;
    g_card_started++;
}

static u32 buf_fingerprint(void) {
    const u8 *p = (const u8 *)s_async_buf;
    u32 h = 2166136261u;
    for (unsigned i = 0; i < 512; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static u32 card_fingerprint(void) {
#if PROBE_SMALL
    /* Padded well beyond the 4 bytes requested, and checked afterwards: the
     * async attempt overran its buffer badly, so this one verifies rather
     * than assumes. */
    static u32 buf[16];
    u8 cmd[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };      /* 0x00 = read header */
    u32 flags = (__NDSHeader->cardControl13 & ~(7u << 24))
              | CARD_BLK_SIZE_4BYTE | CARD_ACTIVATE | CARD_nRESET;

    memset(buf, 0, sizeof buf);
    cardPolledTransfer(flags, buf, 1, cmd);

    for (unsigned i = 1; i < 16; i++)
        if (buf[i]) { g_card_overrun++; break; }

    return buf[0];
#elif PROBE_WITH_ID
    return cardReadID(0);
#else
    u8 header[512];
    memset(header, 0, sizeof header);
    cardReadHeader(header);

    u32 h = 2166136261u;
    for (unsigned i = 0; i < sizeof header; i++) {
        h ^= header[i];
        h *= 16777619u;
    }
    return h;
#endif
}

void card_init(void) {
#if CARD_CHECK_ENABLED
#if PROBE_ASYNC && !PROBE_SMALL
    /* Baseline with the async path itself, waiting for it once here. Taking
     * it from cardReadHeader instead would compare fingerprints of two
     * different transfers and mismatch every time. This is the one blocking
     * wait, at startup, where a frame costs nothing. */
    async_start();
    for (int i = 0; i < ASYNC_TIMEOUT && (REG_ROMCTRL & CARD_BUSY); i++)
        swiWaitForVBlank();
    s_async_pending = 0;
    DC_InvalidateRange(s_async_buf, sizeof s_async_buf);
    s_baseline = buf_fingerprint();
#else
    s_baseline = card_fingerprint();
#endif
    s_ready = 1;
#endif
}

void card_set_busy(int busy) {
    if (busy) {
        /* libfat is about to use the same bus. Any probe in flight would be
         * clobbered by it, and reading the result afterwards saw corrupted
         * data and halted the game mid-save (issue #16). Drop it, and do not
         * start another until well after the SD work has finished. */
        s_async_pending = 0;
        s_strikes = 0;
        s_countdown = CHECK_INTERVAL;
    } else {
        s_countdown = CHECK_INTERVAL;   /* let the bus settle before probing */
    }
    s_busy = busy;
}

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

/* Called once per frame from the runtime, immediately after the vblank wait
 * so the read lands in the part of the frame with the most slack - work time
 * is around 8ms of the 16.7ms budget, so a read that fits in the remainder
 * costs nothing at all. */
void card_check(void) {
#if CARD_CHECK_ENABLED
    if (!s_ready || s_busy) return;

#if PROBE_ASYNC
    /* Collect a transfer started on an earlier frame. Reading REG_ROMCTRL is
     * a single register access, so this costs nothing. */
    if (s_async_pending) {
        if (!(REG_ROMCTRL & CARD_BUSY)) {
            s_async_pending = 0;
            DC_InvalidateRange(s_async_buf, sizeof s_async_buf);
            g_card_done++;

            /* Did the transfer stay inside its buffer? Any non-zero here
             * means it overran, and the counters above cannot be trusted. */
            DC_InvalidateRange(s_async_guard, sizeof s_async_guard);
            for (unsigned i = 0; i < sizeof s_async_guard / 4; i++)
                if (s_async_guard[i]) { g_card_overrun++; break; }

            u32 fp = buf_fingerprint();
            g_card_last_fp = fp;
            g_card_base_fp = s_baseline;
            if (fp != s_baseline) {
                g_card_mismatch++;
#if !CARD_DIAGNOSE
                if (++s_strikes >= 2) card_halt();
#endif
            } else {
                s_strikes = 0;
            }
        } else if (++s_async_wait > ASYNC_TIMEOUT) {
            /* A transfer that never completes IS the removal signal - with no
             * card there, nothing answers. Abandoning it quietly (as this did)
             * meant ejecting the card stopped being detected at all once the
             * probe went asynchronous (issue #17).
             *
             * Still two strikes, so one stalled transfer cannot halt a working
             * game on its own. */
            s_async_pending = 0;
            g_card_timeout++;
#if !CARD_DIAGNOSE
            if (++s_strikes >= 2) card_halt();
#endif
        }
        return;
    }

    if (--s_countdown > 0) return;
    s_countdown = CHECK_INTERVAL;
    async_start();
#else
    if (--s_countdown > 0) return;
    s_countdown = CHECK_INTERVAL;

    /* Two consecutive mismatches before halting. A single odd read on a
     * flashcart would otherwise stop a perfectly good game, and the cost of
     * being wrong here is much higher than more seconds of latency. */
    if (card_fingerprint() != s_baseline) {
        if (++s_strikes >= 2) card_halt();
    } else {
        s_strikes = 0;
    }
#endif
#endif
}
