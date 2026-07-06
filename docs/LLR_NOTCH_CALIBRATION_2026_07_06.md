# IMPLEMENTATION BRIEF — Per-carrier LLR notch calibration (16QAM R2/3 @ MPG@20 parked-notch CW failures)

Branch context: `wip/live-ladder-unvalidated`. All line numbers verified against the working tree 2026-07-06 (I re-read `channel_equalizer_equalize.cpp:440-599` and `soft_demap.hpp:1-102`; they match the reports). `soft_demap.hpp` is included only by `src/ofdm/channel_equalizer_equalize.cpp` and `src/ofdm/ofdm_symbol_demap.cpp` — MC-DPSK (`src/psk/`) is structurally untouched by everything below.

---

## 1. THE DEFECT, precisely

The hypothesis "LLRs are not scaled by |H_k|²/σ²" is **false as stated**: the demapper's per-bit noise variance IS per-carrier — `carrier_noise_var[i] = (σ² + h_err)/(|H_est|² + σ²)` (MMSE, `channel_equalizer_equalize.cpp:549-563`), consumed at `ofdm_symbol_demap.cpp:335-336`, and the whole downstream chain (carrier-LDPC deinterleave → burst deinterleave → frame/channel deinterleave → LDPC) is a permutation-pure float pipeline with **no input normalization** (`ldpc_decoder.cpp:215-218, 351-354`) — the consumers are exonerated. The defect is four coupled failures in how that per-carrier reliability is *formed and gated*, and they map one-to-one onto the two measured F142/F165 signatures:

**D1 — H_est over-reads through a parked notch → nv collapses → confident-wrong |LLR| 8-14.** Data-carrier H comes from the 2×1D Wiener (pilot revisit per carrier every 8 symbols ≈ 107 ms at sp8), whose frequency sinc prior over ≤16 obs plus `noise_norm` diagonal loading (`wiener_interpolator.hpp:181` — a near-zero pilot observation has huge relative noise → down-weighted toward neighbors) **smooths the notch shallow**. The code admits this: *"the Wiener may smooth THROUGH deep nulls so h_power looks normal"* (`equalize.cpp:99-103`). Over-read |H_est|² → small nv → scale 2/nv large → equalized noise demaps at |LLR| up to the 20 cap (the 8-14 values are genuinely computed, not clip artifacts — cap is 20, `demodulator_constants.hpp:24`). Every defense is keyed to the same wrong H_est, so all are blind. DD tracking can't correct it: gated off at `last_fading_index ≥ 0.15` (`channel_equalizer_pilot.cpp:913-926`), and MPG fading reads ~0.34.

**D2 — every null defense 16QAM actually gets references the ABSOLUTE global σ², blind to relative notches at 20 dB mean SNR.** 16QAM's only inflation is `softGrayZoneNoiseInflation = (γ+0.5)/γ`, γ = |H_est|²/**global** σ², cap 12× (`equalize.cpp:125-139`); a carrier 15-20 dB below the frame mean still has γ ≫ 0.5 → inflation ≈ 1.2-1.5 → no protection. Hard erasure requires γ < −6 dB absolute (`equalize.cpp:580-584`, `RX_ERASURE_GAMMA_FLOOR_LINEAR=0.25`) — never trips at 20 dB mean. The in-code comment states the exact mechanism: *"a carrier 27 dB below the frame mean still has |H|² >> global σ², so it reads clean and emits a confident-WRONG LLR that poisons the LDPC"* (`equalize.cpp:84-92`). The two mechanisms built for exactly this are **modulation-gated off for 16QAM**: `relativeFadeNoiseInflation` (QPSK/QAM8 only, `equalize.cpp:483-484`) and eps_H (`!soft_gray_zone_csi` gate, `equalize.cpp:558`). The within-frame |eq|-EMA instability inflation (`ofdm_symbol_demap.cpp:299-346`) is also blind: a 0.1 Hz notch parked for seconds is *stable* within one frame → norm_var ≈ 0.

**D3 — MMSE bias vs. an unbiased ring threshold (16QAM-structural, fires even with ACCURATE H_est).** The MMSE output is shrunk by β = |H|²/(|H|²+σ²); `demapQAM16` compares the shrunken |I| against the unbiased threshold 0.6325 = 2/√10 (`soft_demap.hpp:95,99`, `demodulator_constants.hpp:104`) with no bias compensation → on any low-γ carrier, outer constellation points systematically read as inner → **deterministic wrong-sign ring-bit LLRs**. QPSK does not share this (its sign-bit LLR ∝ eq/nv is invariant to the shrinkage). Flagged in `fable_analysis/02` §4.4, never fixed.

