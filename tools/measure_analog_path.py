#!/usr/bin/env python3
"""Measure the ANALOG path between two stations (sender DAC -> channel -> receiver ADC).

WHY THIS EXISTS. On 2026-07-26 a 5.3 dB discrepancy was attributed to "real hardware
loss" purely by subtraction: the OFDM broadband SNR estimator reads 20.1 dB through the
simulated ITU-Good channel at dial 20, but only ~14.7 dB on the rig at the same dial. That
attribution was WRONG, and it was wrong because nobody had ever measured the analog path
directly -- it was inferred. This tool measures it, so the question is settled by data.

METHOD. A 16-tone multitone at irregular bin spacing across ~300-2900 Hz:
  - per-tone amplitude          -> frequency response / BAND TILT
  - energy in the empty bins    -> NOISE + DISTORTION (IMD) floor
  - tone/(non-tone) power ratio -> the analog path's SNDR CEILING
Irregular spacing is essential: with arithmetic spacing, intermodulation products land on
top of other tones and hide. Here they fall into bins we can read.

The measured SNDR is the COMBINATION of the channel simulator's synthesised noise and the
analog path's own noise, so run the simulator at its highest S:N (e.g. WGN S:N=40) and this
script de-embeds it: 1/SNDR_analog = 1/SNDR_measured - 1/SNR_simulator.

Run the simulator in a NON-FADING mode (WGN). Fading modulates the tones and makes tilt
indistinguishable from fades.

USAGE
  # 1) generate the probe and copy it to the sending station
  ./tools/measure_analog_path.py generate --peak 0.38 --out /tmp/probe_multitone.f32
  #    (peak 0.38 put the IONOS at ~650-790 mV p-p GREEN; scale from your own sine
  #     calibration so the probe does NOT clip -- clipping would confound tilt/distortion)
  # 2) loop it on the sender, e.g. on a Pi:
  #    while true; do aplay -D plughw:2,0 -f FLOAT_LE -r 48000 -c 1 probe_multitone.f32; done
  # 3) capture on the receiver:
  #    sox -q -d -t f32 -r 48000 -c 1 rx_capture.f32 trim 0 12
  # 4) analyse
  ./tools/measure_analog_path.py analyze --rx /tmp/rx_capture.f32 --sim-snr-db 40

RESULT ON THE IONOS BENCH, 2026-07-26 (Pi5 Fe-Pi/sgtl5000 -> IONOS WGN@40 -> Mac):
  band tilt 486-2859 Hz : 0.7 dB peak-to-peak   (NOT the 14.8 dB an older note claimed)
  measured SNDR         : 30.2 dB
  analog SNDR ceiling   : 30.7 dB  (after de-embedding the simulator's 40 dB)
  cost at dial 20       : 0.36 dB  <- the analog path is CLEAN; it cannot explain 5.3 dB
Capture RMS was 0.0924 vs 0.082 during real transfers, so this was measured at the
operational level rather than an artificial one.
"""
import argparse
import math
import sys

import numpy as np

SR = 48000
NFFT = 8192
BIN = SR / NFFT
LO_HZ, HI_HZ = 300.0, 2900.0
N_TONES = 16
SEED = 12345  # fixed so `generate` is reproducible and `analyze` can re-derive the bins


def tone_bins():
    """The probe's tone bins. Deterministic: analyze() re-derives them without a side file."""
    rng = np.random.default_rng(SEED)
    lo, hi = int(LO_HZ / BIN), int(HI_HZ / BIN)
    bins = []
    while len(bins) < N_TONES:
        b = int(rng.integers(lo, hi))
        if all(abs(b - x) >= 20 for x in bins):
            bins.append(b)
    return sorted(bins), rng


def generate(path, peak, seconds):
    bins, rng = tone_bins()
    n = int(SR * seconds)
    t = np.arange(n) / SR
    x = np.zeros(n)
    for b, p in zip(bins, rng.uniform(0, 2 * np.pi, len(bins))):
        x += np.cos(2 * np.pi * (b * BIN) * t + p)
    x /= np.max(np.abs(x))
    x = (x * peak).astype(np.float32)
    x.tofile(path)
    rms = float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))
    print(f"wrote {path}: {seconds:.0f}s, {len(bins)} tones, peak {peak:.3f}, "
          f"RMS {rms:.4f}, crest {20 * math.log10(peak / rms):.2f} dB")
    print("tone freqs (Hz): " + " ".join(f"{b * BIN:.0f}" for b in bins))
    print("NOTE: scale --peak from your own single-tone level calibration so the probe")
    print("      stays inside the simulator's linear range (no clipping).")


