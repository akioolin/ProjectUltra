# Subsystem Adaptivity Audit (living document)

Opened: 2026-05-29. Owner: ongoing.

## Why this document exists

This project is ~6 months of AI iteration. A recurring failure pattern emerged:
**a subsystem is built and tuned for ONE modulation (or ONE channel / ONE SNR),
then other modes are bolted on with `if (mod == X)` special-cases** — instead of
being derived from first principles (constellation geometry, per-carrier SNR,
coherence time/bandwidth) so it is correct for the whole *family* by construction.

These narrow-view subsystems pass their original cell and fail silently elsewhere.
The decision-directed (DD) channel tracker is the canonical case and the reason
this audit exists (see below). The operator's directive:

> "each subsystem must work with the global picture in view ... is it possible to
> make this adaptive so it helps with any modulation / code rate whatever ... we
> must audit all systems so we are sure they are adaptive, to avoid this damn
> failure we got with DD."

This is a **standing systemic audit**, run under the four-perspective stack
(PHY theorist + DSP engineer + veteran HF operator + first-principles) and the
project's proof discipline (whole-matrix, multi-seed, no single-cell "fixed").

## The smell-tests (how to spot a narrow-view subsystem)

1. **Naming tell** — a symbol/constant named for one mode but used by others
   (`dd_qam16_*`, `kQam16DecisionChiSq95`, `*_8psk` carved into a shared path).
2. **`if (mod == QAM16) … else …`** with no general formula behind the branch —
   the branch *is* the design instead of a parameterization.
3. **Single-reference gate** — a decision that checks only one quantity (distance
   to its own hard decision) with no independent cross-check. (DD's exact flaw:
   every gate measured distance to the decision, none compared to the pilots.)
4. **Threshold that "works" at one SNR/channel** — a magic constant calibrated on
   one cell (e.g. Good@15) with no derivation from the channel state it faces.
5. **Constellation-blind margins** — a fixed decision/EVM radius that ignores the
   actual `d_min` of the modulation in use (QPSK √2 vs 8PSK 0.765 vs 16QAM 0.632).

A subsystem is **adaptive** when each of its constants is either (a) genuinely
geometry/probability-independent (e.g. a noise chi-sq radius `−ln(0.05)`, a
posterior odds `ln(9)`), or (b) computed from the modulation/channel parameters
in scope. Mixed bags (most thresholds general, one mode-specific magic value)
are the dangerous middle — they look principled and aren't.

## Canonical case: decision-directed (DD) channel tracking — BUG-8PSK-001

- **Symptom:** forced 8PSK R3/4 delivers perfectly on clean AWGN (2330 bps) but
  fails on Good@20 fading — confident-WRONG bits, LDPC parity fails, bimodal per
  group (6/6 or 0/6). `ULTRA_COHERENT_DD_OFF=1` flips FAIL→PASS.
- **Narrow-view roots:** subsystem named `dd_qam16_*`; built for 16QAM, 8PSK
  bolted on with a couple of `if (mod==QAM8)` cell-guard/margin special-cases.
- **Deeper root (the real, modulation-general flaw):** *every* DD gate measures
  distance to the hard decision (EVM-to-decision, decision-cell, posterior-LLR).
  A symbol that fading rotated cleanly onto a **neighbor** constellation point is
  confident-*wrong* — low EVM to the wrong point, high LLR for the wrong point —
  and passes all gates. Its observation `H_obs = Y/d̂` is rotated by the
  inter-point angle (22.5° for 8PSK) and fed into H with high weight → cascade.
  Tighter constellations (8PSK, 16QAM) trip this far more than QPSK (45° sectors).
- **The missing principle (general, not a QAM8 patch):** the **pilots** are the
  only on-air reference that *cannot* be confident-wrong (known symbols). DD must
  be **innovation-gated against the pilot-anchored estimate** — reject/down-weight
  any DD observation whose disagreement with the pilot anchor exceeds what
  measurement noise + coherence-bandwidth drift can explain. This catches the
  confident-wrong rotation, is modulation-adaptive by construction (tight
  constellations get a tighter implied budget vs their wrong-decision rotation),
  and lets DD still refine where it is genuinely safe (QPSK, clean channels).
- **Status:** fix in progress (innovation-gated, modulation-general DD). Full
  diagnosis in `docs/8PSK_GOOD_FADING_DIAGNOSIS_2026_05_29.md`; bug entry
  BUG-8PSK-001 in `docs/KNOWN_BUGS.md`.

## Case #2: MMSE / Wiener channel estimator — narrow-view found (2026-05-29)

Audited as the "estimator lever" for fading survivability. Findings:

- **On?** Yes — the shared channel-estimation core for all coherent mods
  (QPSK, 8PSK, 16QAM) when scattered pilots are active.
- **Math correct?** Yes, textbook MMSE: normal equations `R·w = r` (Gaussian
  elimination w/ pivoting in `wiener_interpolator.hpp`), Jakes/Clarke time
  correlation `J0(2π·fd·t)`, uniform-PDP frequency correlation `sinc(π·df·τ)`,
  correct MMSE residual variance. Not broken.
- **Adaptive?** **NO** — the correlation model is hardcoded to **Moderate-HF**
  (`robustDelaySpreadS()` default 1.0 ms, `robustDopplerHz()` default 0.5 Hz,
  `channel_equalizer_pilot.cpp:28-41`) and applied unchanged on every channel.
  On Good (0.5 ms / 0.1 Hz) this is 2× too pessimistic in frequency and 5× too
  pessimistic in time — the code comment itself says it "was throwing away older
  pilot observations 5× too aggressively on Good." So it under-weights pilots
  that are still correlated → a noisier estimate than it could produce. Currently
  only overridable via `ULTRA_WIENER_DELAY_SPREAD_S` / `ULTRA_WIENER_DOPPLER_HZ`
  test knobs — not derived at runtime.
