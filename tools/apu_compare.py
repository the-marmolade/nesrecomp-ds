#!/usr/bin/env python3
"""
apu_compare.py - diff the DS APU mapping against a reference NES APU.

The recompiled game is identical on DS and PC, so the APU register writes are
identical too. That makes the register stream a ground truth: replay it through
a reference model of what the NES hardware would do, replay it through a model
of what apu_nds.c does, and the differences are the bugs - no audio recording
or waveform alignment needed.

Constants match nesrecomp's own apu.c (CPU_FREQ 1789773, the standard noise
period table), so "reference" here means the same thing the PC build sounds
like.

Usage:
    python3 tools/apu_compare.py apu_trace.txt
    python3 tools/apu_compare.py apu_trace.txt --noise-scale 8
"""
import argparse
import sys
from collections import defaultdict

CPU_HZ = 1789773

LEN_TABLE = [10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
             12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30]

NOISE_PERIOD = [4, 8, 16, 32, 64, 96, 128, 160,
                202, 254, 380, 508, 762, 1016, 2034, 4068]

# DS PSG duty indices: 0=12.5% 1=25% 2=37.5% 3=50% ...
# NES duty: 0=12.5% 1=25% 2=50% 3=25%-negated
DUTY_MAP = [0, 1, 3, 1]


class Chan:
    def __init__(self):
        self.reg = [0, 0, 0, 0]
        self.length = 0
        self.env_start = False
        self.env_div = 0
        self.env_decay = 0

    def volume(self):
        if not self.length:
            return 0
        return (self.reg[0] & 0x0F) if (self.reg[0] & 0x10) else self.env_decay

    def period(self):
        return ((self.reg[3] & 7) << 8) | self.reg[2]


def clock_envelope(c):
    if c.env_start:
        c.env_start = False
        c.env_decay = 15
        c.env_div = c.reg[0] & 0x0F
    else:
        c.env_div -= 1
        if c.env_div < 0:
            c.env_div = c.reg[0] & 0x0F
            if c.env_decay > 0:
                c.env_decay -= 1
            elif c.reg[0] & 0x20:
                c.env_decay = 15


def clock_length(c):
    if c.length > 0 and not (c.reg[0] & 0x20):
        c.length -= 1


def read_trace(path):
    frames = defaultdict(list)
    with open(path, errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) != 3:
                continue
            try:
                frame = int(parts[0])
                addr = int(parts[1], 16)
                val = int(parts[2], 16)
            except ValueError:
                continue
            frames[frame].append((addr, val))
    return frames


def simulate(frames, noise_scale):
    p1, p2, noise = Chan(), Chan(), Chan()
    tri = {"reg": [0, 0, 0, 0], "length": 0, "lin": 0, "reload": 0}
    seq = 0
    rows = []

    for frame in sorted(frames):
        for addr, val in frames[frame]:
            if 0x4000 <= addr <= 0x4003:
                c, i = p1, addr - 0x4000
            elif 0x4004 <= addr <= 0x4007:
                c, i = p2, addr - 0x4004
            elif addr in (0x400C, 0x400E, 0x400F):
                c, i = noise, addr - 0x400C
            elif addr in (0x4008, 0x400A, 0x400B):
                tri["reg"][addr - 0x4008] = val
                if addr == 0x4008:
                    tri["reload"] = val & 0x7F
                elif addr == 0x400B:
                    tri["length"] = LEN_TABLE[(val >> 3) & 0x1F]
                continue
            elif addr == 0x4015:
                for bit, ch in ((1, p1), (2, p2), (8, noise)):
                    if not (val & bit):
                        ch.length = 0
                if not (val & 4):
                    tri["length"] = 0
                continue
            else:
                continue
            c.reg[i] = val
            if i == 3:
                c.length = LEN_TABLE[(val >> 3) & 0x1F]
                c.env_start = True

        # frame sequencer: four 240Hz steps per video frame
        for _ in range(4):
            for c in (p1, p2, noise):
                clock_envelope(c)
            if seq & 1:
                for c in (p1, p2, noise):
                    clock_length(c)
            seq = (seq + 1) & 3

        row = {"frame": frame}
        for tag, c in (("p1", p1), ("p2", p2)):
            per, vol = c.period(), c.volume()
            hz = CPU_HZ // (16 * (per + 1)) if per >= 8 else 0
            row[tag] = (hz, vol, DUTY_MAP[(c.reg[0] >> 6) & 3])
            row[tag + "_clipped"] = hz > 8191
        nper = NOISE_PERIOD[noise.reg[2] & 0x0F]
        nhz = CPU_HZ // nper
        row["noise"] = (nhz, noise.volume())
        row["noise_param"] = nhz * noise_scale
        row["noise_clipped"] = nhz * noise_scale > 65535
        tper = ((tri["reg"][3] & 7) << 8) | tri["reg"][2]
        thz = CPU_HZ // (32 * (tper + 1)) if tper >= 2 else 0
        row["tri"] = (thz, tri["length"])
        row["tri_clipped"] = thz > 2000
        rows.append(row)

    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--noise-scale", type=int, default=1,
                    help="NOISE_SCALE in apu_nds.c (default 1)")
    ap.add_argument("--frames", type=int, default=20,
                    help="how many frames to print (default 20)")
    args = ap.parse_args()

    frames = read_trace(args.trace)
    if not frames:
        print("no register writes in that trace")
        return 1
    rows = simulate(frames, args.noise_scale)

    print(f"{len(frames)} frames, "
          f"{sum(len(v) for v in frames.values())} register writes\n")
    print(f"{'frame':>6} {'P1 Hz':>7} {'vol':>4} {'P2 Hz':>7} {'vol':>4} "
          f"{'TRI Hz':>7} {'NOISE Hz':>9} {'vol':>4}")
    for r in rows[:args.frames]:
        print(f"{r['frame']:>6} {r['p1'][0]:>7} {r['p1'][1]:>4} "
              f"{r['p2'][0]:>7} {r['p2'][1]:>4} {r['tri'][0]:>7} "
              f"{r['noise'][0]:>9} {r['noise'][1]:>4}")

    # Anything clipped is silently wrong on the DS: the frequency argument is
    # a u16, and apu_nds.c mutes rather than aliasing when it overflows.
    print()
    for tag, key in (("pulse 1", "p1_clipped"), ("pulse 2", "p2_clipped"),
                     ("triangle", "tri_clipped"), ("noise", "noise_clipped")):
        n = sum(1 for r in rows if r.get(key))
        if n:
            pct = n * 100 // len(rows)
            print(f"WARNING: {tag} out of range on {n} frames ({pct}%) "
                  f"- those frames are silent on the DS")

    nmax = max(r["noise_param"] for r in rows)
    print(f"\nnoise freq argument peaks at {nmax} "
          f"(u16 limit 65535, NOISE_SCALE={args.noise_scale})")
    if nmax > 65535:
        print("  -> overflowing; try a smaller NOISE_SCALE")
    elif nmax < 4000:
        print("  -> suspiciously low; percussion will sound too deep, "
              "try --noise-scale 8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
