/* NDS runtime: ROM loading, NES memory map, vblank/NMI pump.
 * No video yet — PPU writes are recorded into shadow state only. */
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <string.h>
#include "nes_runtime.h"
#include "mapper.h"
#include "game_config.h"

extern void func_NMI(void);
extern void video_init(void);
void nes_timing_init(void);
extern void video_build(void);
extern void video_flush(void);
extern void apu_init(void);
extern void apu_frame(void);
extern void apu_write(uint16_t addr, uint8_t val);
extern uint8_t apu_read_status(void);
extern void apu_trace_dump_now(void);
extern void apu_trace_start(void);
extern void apu_test_tone_toggle(void);
extern unsigned apu_state_size(void);
extern void apu_state_save(void *dst);
extern void apu_state_load(const void *src);

int g_fat_ready = 0;      /* set by main() once fatInitDefault has run */
int nes_save_state(void);
int nes_load_state(void);

CPU6502State g_cpu;
uint8_t  g_ram[0x0800];
uint8_t  g_sram[0x2000];
uint8_t  g_chr_ram[0x2000];
uint8_t  g_ppu_oam[0x100];
uint8_t  g_ppu_pal[0x20];
uint8_t  g_ppu_nt[0x1000];
uint8_t  g_ppuctrl, g_ppumask, g_ppustatus;
uint8_t  g_ppuscroll_x, g_ppuscroll_y;
uint8_t  g_oamaddr;
uint8_t  g_controller1_buttons, g_controller2_buttons;
int      g_chr_is_rom = 1;
int      g_bail_active = 0;
uint16_t g_code_window_base = 0x8000;

static uint8_t  s_prg[0x8000];          /* NROM-256: 32K at $8000 */
static uint16_t s_ppuaddr;
static int      s_wtoggle;
static uint8_t  s_ctrl_shift[2];
static int      s_ctrl_strobe;

uint64_t g_frame_count = 0;

/* Debug overlay: L toggles it. Off by default — console writes are slow on
 * hardware, and printing every frame was costing real time. Timer 0/1 are
 * cascaded into one free-running 32-bit counter at the full bus clock, so a
 * tick is ~30ns and it wraps about every two minutes (we only ever diff over
 * a single second). */
static int      s_debug_on;
static uint32_t s_work_acc;      /* summed work ticks over 60 frames  */
static uint32_t s_total_acc;     /* summed frame PERIODS, start to start */
static uint16_t s_last_start;    /* previous frame's start tick */
static int      s_have_last;
static uint32_t s_fps, s_work_us;

/* One 16-bit timer at DIV_1024: 33513982/1024 = 32729 Hz, so a tick is
 * ~30.5us and it wraps every 2 seconds. Unsigned 16-bit subtraction wraps
 * correctly, so per-frame diffs are always right. A cascaded 32-bit pair
 * would give finer resolution but the two halves cannot be read atomically,
 * which is what produced the nonsense numbers. */
static inline uint16_t tmr(void) { return TIMER0_DATA; }

static void timing_init(void) {
    TIMER0_DATA = 0;
    TIMER0_CR   = TIMER_DIV_1024 | TIMER_ENABLE;
}

static inline uint32_t ticks_to_us(uint32_t ticks) {
    return (ticks * 3052u) / 100u;      /* 1 tick = 30.52us */
}

/* NTSC NES runs at 60.0988 Hz; the DS panel is fixed at 59.8261 Hz, so one
 * NES frame per vblank leaves the game 0.456% slow. Accumulate the shortfall
 * and run one extra NES frame whenever it reaches a whole frame — about every
 * 219 DS frames. That frame is emulated and drawn but never displayed, which
 * is the correct trade: game time matches the NES exactly and the dropped
 * frame is far below the threshold of perception.
 *   (60.0988 / 59.8261 - 1) * 2^24 = 76476 */
