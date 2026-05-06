# Modem Improvement Backlog

Forward-looking engineering work that would make ProjectUltra's HF
data link more robust and (in some cases) faster on real channels.
Items are roughly ranked by ROI — cheapest, highest-impact at the
top. Each item is independent: do them in any order based on what
becomes the actual bottleneck in OTA validation.

This is a backlog, not a roadmap. Don't start any of these until
real-radio testing has shown them to be the actual problem.

---

## 1. Soft-combine memory-ARQ audit and hardening

**The idea.** When a frame fails to decode, store its soft LLRs.
When the peer retransmits, combine the new LLRs with the stored
ones before re-running LDPC decode. Two failed receptions of a
noisy frame can decode together when neither would alone.

**Why it matters on HF.** HF errors are bursty and fade-correlated.
A frame's bad symbols on attempt 1 may be different from attempt 2.
Independent failures combine into one success without dropping to
a slower mode.

**Current state.** `src/fec/soft_combine.{cpp,hpp}` exists. The
streaming decoder uses it. We need to verify it's actually doing
weighted LLR accumulation (`L_combined = L_old + L_new`), not
averaging or other lossy combine. Also verify scaling, sign
consistency, and that the buffer is keyed on (CW position, code
rate, modulation, interleaver pattern) so retx with mismatched
parameters don't corrupt the buffer.

**Effort.** 2-5 days for the audit + targeted fixes + tests.

**Risk.** Bad combining can hurt: stale or sign-inverted LLRs make
LDPC decoding *worse*. Guard with explicit invalidation on any PHY
parameter change.

**Estimated gain.** 2-3 dB equivalent SNR in fading; 30-50% fewer
retransmissions on marginal channels.

---

## 2. Narrowband-interference notch filter

**The idea.** Detect strong narrowband tones in the receive band
(broadcast carriers, heterodynes, jammers) and apply an adaptive
notch filter before sync detection and channel equalization. A
single carrier or two can poison sync correlation, AGC tracking,
and pilot estimates across the entire OFDM frame.

**Why it matters on HF.** Real HF rarely matches AWGN or Watterson
simulations. Crowded bands have strong signals from neighboring
modes (FT8, PSK31), broadcast skip, lightning crashes, and
co-channel QRM. Removing one strong tone can rescue an otherwise-
unrecoverable burst.

**Current state.** No interference suppression in the RX path.

**Effort.** 1-2 weeks. Standard adaptive notch (LMS or
autocorrelation-based detection) → IIR notch filter chain with
~6 zeros. Already-known DSP technique, well-documented.

**Risk.** A notch removes wanted energy too. In OFDM, a notch
that overlaps a data carrier costs that carrier's bits. Need
careful detection thresholds and notch placement.

**Estimated gain.** Variable — zero on clean band, but the
difference between "decode" and "fail" on a band with one strong
neighbor.

---

## 3. Low-rate robust fallback waveform

**The idea.** A very-low-rate, very-robust mode (well below R1/4
DQPSK) the link can drop to when conditions degrade further.
Spreading factor ≥ 8 on DBPSK, or a chirp-based payload mode.
Carries control + small data, keeps the ARQ session alive while
the channel recovers.

**Why it matters on HF.** Conditions change. A connection that's
fine at 14:00 UTC may be marginal at 14:30 as the band shifts.
Without a fallback, the session times out and operators have to
re-establish — losing minutes of airtime per fade.

**Current state.** MC-DPSK is our lowest mode at 5+ dB SNR. Below
that, sessions die.

**Effort.** 2-4 weeks. We have most of the pieces (DBPSK encoder,
chirp sync). Need spreading code + matched-filter receiver +
mode-negotiation glue.

**Risk.** Mode switching adds protocol complexity. False-acquisition
testing needed to make sure the fallback doesn't trigger on noise.

**Estimated gain.** Session stays up at 0-5 dB SNR where it would
otherwise fail. Throughput in fallback mode is low (~50-200 bps)
but >>0.

---

## 4. Per-burst mode adaptation with finer granularity

**The idea.** Currently auto-rate picks one waveform/code-rate at
session start (with a few mid-session upgrades). Tighten the loop:
re-evaluate every burst based on the previous burst's decode
quality (LLR confidence + retransmission count). More speed
levels, tighter hysteresis.

**Why it matters on HF.** Channel conditions change on
sub-second timescales (Doppler spread, multipath). The fastest
mode that works *now* changes faster than session-level
adaptation can keep up with.

**Current state.** Adaptive code-rate works. Modulation is
mostly chosen at handshake. Re-evaluation cadence is coarse.

**Effort.** 1-3 weeks for a better policy layer + measurement
+ hysteresis tuning. Existing modulations and rates, no new PHY.