def analyze(rx_path, sim_snr_db, skip_s, use_s):
    bins, _ = tone_bins()
    rx = np.fromfile(rx_path, dtype=np.float32).astype(np.float64)
    if rx.size == 0:
        sys.exit(f"{rx_path}: empty")
    peak, rms = float(np.max(np.abs(rx))), float(np.sqrt(np.mean(rx ** 2)))
    print(f"capture {rx.size / SR:.1f}s  peak {peak:.4f}  RMS {rms:.5f}")
    if peak < 1e-4:
        sys.exit("SILENT capture -- wrong input device, or no signal reaching the receiver")

    seg = rx[int(SR * skip_s):int(SR * (skip_s + use_s))]
    nseg = len(seg) // NFFT
    if nseg < 4:
        sys.exit("capture too short for a stable average; record >= 12 s")
    w = np.hanning(NFFT)
    acc = np.zeros(NFFT // 2 + 1)
    for i in range(nseg):
        acc += np.abs(np.fft.rfft(seg[i * NFFT:(i + 1) * NFFT] * w)) ** 2
    acc /= nseg

    tone_set = {int(b) for b in bins}
    # +/-2 bins around each tone are excluded from the floor: window leakage lives there
    # and would otherwise be counted as distortion.
    guard = {b + d for b in tone_set for d in range(-2, 3)}
    inband = list(range(int(LO_HZ / BIN), int(HI_HZ / BIN)))
    clean = [k for k in inband if k not in guard]
    tone_p = float(sum(acc[k] for k in tone_set))
    # Scale the clean-bin floor up to the full in-band width so the ratio is a true
    # in-band SNDR rather than a per-bin figure.
    noise_p = float(sum(acc[k] for k in clean)) * (len(inband) / max(len(clean), 1))
    sndr = 10 * math.log10(tone_p / noise_p)

    ref = max(acc[int(b)] for b in bins)
    resp = [(b * BIN, 10 * math.log10(acc[int(b)] / ref)) for b in bins]
    print("\nper-tone response (dB re strongest) -> BAND TILT")
    for f, d in resp:
        print(f"  {f:7.0f} Hz  {d:+6.2f} dB")
    tilt = max(d for _, d in resp) - min(d for _, d in resp)
    print(f"\nBAND TILT {LO_HZ:.0f}-{HI_HZ:.0f} Hz : {tilt:.1f} dB peak-to-peak")
    print(f"measured in-band SNDR       : {sndr:.1f} dB")

    if sim_snr_db is not None:
        # De-embed the simulator's synthesised noise to isolate the analog path.
        inv = 10 ** (-sndr / 10) - 10 ** (-sim_snr_db / 10)
        if inv <= 0:
            print(f"measured SNDR >= simulator S:N ({sim_snr_db:.0f} dB) -- analog path is "
                  "below the noise the simulator itself injects; raise S:N and re-run")
            return
        analog = -10 * math.log10(inv)
        print(f"analog path SNDR ceiling    : {analog:.1f} dB  "
              f"(de-embedded from simulator S:N {sim_snr_db:.0f} dB)")
        print("\ncost of that ceiling at each dial setting:")
        for dial in (10, 14, 20, 24):
            tot = -10 * math.log10(10 ** (-dial / 10) + 10 ** (-analog / 10))
            print(f"  dial {dial:>2} dB -> delivered {tot:5.2f} dB   loss {dial - tot:.2f} dB")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate", help="write the multitone probe")
    g.add_argument("--out", default="/tmp/probe_multitone.f32")
    g.add_argument("--peak", type=float, default=0.38)
    g.add_argument("--seconds", type=float, default=20.0)
    a = sub.add_parser("analyze", help="analyse a capture of the probe")
    a.add_argument("--rx", required=True)
    a.add_argument("--sim-snr-db", type=float, default=None,
                   help="the channel simulator's S:N setting, to de-embed its noise")
    a.add_argument("--skip-s", type=float, default=2.0)
    a.add_argument("--use-s", type=float, default=8.0)
    args = ap.parse_args()
    if args.cmd == "generate":
        generate(args.out, args.peak, args.seconds)
    else:
        analyze(args.rx, args.sim_snr_db, args.skip_s, args.use_s)


if __name__ == "__main__":
    main()