**D4 — the global σ² itself is one LTS-pair scalar per frame, never updated on data symbols** (`channel_equalizer_lts.cpp:689-712`, guard at `channel_equalizer_pilot.cpp:1145-1181`), measured to under-read the true post-eq residual **4-14× on Good@20** (in-code, `equalize.cpp:621-633`) — everything overconfident — while channel motion between the two LTS symbols biases it HIGH → the near-zero |LLR| 0.1-0.2 flood frames.

Signature mapping: D1 (+D3) = "confident-but-wrong |LLR| 8-14 through notches"; D4 + accurate-H MMSE shrinkage = "|LLR|_avg 0.1-0.2, near_zero 50-77%".

**Why the "deep nulls are already erased" NULL_DIAG verdict does not refute this:** ULTRA_NULL_DIAG bins carriers by **estimated** |H|²/frame-mean (`equalize.cpp:586-597`). A D1 carrier — true notch, over-read estimate — lands in the *norm* bin and is invisible to the instrument by construction. The 2026-06-14 "≤1.21× exhausted" arithmetic (`CHANGELOG.md:3225-3244`) therefore bounds only the correctly-estimated fade bin on Good@20, not the mis-estimated parked-notch population, and multiplicative-nv analysis cannot see the affine threshold bias D3 at all.

---

## 2. THE FIX

Two stages, both modulation-adaptive by construction (no `if (mod==X)` anywhere; the formulas are functions of |H|², σ², and constellation geometry already in scope). Stage A is pure math with zero new thresholds; Stage B adds one principled term. Both RX-only, wire-format byte-identical.

### Stage A — unbias the decision statistic (fixes D3, self-erases correctly-estimated notches)

**Formula.** Convert the (eq, nv) pair handed to the demapper from MMSE-biased to the unbiased (ZF-consistent) form. With P = |H_est|², σ²_eff = noise_variance (+ h_err term), β = P/(P+σ²_eff):

```
today:  eq  = conj(H)·Y/(P+σ²)          nv  = σ²_eff/(P+σ²)
fix:    eq' = eq/β = conj(H)·Y/P        nv' = nv/β = σ²_eff/P
```

Properties (verifiable algebra):
- **QPSK/BPSK sign bits bit-identical** (LLR ∝ eq/nv, both scale by 1/β) — clean-channel QPSK scale untouched by construction.
- **16QAM ring bit becomes correct**: LLR = (2/nv′)(|I′| − 0.6325) now compares the *unbiased* amplitude against the threshold, and its scale 2/nv′ = 2P/σ² **→ 0 as the notch deepens** — a notched carrier's ring bits go to near-erasure automatically, monotone in the true per-carrier γ, with no gate, no cap, no mod branch. Same benefit flows to QAM32/64 (their level grids share the assumption) and 8PSK max-log distances (currently computed on the shrunken symbol).
- The confident-wrong-through-accurate-notch mechanism (D3) is eliminated for every amplitude-bearing modulation at once.

**Insertion point.** `src/ofdm/channel_equalizer_equalize.cpp` per-carrier loop, both branches: pilot path lines **549-563** and adaptive/LMS path lines **522-524**. Concretely: replace the `/(mmse_denom)` denominators with `/h_power` for both `equalized[i]` and `carrier_noise_var[i]` numerator-over-P form (equivalently multiply both by `mmse_denom/h_power` after computing them). Keep the MMSE tap if preferred for the DD/EVM consumers inside `equalize()` (lines 653-745 use `equalized[]` for chi-sq gates) — if so, compute the unbias at the very end of the loop, after line 584, so DD gating sees the same statistics as today.

**Mandatory numerical guard (do not skip):** the existing clamp `carrier_noise_var[i] ≤ MAX_CARRIER_NOISE_VAR = 100` (`demodulator_constants.hpp:132`, applied at :563/:572/:578) **breaks the eq′/nv′ pairing** on very deep absolute notches: eq′ ∝ 1/P grows unbounded while nv′ saturates at 100 → you would *create* confident-wrong LLRs. When nv′ would exceed the clamp (i.e. P < σ²_eff/100, absolute γ < −20 dB), set `carrier_erasure_flags_[i]=1`, `equalized[i]=0`, `nv=MAX` — the existing erasure semantics (`ofdm_symbol_demap.cpp:368-371` emits exact-0.0 LLRs). This subsumes and is strictly more principled than the current −6 dB absolute gate at :580-584.