#define NES_RATE_FRAC  76476u
#define NES_RATE_ONE   (1u << 24)
static uint32_t s_rate_acc;
static int      s_catchup;


uint64_t g_nmi_count = 0;
static uint16_t s_last_pc;
static uint32_t s_defer_count;

/* Nametable mirroring comes from the iNES header via the mapper, not from a
 * per-game constant. Vertical: $2000/$2800 are one physical table and
 * $2400/$2C00 the other. Horizontal: the pairs are $2000/$2400 and
 * $2800/$2C00. Single-screen maps everything onto one table. */
static int s_mirroring = MIRROR_VERTICAL;

static inline uint16_t nt_index(uint16_t a) {
    switch (s_mirroring) {
    case MIRROR_HORIZONTAL: return (uint16_t)((a & 0x3FF) | ((a >> 1) & 0x400));
    case MIRROR_ONE_LOWER:  return (uint16_t)(a & 0x3FF);
    case MIRROR_ONE_UPPER:  return (uint16_t)((a & 0x3FF) | 0x400);
    default:                return (uint16_t)(a & 0x7FF);   /* vertical */
    }
}

/* $3F10/$3F14/$3F18/$3F1C are the SAME bytes as $3F00/$3F04/$3F08/$3F0C —
 * sprite palette entry 0 and background entry 0 are one physical location. */
static inline uint8_t pal_index(uint16_t a) {
    uint8_t i = a & 0x1F;
    if ((i & 0x13) == 0x10) i &= ~0x10;   /* 10,14,18,1C -> 00,04,08,0C */
    return i;
}

#define CYCLES_PER_FRAME 29781
static uint32_t s_ops;
static int      s_nmi_depth;

int nes_rom_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hdr[16];
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "NES\x1A", 4)) { fclose(f); return 0; }
    int prg16k = hdr[4], chr8k = hdr[5];
    if (prg16k == 1) {                   /* NROM-128: mirror 16K twice */
        fread(s_prg, 1, 0x4000, f);
        memcpy(s_prg + 0x4000, s_prg, 0x4000);
    } else {
        fread(s_prg, 1, 0x8000, f);
    }
    if (chr8k) fread(g_chr_ram, 1, 0x2000, f);
    fclose(f);

    /* iNES byte 6: bit 0 is mirroring, bits 4-7 the low nibble of the mapper
     * number; byte 7 carries the high nibble. s_prg is static so the pointer
     * stays valid for the life of the program. */
    int mapper = (hdr[6] >> 4) | (hdr[7] & 0xF0);
    mapper_init(s_prg, (prg16k == 1) ? 1 : 2, mapper, hdr[6] & 1);
    if (chr8k) mapper_init_chr(g_chr_ram, chr8k);
    g_chr_is_rom = chr8k ? 1 : 0;
    s_mirroring  = mapper_get_mirroring();

    iprintf("%s: mapper %d, %s\n", g_game->name, mapper,
            s_mirroring == MIRROR_VERTICAL ? "vertical" : "horizontal");
    return 1;
}