- **Optimal?** No — mis-tuned (above), plus it is *separable 1D* (time then
  frequency) rather than joint 2D. Structure right, parameterization wrong.
- **Fix direction:** derive `delay_spread` and `Doppler` from the **measured**
  channel (fading index is already computed; delay spread is estimable from the
  CIR, Doppler from temporal variation) and feed those into the correlation
  model. Because it's the shared estimator, this should lift QPSK/8PSK/16QAM
  together — part of maximizing fading survivability (see note below).
- **Status:** quantifying the contribution offline (Moderate-default vs
  Good-tuned params, `measure_ack_fer` qam8/qpsk Good@20) before committing.

**Sequencing note — survivability before airtime:** maximizing fading
survivability is the prerequisite, not a parallel track, because the two are
coupled: a frame that doesn't survive a fade is retransmitted, and a resend
costs a full frame + turnaround of airtime. So improving survivability
(adaptive estimation + frequency diversity + erasure-aware LLR) *also* reclaims
the resend airtime, and it unlocks running a higher modulation rung reliably —
which is where the throughput toward 3000 actually comes from. Pure-protocol
airtime waste (handshake, ACK gaps, dead air) is a separate lever attacked
after, with proper measurement tooling. This matches the project's documented
"reliability first, then throughput" methodology.

## Audit register (subsystems to sweep)

Status: ⬜ not started · 🔶 in progress · ✅ confirmed adaptive · ⚠️ narrow-view found

| Subsystem | File(s) | Status | Notes |
|---|---|---|---|
| Decision-directed channel tracking | `channel_equalizer_pilot.cpp`, `channel_equalizer_equalize.cpp` | 🔶 | Canonical case; innovation-gating fix in progress. `dd_qam16_*` naming to follow once behaviour is proven general. |
| Soft demapper / LLR scaling | `soft_demap.hpp`, `channel_equalizer_equalize.cpp` | ⬜ | Check σ² source per mode; LLR scaling `1+10·fading²` calibrated on which mod/channel? |
| Rate / modulation picker | `waveform_selection.hpp`, `connection_policy.hpp` | ⬜ | Known scar tissue (4 gate-arrays × 3 passes); QAM8 was unhandled → AWGN fallthrough. Channel-adaptive promotion is the matching workstream. 2026-07-03: the connect-SNR input side is now adaptive-by-derivation (`ConnectSnrPool`, #58 inc 3, knob-gated): decorrelation clustering + wire freshness both keyed to Tc(f_D)/fading class via `retxTroughDopplerHz`→`coherenceTimeMsForDoppler` — zero tuned ms constants; the only fixed numbers are the −10 dB wire sentinel (= encodeSNR floor/byte 0) and 3·Tc (Clarke decorrelation multiple). |
| Light-sync / warm-sync gates | `streaming_signal_policy.hpp`, `streaming_sync_acquisition.cpp` | ⬜ | Thresholds (0.50/0.90 coherent) tuned per channel? WARM vs COLD position-gating is the adaptive form. |
| Interleaver depth (burst group frames) | `streaming_burst_interleave.cpp` | ⬜ | Depth vs coherence time is channel-specific (FREQ for Good, TIME/burst for Mod/Poor). Currently env-knob `ULTRA_BURST_GROUP_FRAMES`. |
| LDPC codeword length | `fec/`, `ULTRA_LDPC_Z` | ⬜ | Length-vs-interleave coupling; N currently fixed/knob-driven, not channel-derived. |
| ARQ / ACK RTO timers | `protocol/connection.cpp` | ⬜ | Airtime-derived RTO (good); confirm it scales with mode/rate, not magic clamp. |
| MMSE/Wiener channel estimator | `wiener_interpolator.hpp`, `channel_equalizer_pilot.cpp` | ⚠️ | **Narrow-view found** — math is textbook-correct but the correlation model is hardcoded to Moderate-HF (delay 1 ms / Doppler 0.5 Hz) and run on every channel; on Good (0.5 ms / 0.1 Hz) that under-uses correlated pilots (5× too aggressive in time). Fix = derive delay/Doppler from the measured channel. Shared by QPSK/8PSK/16QAM. See Case #2 below. |
| AGC / carrier-sense thresholds | `modem_engine.cpp`, GUI | ⬜ | Energy-detection floor (~+6 dB); OFDM-gated. Band/sample selection per mode? |

## How this ties to the rest of the plan

- The **runtime-config-derivation refactor** (see memory
  `project_env_knobs_to_runtime_derivation_refactor`) is the *production form* of
  this audit: the bucket-3 env knobs (LDPC_Z, group frames, pilot spacing, Wiener
  params, rate/mod) are the discovery scaffolding for the channel→best-config
  mapping; once mapped, they collapse into one code-derived adaptive PHY layer.
- The **adaptive-PHY design** (`docs/PHY_ADAPTATION_DESIGN_2026_05_26.md`) is the
  per-traffic-class / per-channel parameterization this audit feeds.
- Sequencing: discover/validate each subsystem's adaptive form via the GUI
  harness, then codify. Do not codify before the mapping is proven.

## Method (per the project rules)

- Pair every code-pattern check with **measurement** (a narrow-view subsystem
  often passes code review — DD did). See `feedback_audits_need_measurement...`.
- Every generalization passes the **whole-matrix** gate: each modulation × code
  rate × channel × SNR, multi-seed, no regression on the modes that worked.
- Quick mode-specific patches are tolerated only as **labeled prototypes**; the
  principled, family-general form must replace them before merge.
