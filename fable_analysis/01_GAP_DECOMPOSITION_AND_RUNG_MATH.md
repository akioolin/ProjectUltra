# 01 — Gap decomposition & rung math (Good@20, target 3086 bps)

**Author:** Claude Fable 5, 2026-06-12. Audit method: 11-agent parallel code survey +
8-agent adversarial verification + live GUI-gate experiments (see `07_VERIFICATION_RUNS.md`).
All numbers below are derived from code with file:line citations, not from docs (several
project docs carry stale numerology — see `08_STALE_DOCS_AND_BUGS_REGISTER.md`).

## 1. The benchmark comparison is VALID (and slightly conservative)

Before optimizing, we verified we are chasing a real gap, not an accounting artifact:

- **Channel parity:** the OTASim GOOD profile is an *exact* ITU-R F.1487 / CCIR 549-2
  "Good" implementation — 2 independent equal-power Rayleigh paths (0.707/0.707),
  0.5 ms differential delay, 0.1 Hz Doppler spread with the correct 2σ convention
  (σ = 0.05 Hz), Gaussian spectrum via 128 stratified sum-of-sinusoids oscillators,
  fading on the analytic signal (`src/ota_channel_core/models.cpp:846-856, 470, 561-611`;
  asserted by `tests/test_watterson_proof.cpp` PARTS A–L). No divergence found.
- **SNR convention:** our `--snr-db 20` references the RX FIR's equivalent noise
  bandwidth of **2606.09 Hz, not 3000 Hz** (`models.cpp:97-103`, `models.hpp:25-26`,
  `docs/CALIBRATION_AUDIT.md:289`). A strict 3 kHz S/N meter would call our 20 dB
  **19.39 dB** — i.e. our gate runs **0.61 dB HARDER** than the leader's published row.
  (For exact published-comparison parity, run the gate at `--snr-db 20.61`.)
- **Impairment set:** the served fading channels hard-zero CFO (`models.cpp:960-967`)
  and the two stations share the server timebase (no ppm offset) — equivalent to
  bench practice for published modem tables. Note this means the burst path's
  fixed-stride slicing has **never been exercised with real clock skew** (flagged in 03).
- **Goodput accounting:** `GOODPUT_BPS` = app file bytes / wall-clock from CONNECTED to
  last-chunk-ACK (`src/gui/app.cpp:1629,1045-1052`, `tools/gui_qso_scenario.sh:288-297`).
  It *includes* ladder ramp, MODE_CHANGEs, connect guard, and final-ACK tail that the
  leader's long-file bytes/min amortizes to ~0. At 21 KB this understates steady state
  by **~15-20%** (bracketed 10-30%). Corrected steady state today: **~1750-2250 bps**.
  Still a real 1.4-1.8× deficit. **The accounting is not the gap.**

## 2. Air-true numerology (single source of truth for all rate math)

Production wideband config is `presets::balanced()` (`modem_engine.cpp:42`): FFT=1024
@48 kHz (46.875 Hz bins), 59 carriers (~2766 Hz occupied), CP=MEDIUM = 48×(1024/512) =
**96 samples = 2.0 ms** (`types.hpp:327-343`) → symbol = **1120 samples = 23.333 ms**,
**42.857 sym/s**. CP overhead 8.57%.

⚠ Three divergent symbol-length models coexist (see 08): the air truth above; the
protocol layer's 1152-sample/24 ms model (`connection_policy.hpp:19-21`,
`waveform_selection.hpp:68`) used for ALL budget/timeout math (−2.9% bias); and a dead
1280-sample fallback (`ofdm_chirp_waveform.cpp:1078-1083`). TX/RX agree on 1120, so
links work; the budget model under-fills key-downs.

Pilots (`include/ultra/ofdm_link_adaptation.hpp:26-94`): scattered, rotating 1
carrier/symbol. Spacing by (mod, rate): **R3/4 → 8** (8 pilots / 51 data),
**R2/3, R1/2, R1/4 → 5** (12 pilots / 47 data), **R5/6 → falls into `default` → 5**
(this is a bug — see 05). LDPC N=648 fixed; K = 162/324/432/486/540 for
R1/4–R5/6 (`src/fec/ldpc_codec.cpp:38-42`).

## 3. Raw PHY rung table (info bps = data_carriers × bits × rate × 42.857)

