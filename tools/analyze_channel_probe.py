#!/usr/bin/env python3
"""Independent oracle: does the dumped channel deformation match textbook ITU-R
'Good' fading? Built ONLY from the spec (fd=0.1 Hz Rayleigh, 0.5 ms 2-path comb),
knows nothing about our C++. Reads channel_probe's _in/_out f32, recovers H(f,t)=
Y/X per tone, and checks per-carrier Rayleigh AFD/LCR, Doppler spread, comb spacing,
and — the point — how deep/long the in-band nulls a 12s burst actually sees are.

Usage: analyze_channel_probe.py /tmp/chanprobe_good   (expects _in.f32 _out.f32)
"""
import sys, numpy as np

prefix = sys.argv[1] if len(sys.argv) > 1 else "/tmp/chanprobe_good"
fd = float(sys.argv[2]) if len(sys.argv) > 2 else 0.1      # spec Doppler spread (Hz)
delay_ms = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5  # spec delay (ms)
FS = 48000
NPERSEG, HOP = 4800, 2400          # 10 Hz bins (tones land exactly), 20 frames/s
x = np.fromfile(prefix + "_in.f32", dtype=np.float32).astype(np.float64)
y = np.fromfile(prefix + "_out.f32", dtype=np.float32).astype(np.float64)
n = min(len(x), len(y)); x, y = x[:n], y[:n]
dt = HOP / FS

# ---- manual STFT (Hann) ----
win = np.hanning(NPERSEG)
nfr = 1 + (n - NPERSEG) // HOP
def stft(s):
    out = np.empty((nfr, NPERSEG // 2 + 1), dtype=np.complex128)
    for i in range(nfr):
        seg = s[i*HOP : i*HOP + NPERSEG] * win
        out[i] = np.fft.rfft(seg)
    return out
X, Y = stft(x), stft(y)

tone_freqs = np.array([250.0 + 50.0*k for k in range(51)])
bins = np.round(tone_freqs * NPERSEG / FS).astype(int)
H = Y[:, bins] / X[:, bins]          # (frames, carriers) complex transfer fn
amp = np.abs(H)                      # |H(f,t)|

# ---- closed-form Rayleigh predictions (ρ relative to envelope RMS) ----
def afd(rho):  return (np.exp(rho**2) - 1.0) / (rho * fd * np.sqrt(2*np.pi))
def lcr(rho):  return np.sqrt(2*np.pi) * fd * rho * np.exp(-rho**2)
def below_frac(rho): return 1.0 - np.exp(-rho**2)   # Rayleigh CDF P(r<rho*rms)

print(f"== channel-probe oracle ==  frames={nfr} dt={dt*1000:.1f}ms span={nfr*dt:.1f}s "
      f"carriers={len(bins)}  (spec: fd={fd}Hz delay={delay_ms}ms)")

# normalize each carrier by its OWN envelope RMS so rho is comparable
rms = np.sqrt((amp**2).mean(axis=0, keepdims=True))
r = amp / rms                        # envelope normalized to RMS=1 per carrier

print("\n-- per-carrier deep-fade stats (pooled over %d carriers) vs Rayleigh@%.2fHz --" % (len(bins), fd))
print(f"{'level':>8} {'meas AFD':>10} {'pred AFD':>10} {'meas %below':>12} {'pred %below':>12}")
for db in (-3, -6, -10):
    rho = 10**(db/20)
    # average fade duration: mean length of contiguous runs r<rho, across carriers
    durs = []
    for c in range(r.shape[1]):
        m = r[:, c] < rho
        if not m.any(): continue
        d = np.diff(np.flatnonzero(np.diff(np.r_[0, m.view(np.int8), 0])))[::2]
        durs.extend(d)
    meas_afd = (np.mean(durs)*dt) if durs else 0.0
    meas_below = m_below = (r < rho).mean()
    print(f"{db:>6}dB {meas_afd:>9.2f}s {afd(rho):>9.2f}s {100*meas_below:>11.1f}% {100*below_frac(rho):>11.1f}%")

# ---- THE BURST QUESTION: longest CONTINUOUS in-band null within any 12s window ----
print("\n-- in-band null persistence (the 12s-burst question) --")
for db in (-6, -10):
    rho = 10**(db/20)
    maxrun = 0
    for c in range(r.shape[1]):
        m = (r[:, c] < rho).view(np.int8)
        # longest run of 1s
        best = cur = 0
        for v in m:
            cur = cur + 1 if v else 0
            best = max(best, cur)
        maxrun = max(maxrun, best)
    print(f"  worst continuous null @ {db}dB over whole run: {maxrun*dt:.2f}s "
          f"(Rayleigh avg fade dur = {afd(rho):.2f}s)")

# ---- Doppler spread: PSD of H(t) per carrier, pooled ----
Hc = (H - H.mean(axis=0, keepdims=True)) * np.hanning(nfr)[:, None]
Pf = (np.abs(np.fft.fft(Hc, axis=0))**2).mean(axis=1)   # complex -> full FFT
fr_full = np.fft.fftfreq(nfr, dt)
order = np.argsort(fr_full); fr_full, Pf = fr_full[order], Pf[order]
# fold to |f| (Doppler spread is two-sided about 0)
absf = np.abs(fr_full)
fr = np.unique(absf)
P = np.array([Pf[absf == f].sum() for f in fr]); P /= P.max()
half = fr[np.where(P >= 0.5)[0][-1]]
cum = np.cumsum(P)/P.sum()
f95 = fr[np.searchsorted(cum, 0.95)]
print(f"\n-- Doppler spectrum of H(t) --")
print(f"  measured half-power width ~{half:.3f}Hz, 95%-power width ~{f95:.3f}Hz "
      f"(spec spread {fd}Hz)")

# ---- comb spacing (delay) from a few instantaneous |H(f)| snapshots ----
print(f"\n-- frequency selectivity (comb spacing -> delay) --")
print(f"  spec: 2-path {delay_ms}ms -> nulls every {1000/delay_ms:.0f}Hz")
# carrier-to-carrier correlation of |H| in frequency -> coherence bandwidth proxy
amp_demean = amp - amp.mean(axis=1, keepdims=True)
fc = (amp_demean[:, :-1] * amp_demean[:, 1:]).mean() / (amp_demean**2).mean()
print(f"  adjacent-carrier(50Hz) |H| correlation = {fc:.3f} (─>1 = flat, low = selective)")
# instantaneous spread: median over time of (max-min)|H| across band, in dB
inst_db = 20*np.log10(amp.max(axis=1) / np.clip(amp.min(axis=1), 1e-9, None))
print(f"  instantaneous in-band |H| spread: median {np.median(inst_db):.1f}dB, "
      f"p90 {np.percentile(inst_db,90):.1f}dB, max {inst_db.max():.1f}dB")