uint8_t nes_read(uint16_t addr) {
    if (addr < 0x2000) return g_ram[addr & 0x07FF];
    if (addr < 0x4000) {
        switch (addr & 7) {
        case 2: {                        /* PPUSTATUS: read clears vblank + toggle */
            uint8_t v = g_ppustatus;
            g_ppustatus &= ~0x80;
            s_wtoggle = 0;
            /* HACK: no dot-accurate PPU yet. SMB polls bit 6 twice per frame —
             * Sprite0Clr ($813D) waits for it LOW, Sprite0Hit ($8150) waits for
             * it HIGH. Derive it from position within the frame rather than a
             * read count, so it stays correct however often the game polls.
             * sprite0_cycle comes from the game config:
             * 2273 vblank cycles + 32 * 113.67 cycles/scanline. */
            if (g_game->sprite0_cycle && s_ops >= g_game->sprite0_cycle &&
                s_ops < CYCLES_PER_FRAME - 1000)
                g_ppustatus |= 0x40;
            else
                g_ppustatus &= ~0x40;
            return v;
        }
        case 7: {                        /* PPUDATA */
            uint16_t a = s_ppuaddr & 0x3FFF;
            s_ppuaddr += (g_ppuctrl & 0x04) ? 32 : 1;
            if (a < 0x2000) return g_chr_ram[a];
            if (a < 0x3F00) return g_ppu_nt[nt_index(a)];
            return g_ppu_pal[pal_index(a)];
        }
        default: return 0;
        }
    }
    if (addr == 0x4015) return apu_read_status();
    if (addr == 0x4016 || addr == 0x4017) {
        int p = addr & 1;
        uint8_t bit = s_ctrl_shift[p] & 1;
        s_ctrl_shift[p] >>= 1;
        return bit | 0x40;
    }
    if (addr >= 0x6000 && addr < 0x8000) return g_sram[addr - 0x6000];
    /* Through the mapper rather than indexing s_prg directly: NROM is a flat
     * 32K window, but MMC1/MMC3 switch banks and mapper_peek_prg() already
     * knows the current mapping. */
    if (addr >= 0x8000) return mapper_peek_prg(addr);
    return 0;
}

void nes_write(uint16_t addr, uint8_t val) {
    if (addr < 0x2000) { g_ram[addr & 0x07FF] = val; return; }
    if (addr < 0x4000) {
        switch (addr & 7) {
        case 0: g_ppuctrl = val; return;
        case 1: g_ppumask = val; return;
        case 3: g_oamaddr = val; return;
        case 4: g_ppu_oam[g_oamaddr++] = val; return;
        case 5:
            if (!s_wtoggle) g_ppuscroll_x = val; else g_ppuscroll_y = val;
            s_wtoggle ^= 1;
            return;
        case 6:
            if (!s_wtoggle) s_ppuaddr = (s_ppuaddr & 0x00FF) | ((uint16_t)val << 8);
            else            s_ppuaddr = (s_ppuaddr & 0xFF00) | val;
            s_wtoggle ^= 1;
            return;
        case 7: {
            uint16_t a = s_ppuaddr & 0x3FFF;
            if (a >= 0x2000 && a < 0x3F00)      g_ppu_nt[nt_index(a)] = val;
            else if (a >= 0x3F00)               g_ppu_pal[pal_index(a)] = val;
            else if (!g_chr_is_rom)             g_chr_ram[a] = val;
            s_ppuaddr += (g_ppuctrl & 0x04) ? 32 : 1;
            return;
        }
        default: return;
        }
    }
    if (addr == 0x4014) {                /* OAM DMA */
        uint16_t base = (uint16_t)val << 8;
        for (int i = 0; i < 256; i++) g_ppu_oam[(uint8_t)(g_oamaddr + i)] = nes_read(base + i);
        s_ops += 513;
        return;
    }
    if (addr == 0x4016) {
        int was = s_ctrl_strobe;
        s_ctrl_strobe = val & 1;
        if (was && !s_ctrl_strobe) {
            s_ctrl_shift[0] = g_controller1_buttons;
            s_ctrl_shift[1] = g_controller2_buttons;
        }
        return;
    }
    if (addr >= 0x4000 && addr <= 0x4015) { apu_write(addr, val); return; }
    if (addr >= 0x6000 && addr < 0x8000) { g_sram[addr - 0x6000] = val; return; }
    /* Writes above $8000 are mapper register writes, not ROM. Harmless on
     * NROM, essential for anything that bank switches. */
    if (addr >= 0x8000) { mapper_write(addr, val); s_mirroring = mapper_get_mirroring(); }
}

