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
#define CHECK_INTERVAL     120

extern void apu_silence(void);

static u32 s_baseline;
static int s_busy;
static int s_ready;
static int s_countdown = CHECK_INTERVAL;
static int s_strikes;

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
#define PROBE_ASYNC   1
#define ASYNC_TIMEOUT 60      /* frames to wait before giving up on a read */
#define ASYNC_DMA_CH  2       /* 3 is libnds' dmaCopy, used by video_flush */

#ifndef CARD_BUSY
#define CARD_BUSY (1u << 31)
#endif
#ifndef CARD_BLK_SIZE
#define CARD_BLK_SIZE(n) ((n) << 24)
#endif

static u32 s_async_buf[512 / 4] __attribute__((aligned(4)));
static int s_async_pending;
static int s_async_wait;

static void async_start(void) {
    u8 cmd[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };   /* 0x00 = read header */
    u32 flags = __NDSHeader->cardControl13 | CARD_BLK_SIZE(1) | CARD_ACTIVATE
              | CARD_nRESET;

    memset(s_async_buf, 0, sizeof s_async_buf);
    cardStartTransfer(cmd, s_async_buf, ASYNC_DMA_CH, flags);
    s_async_pending = 1;
    s_async_wait = 0;
}

static u32 buf_fingerprint(void) {
    const u8 *p = (const u8 *)s_async_buf;
    u32 h = 2166136261u;
    for (unsigned i = 0; i < sizeof s_async_buf; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static u32 card_fingerprint(void) {
#if PROBE_WITH_ID
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
#if PROBE_ASYNC
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

            u32 fp = buf_fingerprint();
            if (fp != s_baseline) {
                if (++s_strikes >= 2) card_halt();
            } else {
                s_strikes = 0;
            }
        } else if (++s_async_wait > ASYNC_TIMEOUT) {
            /* Never completed. Abandon it rather than waiting forever: a
             * silently disabled check is far better than a frozen console. */
            s_async_pending = 0;
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