**Gate stack in Stage A: leave softGrayZone/relativeFade/eps_H untouched** (they multiply nv, still monotone). Land Stage A behind `ULTRA_ZF_LLR_UNBIAS` (default off) for the A/B, flip default after validation.

### Stage B — per-carrier reliability floor from RAW pilot observations (fixes D1, the mis-estimated notch)

The one signal that sees through Wiener smoothing is the **raw direct LS pilot observation** of each carrier — available every `spacing`=8 symbols (~107 ms ≪ the seconds-long notch park time; scattered pattern `pilot_pattern.hpp:78-98`) and already collected into the Wiener history at `channel_equalizer_pilot.cpp:672-681`. Measured fact: the Wiener `error_var` does NOT flag notches (flat ~0.003 across depth bins, `CHANGELOG.md:3234-3236`), so eps_H can never be the vehicle — the raw obs power can.

**Formula.** Maintain per carrier the most recent direct-observation power `O_k = |H_LS,k|²` (age ≤ spacing symbols). Use a reliability power that cannot exceed what was directly observed plus its own noise allowance:

```
P_rel,k = min( |H_est,k|² , O_k + σ² )        // E[O_k] = P_true + σ²_pilot
nv_k    = σ²_eff / P_rel,k                    // Stage-A form; erasure guard as above
```

Equalizer tap stays on the smoothed H_est (phase quality); only the *reliability* uses P_rel. On AWGN/flat channels O_k ≈ P_est → `min` is a no-op → **no clean-channel regression by construction** (same argument that justified eps_H default-on). It fires exactly and only when the smoothed estimate over-reads a directly-observed notch — the D1 population. Modulation-independent, channel-adaptive (the observation cadence is the pilot geometry, not a tuned constant).

**Insertion points:** persist `O_k` + age in `demodulator_impl.hpp` (next to `per_carrier_h_error_var_`, :74), write it where pilot LS obs are pushed (`channel_equalizer_pilot.cpp:672-681`), consume it in the nv computation at `equalize.cpp:549-563`. Env-gate `ULTRA_NOTCH_NV` (default off) until A/B'd.

**End-state (after both validated):** the gate patchwork — softGrayZone (:568-573), relativeFade (:574-579), the −6 dB absolute erasure (:580-584) — exists to compensate for exactly D2/D3; Stage A+B replace their function with one calibrated model. Retire them in a *separate, measured* step (this is the "unified per-carrier σ²_eff" that `fable_analysis/02:142-166` recommends and `MODEM_INFRASTRUCTURE_MAP.md:286` carries as the open ADAPT Phase-2b item). Do not silently stack Stage B on relativeFade for QPSK — A/B both ways; the double-count lessons (§3) all came from blind stacking.

**Explicitly out of scope (Stage C, later):** recalibrating the global σ² (the 4-14× under-read, D4). The frame-global multiplicative form was net-negative in 2026-05-26; revisit only with the genie-sigma oracle after A+B land.

---

## 3. WHAT NOT TO REDO