**Risk.** Poor thresholds can flap modes ("R1/2 → R1/4 → R1/2 →
…"), each transition costing a frame of overhead. Both peers
must agree deterministically.

**Estimated gain.** 10-20% on rapidly-fading channels.

---

## 5. Per-subcarrier bit loading (OFDM-native)

**The idea.** OFDM puts the same modulation on all 59 carriers.
On a frequency-selective HF channel, some carriers have +10 dB
SNR vs the worst. Assigning higher-order constellations to clean
carriers and lower-order (or bit-zero / disabled) to faded ones
extracts more capacity. Levin-Campello bit-allocation is the
classic algorithm; bit-zero ("carrier mask") is a useful
simplified first step.

**Why it matters on HF.** Multipath fading creates spectral
notches. Today the entire frame's modulation is bottlenecked by
the worst carrier the equalizer can still handle.

**Current state.** Uniform modulation across all data carriers.

**Architectural constraint surfaced 2026-05-05 (do not retry these
approaches without addressing the constraint):**

Three approaches were attempted and all failed hardware validation
on Watterson Good SNR=15. See `docs/SESSION_2026-05-05_NIGHT.md`
and tag `experimental/per-carrier-attempt-1-failed-2026-05-05` for
full evidence.

1. **RX-only LLR rescaling** (multiply LLRs by `[0.1..1.0]` based
   on `|H_k| / median(|H|)`). Failed in 2 rounds: double-counts
   fade penalty (the soft demap already weights by `|H_k|² / σ²_k`),
   and median-of-|H| is not a physically meaningful threshold.
   Caused 1/5 → 4/5 catastrophic regressions across rounds.
2. **TX-aware closed-loop carrier mask via MODE_CHANGE wire field.**
   Failed because MODE_CHANGE round-trip (5–15 s for the 28-byte
   2-CW frame) is **slower than the channel coherence time on
   Watterson Good** (~10 s with ±5 dB per-carrier γ_k swings on
   sub-second timescales). The mask flaps every 5–15 s; during
   each transition TX and RX disagree on which carriers are
   active, bit positions misalign, LDPC fails, ARQ pile-up, rate
   adapter panics. 3/5 seeds catastrophic, no seed beat baseline.

The viable architecture is **frame-local mask derivation, no
wire signaling**: both peers derive identical masks from each
frame's own LTS preamble using a deterministic rule. Requires:
- Reciprocity assumption between TX and RX channels (must be
  measured first on the actual hardware harness — half-duplex HF
  is generally close but not exactly reciprocal at the per-carrier
  level).
- Bit-exact identical mask-derivation rule on both sides.
- LDPC-side handling of per-frame-variable puncturing pattern (not
  a code-rate change; the codeword length stays fixed, but
  punctured bit positions vary per frame).

This is non-trivial. Probably 2–3 weeks of careful design and
testing, not a single Codex round.

**Effort.** 3-6 weeks (frame-local approach). Reciprocity
measurement (half-duplex calibration), per-carrier SNR estimation
(we already have most of this from the channel equalizer),
deterministic mask-derivation rule, modulator/demodulator wiring,
LDPC-side per-frame puncturing awareness, and tests.

**Risk.** Reciprocity violations cause TX and RX to derive
different masks → silent decode failures (worse than current,
because there's no feedback channel to detect the mismatch).
Mitigation: include a mask CRC or count in the next ACK so a
mismatch is at least observable.

**Estimated gain.** 30-50% on fading channels with strong
frequency-selective notches. Smaller on flat fading or AWGN.

---

## 6. Periodic full-band training symbols within a frame

**The idea.** Insert known training symbols every N data symbols
inside long frames. Receiver uses them to refresh channel
estimate, equalizer taps, AGC, and timing. Currently we rely on
LTS at frame start + sparse pilots throughout.

**Why it matters on HF.** Doppler shifts and slow fading drift
the channel during a frame. Initial training is stale by the
end of a long packet, especially at high QAM.

**Current state.** LTS at frame start. Sparse pilot tones (4-6
of 59 carriers) for tracking.

**Effort.** 2-4 weeks. Insert one full-band training symbol every
~100 ms of payload, signal it deterministically, update receiver
to use it.

**Risk.** Training symbols cost airtime. If too frequent, throughput
drops on stable channels. Need profile per use case (fast Doppler
vs stable NVIS).

**Estimated gain.** Enables higher-QAM modes on Doppler/multipath
channels that currently force fallback to DQPSK.

---

## 7. Rate-compatible LDPC for incremental-redundancy HARQ

**The idea.** Replace chase combining with incremental redundancy:
on retransmission, send *additional parity bits* from a punctured
mother code rather than the same coded bits. Receiver soft-combines
original + new parity into a stronger code. Mathematically more
efficient than chase combining.

**Why it matters on HF.** Near-threshold SNR, chase combining
eventually decodes but wastes airtime sending duplicate bits.
IR adds new information each retransmission.

**Current state.** Chase combining only. IEEE 802.11n LDPC codes
are fixed instances, not designed for puncturing.

**Effort.** 4-8+ weeks. Mother-code design or rate-compatible LDPC
family, puncturing pattern, soft-buffer changes, decoder support.

**Risk.** High complexity. Bad puncturing patterns destroy
LDPC performance. Should come AFTER #1 (chase combining audit) is
verified, because that's cheaper and lower risk.

**Estimated gain.** 1-2 dB equivalent SNR over good chase
combining; meaningful retransmission savings near threshold.

---

## 8. Pre-sync AGC / level normalization for compressed-audio inputs

**The idea.** Add an explicit RMS-based audio level normalizer at the
top of the decode pipeline (before chirp correlation, before LTS
search, before LDPC LLR generation). Either a fast peak-tracking
gain control or an RMS-window normalizer that keeps signal RMS in a
known range regardless of how aggressively the upstream radio /
SDR / web tool has compressed the audio.

**Why it matters on HF.** Today our pilot-based LLR scaling
compensates for level variations *after* sync is established. The
chirp correlation itself has no AGC compensation — it assumes the
audio level is roughly what the encoder produced. Most real radios
in data mode have a benign IF AGC and we're fine. But:

- WebSDR audio AGC (KiwiSDR, OpenWebRX) flattens dynamic range by
  10-20 dB and squashes our chirp into the noise floor. Verified
  2026-05-03 OTA test: friend transmitted, signal was clearly
  visible in waterfall (~+15 dB SNR), but the AGC-on web record
  produced audio with only +2-3 dB SNR — chirp correlation 0.20-
  0.43 instead of the 0.94 we got with AGC off.
- Casual operators with mis-configured radio AGC (long time
  constants, hang AGC, "noise reduction" features) can produce
  similar artifacts even without an SDR in the loop.
- Even modern radios in data mode often default to AGC=SLOW which
  pumps on burst signals.

**Current state.** No pre-sync level control. Chirp correlation
threshold is fixed at 0.45-0.52. Signals that survive everything
*except* aggressive AGC fail at sync, never reaching the parts of
the decoder that actually have level adaptation.

**Approach.**
- Track audio RMS over a 200-500 ms sliding window.
- Apply a slow gain to bring RMS to a reference (e.g. 0.15 RMS).
- Cap the per-step gain change so impulsive noise can't drive the
  AGC into oscillation.
- Run BEFORE chirp correlation. The waveform's downstream pilot
  tracking still handles fine-grained level changes.

**Effort.** 1-2 days for the AGC, the threshold tuning, and a
regression fixture set generated with sox `compand` to simulate
known AGC compression ratios.

**Risk.** A hot AGC could amplify noise enough to create false
chirp matches. Threshold + min-SNR gate at the chirp output should
mitigate. Validate on the existing OTA fixture (the AGC-on
recording from 2026-05-03 is now committed test material).

**Estimated gain.** Decoders WebSDR-recorded audio without manual
AGC-off configuration. Improves robustness with real radios whose
operators don't perfectly tune AGC for data. No expected gain in
already-clean conditions.

**Validation.** The 2026-05-03 OTA recording is the canonical test
case: same audio, AGC-on version should now decode like AGC-off.

---

## Out of scope (intentionally)

- **New waveform architecture (single-carrier with adaptive
  equalizer + RAKE):** would be 6-12+ weeks of major rework on
  top of an already-working OFDM stack. Defer indefinitely
  unless OTA testing shows OFDM is fundamentally inadequate for
  our target channels.
- **More text compression (Huffman/PMC) at modem layer:** Pat's
  B2F already does gzip on application payloads. Adding another
  compression layer below it gives near-zero benefit on
  already-compressed B2F messages and adds latency.
- **VARA / Pactor wire-protocol compatibility:** would require
  reverse-engineering closed protocols. Stay open and let our
  TCP API compatibility (validated tonight) carry interop.

---

## How to use this list

When real-radio testing surfaces a specific failure mode:
- "Sessions die at low SNR" → look at #3 (fallback waveform)
- "Decoding fails near band-edge with adjacent QRM" → #2 (notch)
- "Throughput poor on disturbed-path runs" → #1 (chase audit) then #5 (bit loading) or #6 (training)
- "Mode oscillates" → #4 (per-burst adaptation tuning)
- "Sync fails on WebSDR / aggressive-AGC inputs even though waterfall shows clean signal" → #8 (pre-sync AGC)

Don't start anything from this list speculatively. Each item has
real implementation cost and will displace other work. Validate
the underlying need on real hardware first.