| Rung | QPSK (2b) | 8PSK/QAM8 (3b) | 16QAM (4b) |
|---|---:|---:|---:|
| R1/2 (sp5, 47 data) | 2014 | 3021 | 4029 |
| R2/3 (sp5, 47 data) | 2686 | 4029 | 5371 |
| **R3/4 (sp8, 51 data)** | **3279** | **4918** | **6557** |
| R5/6 (sp5 as coded) | 3357 | 5036 | 6714 |
| R5/6 (sp8 if fixed) | 3643 | 5464 | 7286 |

The `docs/COMPETITIVE_BENCHMARK_TARGET.md:69-76` raw table (3438 @R3/4, 5157 D8PSK) is
**stale** — it reverse-engineers exactly to the retired DQPSK-era geometry (1152-sample
symbols + legacy 4/6-pilot layouts). Do not plan against it.

## 4. The steady-state cycle (QPSK R3/4, zero retx, from code constants)

Every burst cycle (file:line in `04_AIRTIME_EFFICIENCY_LEDGER.md`):

```
150 ms lead-in + 1410 ms BURST_HEADER descriptor (1200 ms dual chirp + ~210 ms control)
+ 5 × 1236.7 ms data frames (53 sym: 2 LTS + 51 data) + 50 ms tail
+ ~150 ms RX decode/queue + 875 ms tone-ACK key-down (675 ms ACK @25 ms/sym + lead/tail)
+ ~150 ms detect/refill  ≈  8.97 s per cycle
payload = 5 × 456 file bytes = 18 240 bits  →  ~2 030 bps zero-retx ceiling
```

**The measured best (1910 bps) is 94-96% of this ceiling.** The top end of current
performance is the *protocol structure*, not decode quality; the 1290 floor is ladder
ramp + retx. Protocol efficiency = 2030/3279 = **62%**.

## 5. Why QPSK is mathematically excluded

- Best QPSK rung as coded: R5/6 = 3357 raw → needs **92% airtime efficiency** for 3086.
  Even with ZERO protocol overhead, QPSK R3/4 caps at 3279×(456/480) ≈ **3115**, and
  R5/6's 17% redundancy was already measured broken against Good's ~23% instantaneous
  fade erasure (`docs/RATE_LADDER_ANCHORS.md:89-91`). Half-duplex turnaround + preamble
  + ACK make >90% efficiency physically impossible.
- **Conclusion (hard):** matching the leader requires a denser-modulation rung.
  This was known qualitatively; the table above makes it exact.

## 6. Route arithmetic to ≥3086 (zero-retx ceilings, current code constants)

| Route | Today's overheads | + descriptor short-anchor & fast-ACK levers | Requirement |
|---|---:|---:|---|
| QPSK R3/4 (status quo) | ~2 030 | ~2 500 | — capped, excluded |
| **8PSK R3/4** | ~2 800 | **~3 430** | damage must drop to ≤~10% (measured ~31%, 07) |
| **16QAM R3/4** | **~3 270** | **~4 170** | damage must drop (measured ~30%, 07) |
| 16QAM R2/3 | ~2 800 | ~3 540 | the FEC-margin sweet spot candidate |

Both viable routes run through the **same receiver fix** — the LLR/estimation
calibration described in `02_LLR_CALIBRATION_THE_MISSED_FIX.md` is the common enabler:
it is the leading explanation for BOTH 16QAM's structural failure AND 8PSK's 2×
survivability gap (confident-wrong LLRs near frequency-selective nulls).

Channel headroom confirmed: genie bit-load ceiling ≈ **3764 bps** at Good@20; 85% of
carriers are 16QAM-capable (`docs/16QAM_DECODABILITY_DIAGNOSIS_2026_05_29.md:12-29`).
After decodability falls, per-carrier bit-loading (QPSK/8PSK/16QAM mixed) is the
polish toward ~3764 — not before (established dead end).

## 7. Consistency check against measured anchors

| Anchor (GUI gate) | Measured | This model says |
|---|---|---|
| Forced QPSK R3/4 Good@20 5-seed avg (06-02) | 1630 | ceiling 2030 minus retx/ramp ✓ |
| Adaptive ladder Good@20 5-seed (06-11) | 1290-1910 | 94-96% of ceiling at top ✓ |
| Forced 8PSK R3/4 AWGN30 (05-29) | 2330 | old stack; ceiling now ~2 800 ✓ direction |
| Forced 16QAM Good@20 (05-29) | FAIL, 256 CW fails | the wall (02) ✓ |

The model reconciles. The gap is fully explained by (rung ceiling) × (37% overhead) ×
(retx/ramp), with no residual mystery factor.