/* ---------- save states ----------
 *
 * Saving happens at the frame boundary, with the 6502 parked in its idle loop
 * and the C call stack always main -> func_RESET -> func_8000_b0. Because that
 * stack is identical on every save and every load, restoring is just a matter
 * of overwriting memory and carrying on — no need to unwind or rebuild
 * anything. The 6502's own stack lives inside g_ram, so it comes along too.
 *
 * CHR is not saved: it is ROM for NROM titles and reloaded at startup. */
#define SAVE_MAGIC 0x4E44534Bu   /* "NDSK" */
#define SAVE_VER   1

typedef struct {
    uint32_t     magic, version;
    CPU6502State cpu;
    uint8_t      ram[0x800];
    uint8_t      sram[0x2000];
    uint8_t      nt[0x1000];
    uint8_t      pal[0x20];
    uint8_t      oam[0x100];
    uint8_t      ppuctrl, ppumask, ppustatus;
    uint8_t      scroll_x, scroll_y, oamaddr;
    uint16_t     ppuaddr;
    int32_t      wtoggle;
    uint16_t     code_window;
    uint32_t     ops;
    uint8_t      apu[128];
} SaveState;

static SaveState s_save;    /* static: 21KB is too much for the stack */

static const char *save_path(void) { return "/smb.sav"; }

int nes_save_state(void) {
    if (!g_fat_ready) return 0;
    s_save.magic   = SAVE_MAGIC;
    s_save.version = SAVE_VER;
    s_save.cpu     = g_cpu;
    memcpy(s_save.ram,  g_ram,     sizeof s_save.ram);
    memcpy(s_save.sram, g_sram,    sizeof s_save.sram);
    memcpy(s_save.nt,   g_ppu_nt,  sizeof s_save.nt);
    memcpy(s_save.pal,  g_ppu_pal, sizeof s_save.pal);
    memcpy(s_save.oam,  g_ppu_oam, sizeof s_save.oam);
    s_save.ppuctrl   = g_ppuctrl;
    s_save.ppumask   = g_ppumask;
    s_save.ppustatus = g_ppustatus;
    s_save.scroll_x  = g_ppuscroll_x;
    s_save.scroll_y  = g_ppuscroll_y;
    s_save.oamaddr   = g_oamaddr;
    s_save.ppuaddr   = s_ppuaddr;
    s_save.wtoggle   = s_wtoggle;
    s_save.code_window = g_code_window_base;
    s_save.ops       = s_ops;
    if (apu_state_size() <= sizeof s_save.apu) apu_state_save(s_save.apu);

    FILE *f = fopen(save_path(), "wb");
    if (!f) return 0;
    size_t n = fwrite(&s_save, 1, sizeof s_save, f);
    fclose(f);
    return n == sizeof s_save;
}

int nes_load_state(void) {
    if (!g_fat_ready) return 0;
    FILE *f = fopen(save_path(), "rb");
    if (!f) return 0;
    size_t n = fread(&s_save, 1, sizeof s_save, f);
    fclose(f);
    if (n != sizeof s_save) return 0;
    if (s_save.magic != SAVE_MAGIC || s_save.version != SAVE_VER) return 0;

    g_cpu = s_save.cpu;
    memcpy(g_ram,     s_save.ram,  sizeof s_save.ram);
    memcpy(g_sram,    s_save.sram, sizeof s_save.sram);
    memcpy(g_ppu_nt,  s_save.nt,   sizeof s_save.nt);
    memcpy(g_ppu_pal, s_save.pal,  sizeof s_save.pal);
    memcpy(g_ppu_oam, s_save.oam,  sizeof s_save.oam);
    g_ppuctrl     = s_save.ppuctrl;
    g_ppumask     = s_save.ppumask;
    g_ppustatus   = s_save.ppustatus;
    g_ppuscroll_x = s_save.scroll_x;
    g_ppuscroll_y = s_save.scroll_y;
    g_oamaddr     = s_save.oamaddr;
    s_ppuaddr     = s_save.ppuaddr;
    s_wtoggle     = s_save.wtoggle;
    g_code_window_base = s_save.code_window;
    s_ops         = s_save.ops;
    if (apu_state_size() <= sizeof s_save.apu) apu_state_load(s_save.apu);
    return 1;
}