| Prior attempt | Why it failed | Why this differs |
|---|---|---|
| **"LLR-calibration exhausted" panel, 2026-06-14** (`CHANGELOG.md:3225-3244`; blanket "LLR re-weighting" dead-end `CHANGELOG.md:1721-1723`) | Fade-bin headroom ≤1.21×; deep bins already at ~195× nv | Scope-limited verdict: measured on 16QAM/Good@20, binned by **estimated** |H|² → structurally blind to D1 (mis-estimated notches land in the *norm* bin), and multiplicative-nv arithmetic can't see the affine ring-threshold bias D3. Never re-derived for MPG parked notches. |
| **Frame-global multiplicative σ² calibration** (2026-05-26, 4→12 CWfail, reverted; `FADING_RELIABILITY_CAMPAIGN_2026_05_26.md:23`) | One scalar for all carriers | Stage A/B are per-carrier and structural (bias + observation floor), not a scalar re-scale. `fable_analysis/02:89-98` explicitly warns not to let this revert kill per-carrier work. |
| **ULTRA_LLR_NOISE_EMP_FLOOR** single-symbol empirical floor (net-negative 1490 vs 1740 bps; `equalize.cpp:621-649`) | Confidently-wrong decision lands NEAR a constellation point → small residual → floor fails exactly on the poison carriers | Stage B keys on the raw *pilot* observation (known TX symbol), immune to the near-a-point failure mode. Do not re-run the single-symbol form. |
| **eps_H-for-QAM16** (regressed good@24 2720→2020 bps, CW-fails 135→512; `equalize.cpp:553-557`; dead-end list) | Double-counts with softGrayZone; and error_var is flat ~0.003 (pilot geometry, not a null-targeter) | Stage B does not use Wiener error_var at all; Stage A is not an inflation (it fixes the decision statistic). Neither stacks a new multiplier onto softGrayZone. |
| **ULTRA_REL_FADE_QAM16** (built, A/B'd, REVERTED −20/−21%, CW-fails 8→32; `CHANGELOG.md:3346-3354`) | Frame-mean gate keyed to *smoothed* H_est fired on healthy carriers and starved the R2/3 LDPC | Stage B keys to the raw direct observation — it can only fire where the notch was actually *seen*; no frame-mean reference, no crude onset constant. |
| **k=2.0 over-inflation lesson** (`CHANGELOG.md:3320-3354`) | Over-erasure starves R2/3 (~15-18% correction budget) | Stage A's near-erasure is *exact* (the true LLR for the true γ), not a tuned inflation; Stage B's floor is bounded by a real observation. Still: watch total erasure fraction per CW in the A/B. |
| **Fading-index hard-gated softGrayZone variant** (rejected 2026-05-22, `SOFT_CSI_LLR_2026_05_22.md:64-65`) | Hard channel-class gate lost seed43 | No channel-class gates anywhere in A/B. |

---

## 4. VALIDATION PLAN

**Step 0 — mechanism confirmation (before writing the fix), oracle split on the failing cell:**
```bash
# genie channel: if Stage-A's target is right, true-H + unbiased demap should kill the ring-bit poison
ULTRA_GENIE_DATA_AIDED=1 ./build/measure_ack_fer --snr 20 --channel good --mod qam16 --rate r2_3 \
  --config data4_full --frame-cw 1 --seed 42 --n 200          # frame-cw 1 REQUIRED (FIFO alignment)
# genie sigma: bounds what nv-calibration alone can win
ULTRA_QAM16_GENIE_SIGMA_EMPIRICAL=1 ./build/measure_ack_fer --snr 20 --channel good --mod qam16 \
  --rate r2_3 --config data4_full --seed 42 --n 200
```
Genie-H pass-rate minus baseline = D1's share; genie-sigma delta = calibration's share. Record both.

**Step 1 — measure_ack_fer A/B (Stage A, then A+B), paired seeds, both configs:**
```bash
for s in 42 43 44; do
  # baseline vs ULTRA_ZF_LLR_UNBIAS=1 (then + ULTRA_NOTCH_NV=1), identical seeds:
  ./build/measure_ack_fer --snr 20 --channel good --mod qam16 --rate r2_3 --config data4_full  --seed $s --n 200
  ./build/measure_ack_fer --snr 20 --channel good --mod qam16 --rate r2_3 --config burst_chunk --burst-interleave 1 --seed $s --n 40
  # QPSK control (expect NEUTRAL — sign-bit LLRs invariant under Stage A):
  ./build/measure_ack_fer --snr 20 --channel good --mod qpsk  --rate r2_3 --config data4_full  --seed $s --n 200
  # AWGN clean-channel guards (β≈0.99 at 20 dB → near-no-op expected; Stage B min() inactive):
  ./build/measure_ack_fer --snr 20 --channel awgn --mod qam16 --rate r2_3 --config data4_full  --seed $s --n 200
  ./build/measure_ack_fer --snr 14 --channel awgn --mod qpsk  --rate r1_2 --config data4_full  --seed $s --n 200   # R1/2 floor locator guard
done   # always | tee /tmp/notchllr_*.log
```
Expected direction: qam16/good@20 `decode_fail` **down** (burst_chunk is the config that reproduces the production parked-notch frame train); qpsk and both AWGN rows within seed noise. Instrument with `ULTRA_FAILURE_ATTRIBUTION=1` — `llr_to_empirical_sigma2` (`ofdm_stream_processor.cpp:137-174`) must move toward 1.0 on surviving hard frames, and `carrier_metrics` absH-vs-EVM must show notch carriers now at low |LLR|. `ULTRA_NULL_DIAG=1` for the depth-bin decomposition (with the estimated-|H|² binning caveat in mind).

**Step 2 — faithful gate, paired multi-seed** (gate noise is ±25%; direction + CW-fail counts, never single-run absolutes):
```bash
for s in 42 43 44; do
  tools/gui_qso_scenario.sh --channel good --snr-db 20 --seed $s --file-kb 21 \
    --out /tmp/notchllr_gate_s$s 2>&1 | tee /tmp/notchllr_gate_s$s.log &   # run in background
done
```
Compare `summary.env` (`RESULT`, `FILE_CRC_OK_COUNT`, `GOODPUT_BPS`) and grepped `CW[n]: FAIL` counts against same-seed baseline runs. Plus `ctest --test-dir build --output-on-failure -j4` green throughout.

**Step 3 — rig MPG@20** (the mission cell; sim can't fully reproduce the parked-notch statistics): standard IONOS recipe, 16QAM R2/3 forced, ≥3 transfers each way, compare CW-fail rate and goodput vs baseline build.

---

## 5. RISKS

1. **LLR-shape false-lock gates sit exactly on the new signature** (`src/sync/signal_policy.hpp:13-27, 83-102`, `ULTRA_LLR_REJECT_SHAPE` default-ON since 2026-07-05: reject if near_zero_fraction > 0.50, or mean_abs < 1.5 AND near_zero > 0.30; `kMinLLRForEscalation=1.5` at `streaming_ofdm_decode.cpp:1645-1674`, `kMinLLRForSingleCWDecode` at :699). Converting notch bits to near-erasure **raises near_zero_fraction** — a genuine frame with a wide notch can cross 0.50 and be eaten *before* LDPC, converting a decode win into a sync loss. The F165 expected-anchor immunity covers armed data frames, but light-sync continuations and CW0-peeks are exposed. Mitigation: log `llr_near_zero` distributions in the A/B; if real frames approach the boundary, exclude erasure-flagged carriers from the near_zero denominator (they are *declared*, not suspicious).
2. **Clamp-pairing hazard (self-inflicted D3)**: the `MAX_CARRIER_NOISE_VAR=100` clamp desynchronizes eq′ from nv′ at deep absolute notches — the erasure guard in §2 Stage A is mandatory, not optional.
3. **Tuned-constant stack re-calibration**: softGrayZone (cap 12), CE margins (QAM16 1.2, `demodulator_constants.hpp:119`), CARRIER_ADAPTIVE_K=10, and relativeFade (cap 30) were all tuned against today's nv scale on low-γ carriers; Stage A changes nv exactly there. Watch for over-inflation stacking (the k=2.0 lesson) — this is why Stage A ships with gates *unchanged* and the unification is a separate measured step.
4. **LDPC scale-assuming internals**: perturbation-retry sigmas {0.3..2.0} and the ±6.0 hard-decision magnitude (`frame_v2.cpp:2209-2233`, `ldpc_decoder.cpp:305`) assume the canonical scale. Stage A preserves sign-bit scale (the dominant population), so exposure is low, but a marginal-frame retry-ladder shift is possible — the AWGN guard rows cover it.
5. **HARQ soft-combine consistency**: combining is a raw LLR sum; within one build both transmissions use the same scale — fine. Never A/B by mixing builds mid-session.
6. **AWGN 16QAM near entry floors**: β at 10-14 dB per-carrier γ is 0.90-0.96 → ring-threshold shift up to ~10% — could move the R1/2 AWGN 14 dB locator either way; guarded in Step 1.
7. **OFDM_NARROW shares the equalizer** — gains and risks apply there too; its w=3 ARQ masks small FER shifts, but the ~17.6 dB floor should be spot-checked once before merge.
8. **MC-DPSK untouched by construction** (verified: `soft_demap.hpp` included only by `channel_equalizer_equalize.cpp` and `ofdm_symbol_demap.cpp`; no `src/psk/` consumer). Keep it that way — no changes outside `src/ofdm/` + the two env knobs.
9. **DD/EVM consumers of `equalized[]`** inside `equalize()` (chi-sq radius, decision-cell, ln(9) odds gates, :653-745) assume the MMSE-scaled symbol; if Stage A unbiases in place rather than at loop end, those gates' radii are silently re-scaled — insert the unbias *after* line 584 as specified.

Key files: `/Users/mathieuvachon/Projects/ProjectUltra/src/ofdm/channel_equalizer_equalize.cpp` (primary insertion, lines 522-524, 549-584), `/Users/mathieuvachon/Projects/ProjectUltra/src/ofdm/soft_demap.hpp` (no change needed if the (eq′, nv′) pair is formed upstream), `/Users/mathieuvachon/Projects/ProjectUltra/src/ofdm/channel_equalizer_pilot.cpp:672-681` (Stage B raw-obs capture), `/Users/mathieuvachon/Projects/ProjectUltra/src/ofdm/demodulator_impl.hpp` (Stage B state), `/Users/mathieuvachon/Projects/ProjectUltra/src/sync/signal_policy.hpp` (risk 1), `/Users/mathieuvachon/Projects/ProjectUltra/tools/measure_ack_fer.cpp` (harness).