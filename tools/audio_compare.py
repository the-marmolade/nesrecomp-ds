#!/usr/bin/env python3
"""
audio_compare.py - measure how the DS port's audio differs from a reference.

Guessing at mix levels by ear is unreliable and slow. Both builds play the
same deterministic music, so record each to WAV and compare them numerically.

The headline number is SPECTRAL CENTROID: the energy-weighted mean frequency,
in Hz. It is the objective version of "sounds too high". If the port's
centroid sits above the reference's, the mix is too bright - typically
missing or quiet bass, which on the NES means the triangle channel or the
DMC thumps.

Band energies then say WHERE the difference is, so you can tell a weak
triangle (low band) from over-loud noise (high band).

Recording the two sides:

  Reference   Mesen -> Tools -> Record to WAV, or any NES emulator with
              audio capture. This is your ground truth.
  DS port     melonDS has no WAV export; use Audacity with Windows WASAPI
              loopback, or OBS, and export mono WAV.

Play the same passage in both - the title screen is easiest, since it starts
identically every time and needs no input.

Usage:
    python3 tools/audio_compare.py reference.wav port.wav
"""
import argparse
import struct
import sys
import wave


def read_wav(path):
    """Return (samples as floats in -1..1, sample rate). Mixes down to mono."""
    with wave.open(path, "rb") as w:
        n_ch = w.getnchannels()
        width = w.getsampwidth()
        rate = w.getframerate()
        raw = w.readframes(w.getnframes())

    if width != 2:
        raise SystemExit(f"{path}: need 16-bit WAV, got {width * 8}-bit")

    vals = struct.unpack("<%dh" % (len(raw) // 2), raw)
    if n_ch > 1:
        vals = [sum(vals[i:i + n_ch]) / n_ch for i in range(0, len(vals), n_ch)]
    return [v / 32768.0 for v in vals], rate


def dft_magnitudes(block, rate, win=4096):
    """Averaged magnitude spectrum over Hann-windowed frames.

    An earlier version decimated the signal to keep this dependency-free,
    which aliased everything above a few hundred Hz into the bass band and
    reported two very different signals as identical. Use a real FFT."""
    import numpy as np

    x = np.asarray(block, dtype=np.float64)
    if len(x) < win:
        x = np.pad(x, (0, win - len(x)))

    hann = np.hanning(win)
    acc = np.zeros(win // 2)
    frames = 0
    for start in range(0, len(x) - win + 1, win // 2):
        spec = np.abs(np.fft.rfft(x[start:start + win] * hann)[:win // 2])
        acc += spec
        frames += 1
    if frames:
        acc /= frames

    freqs = np.fft.rfftfreq(win, 1.0 / rate)[:win // 2]
    return list(freqs), list(acc)


def analyse(samples, rate, label):
    # One second from a quarter of the way in: past any intro silence, and
    # long enough to cover several notes.
    start = len(samples) // 4
    block = samples[start:start + rate]
    if len(block) < rate // 2:
        block = samples[:rate]

    rms = (sum(s * s for s in block) / max(1, len(block))) ** 0.5

    freqs, mags = dft_magnitudes(block, rate)
    half = len(freqs) // 2
    freqs, mags = freqs[:half], mags[:half]

    total = sum(mags) or 1e-9
    centroid = sum(f * m for f, m in zip(freqs, mags)) / total

    bands = {"bass 0-250Hz": 0.0, "low-mid 250-1k": 0.0,
             "mid 1k-3k": 0.0, "high 3k+": 0.0}
    for f, m in zip(freqs, mags):
        if f < 250:
            bands["bass 0-250Hz"] += m
        elif f < 1000:
            bands["low-mid 250-1k"] += m
        elif f < 3000:
            bands["mid 1k-3k"] += m
        else:
            bands["high 3k+"] += m

    print(f"--- {label}")
    print(f"  duration          {len(samples) / rate:.1f}s @ {rate}Hz")
    print(f"  RMS level         {rms:.4f}")
    print(f"  spectral centroid {centroid:.0f} Hz")
    for k, v in bands.items():
        print(f"  {k:<18} {v / total * 100:5.1f}%")
    print()
    return {"rms": rms, "centroid": centroid,
            "bands": {k: v / total for k, v in bands.items()}}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference", help="WAV from a reference NES emulator")
    ap.add_argument("port", help="WAV captured from the DS port")
    args = ap.parse_args()

    ref_s, ref_r = read_wav(args.reference)
    port_s, port_r = read_wav(args.port)

    ref = analyse(ref_s, ref_r, "reference (NES)")
    port = analyse(port_s, port_r, "DS port")

    print("=== difference ===")
    ratio = port["centroid"] / (ref["centroid"] or 1e-9)
    print(f"centroid ratio {ratio:.2f}x", end="  ")
    if ratio > 1.15:
        print("- port is BRIGHTER; likely weak bass (triangle / DMC)")
    elif ratio < 0.87:
        print("- port is DARKER; likely weak treble (pulses / noise)")
    else:
        print("- tonal balance is close")

    print()
    for band in ref["bands"]:
        r, p = ref["bands"][band] * 100, port["bands"][band] * 100
        d = p - r
        flag = ""
        if abs(d) > 5:
            flag = "  <-- adjust this channel's level"
        print(f"  {band:<18} ref {r:5.1f}%   port {p:5.1f}%   "
              f"diff {d:+5.1f}%{flag}")

    print("""
Mapping bands back to channels:
  bass 0-250Hz     triangle (SMB's bass line), DMC thumps
  low-mid 250-1k   pulse 1 and 2 carrying the melody
  mid 1k-3k        upper pulse harmonics, higher melody notes
  high 3k+         noise (percussion), square-wave harmonics

Adjust the soundSetVolume scaling in apu_nds.c for whichever band is off,
then re-record and re-run. Two or three iterations should converge - which
beats guessing, and gives you a number to point at when it is right.""")
    return 0


if __name__ == "__main__":
    sys.exit(main())