uint16_t nes_read16zp(uint8_t zp) {
    return (uint16_t)g_ram[zp] | ((uint16_t)g_ram[(uint8_t)(zp + 1)] << 8);
}

/* The NES shift register clocks out A first, then B, Select, Start, Up,
 * Down, Left, Right. nes_read() shifts right and returns bit 0, so A must
 * sit in bit 0 and Right in bit 7 — not the other way round. */
static void poll_input(void) {
    scanKeys();
    int k = keysHeld();
    uint8_t b = 0;
    if (k & KEY_A)      b |= 0x01;   /* NES A — jump */
    if (k & KEY_B)      b |= 0x02;   /* NES B — run/fire */
    if (k & KEY_SELECT) b |= 0x04;
    if (k & KEY_START)  b |= 0x08;
    if (k & KEY_UP)     b |= 0x10;
    if (k & KEY_DOWN)   b |= 0x20;
    if (k & KEY_LEFT)   b |= 0x40;
    if (k & KEY_RIGHT)  b |= 0x80;
    g_controller1_buttons = b;

    if (keysDown() & KEY_L) {
        s_debug_on = !s_debug_on;
        if (!s_debug_on) iprintf("\x1b[2J");   /* clear on the way out */
    }

    /* R + B toggles a known A440 test tone, for checking PSG_SCALE. */
    if ((k & KEY_R) && (keysDown() & KEY_B)) apu_test_tone_toggle();

    /* R + Select arms APU tracing, R + L writes it out. Not on by default:
     * the console chatter gets in the way of audio recording. */
    if ((k & KEY_R) && (keysDown() & KEY_SELECT)) apu_trace_start();
    if ((k & KEY_R) && (keysDown() & KEY_L))      apu_trace_dump_now();

    /* R + X saves, R + Y loads. Held R avoids accidental presses mid-jump. */
    if ((k & KEY_R) && (keysDown() & KEY_X))
        iprintf(nes_save_state() ? "state saved\n" : "save failed\n");
    if ((k & KEY_R) && (keysDown() & KEY_Y))
        iprintf(nes_load_state() ? "state loaded\n" : "load failed\n");
}

void nes_timing_init(void) { timing_init(); }

void maybe_trigger_vblank(int cycles) {
    s_ops += (cycles > 0) ? (uint32_t)cycles : 1;
    if (s_ops < CYCLES_PER_FRAME) return;

    if (s_nmi_depth) {
        /* Still inside func_NMI a whole frame later — it isn't returning.
         * Report where the 6502 is spinning instead of hanging silently. */
        s_ops -= CYCLES_PER_FRAME;
        if ((++s_defer_count % 30) == 1)
            iprintf("in NMI %lu frames, pc=%04X A=%02X X=%02X Y=%02X\n",
                    (unsigned long)s_defer_count, s_last_pc,
                    g_cpu.A, g_cpu.X, g_cpu.Y);
        return;
    }

    s_ops -= CYCLES_PER_FRAME;
    g_ppustatus |= 0x80;                 /* vblank starts */
    g_ppustatus &= ~0x40;                /* sprite 0 hit clears each frame */

    poll_input();
    uint16_t t_start = tmr();
    /* Frame period must be measured start-to-start. Measuring from here to
     * the end of video_flush misses the tail of the frame — the recompiled
     * main loop still running until the next budget expiry — which made the
     * period look short and fps read high. */
    if (s_have_last) s_total_acc += (uint16_t)(t_start - s_last_start);
    s_last_start = t_start;
    s_have_last  = 1;

    if (g_ppuctrl & 0x80) {
        s_nmi_depth++;
        g_nmi_count++;
        /* Push the 6502 NMI hardware frame: PC high, PC low, status. The
         * generated func_NMI() ends in RTI, which pops all three — without
         * this push the stack pointer climbs by 3 every frame until a PHA
         * lands on the PPUCTRL byte the handler saved, which is what was
         * producing ctrl=0x8B and the flashing. $0000 is the sentinel PC the
         * recompiler expects; RTI reports any hijacked target separately. */
        g_ram[0x100 + g_cpu.S--] = 0x00;   /* PC high */
        g_ram[0x100 + g_cpu.S--] = 0x00;   /* PC low  */
        g_ram[0x100 + g_cpu.S--] =
            (uint8_t)((g_cpu.N << 7) | (g_cpu.V << 6) | 0x20 |
                      (g_cpu.D << 3) | (g_cpu.I << 2) |
                      (g_cpu.Z << 1) |  g_cpu.C);
        func_NMI();
        s_nmi_depth--;
    }

    /* Wait until vblank BEFORE touching VRAM. video_frame() rewrites the whole
     * BG map and all of OAM; doing that while the display is scanning them out
     * tears and flickers. func_NMI() is most of a frame's work, so waiting
     * first (as this used to) put the writes right in the middle of display. */
    video_build();          /* heavy work: RAM shadows, outside vblank */
    apu_frame();

    /* Work time excludes the vblank wait: it is what we spend emulating and
     * building the frame, so anything approaching 16.7ms means dropped frames. */
    s_work_acc += (uint16_t)(tmr() - t_start);

    /* Skip the wait on a catch-up frame so two NES frames land inside one
     * DS frame; the first one's output is simply overwritten. */
    if (s_catchup) s_catchup = 0;
    else           swiWaitForVBlank();

    video_flush();

    s_rate_acc += NES_RATE_FRAC;
    if (s_rate_acc >= NES_RATE_ONE) {
        s_rate_acc -= NES_RATE_ONE;
        s_catchup = 1;
    }
    /* Do NOT clear the vblank flag here — on hardware it stays set until the
     * CPU reads $2002, and nes_read() does that. Clearing it here means the
     * RESET spin-wait at $800A never sees it and the game never starts. */

    if ((++g_frame_count % 60) == 0) {
        /* Average first, then scale — 60 * 65535 * 3052 overflows 32 bits. */
        /* Average first, then scale. fps is kept x100: integer 1000000/us
         * lands right on the 59/60 boundary and flips with rounding, which
         * looks like instability when the timing is actually fine. */
        uint32_t frame_us = ticks_to_us(s_total_acc / 60u);
        s_work_us = ticks_to_us(s_work_acc  / 60u);
        s_fps     = frame_us ? (100000000u / frame_us) : 0;
        s_work_acc = s_total_acc = 0;

        if (s_debug_on) {
            /* Home the cursor instead of scrolling — scrolling the console is
             * itself slow enough to skew the numbers. */
            iprintf("\x1b[0;0Hfps %2lu.%02lu  work %2lu.%02lums   \n",
                    (unsigned long)(s_fps / 100), (unsigned long)(s_fps % 100),
                    (unsigned long)(s_work_us / 1000),
                    (unsigned long)((s_work_us % 1000) / 10));
            iprintf("om=%02X ot=%02X srt=%02X ges=%02X  \n",
                    g_ram[0x0770], g_ram[0x0772], g_ram[0x073C], g_ram[0x000E]);
            /* Should read ~60.1, not 59.8, once rate matching is working. */
        }
    }
}

void nes_instruction_boundary(uint16_t pc, int cycles) {
    s_last_pc = pc; maybe_trigger_vblank(cycles);
}
void nes_cpu_instruction_boundary(uint16_t pc, int cycles) {
    s_last_pc = pc; maybe_trigger_vblank(cycles);
}
int  coroutine_scheduler_setjmp(void)                      { return 0; }
