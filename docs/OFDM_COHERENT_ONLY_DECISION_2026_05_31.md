# OFDM goes coherent-only — retire differential-OFDM (decision-in-progress, 2026-05-31)

**Status:** DIRECTION DECIDED, deletion GATED on multi-seed proof. This is a Tier-0
architecture decision (removes a modulation path), so the *direction* is committed but the
*code removal* waits on the validation matrix in §6. Tracks into `REMOVAL_BACKLOG.md` once locked.

**One line:** the OFDM band (SNR ≥ 10) becomes **coherent-only** (QPSK → 8PSK → 16QAM).
Differential modulation (DQPSK/DBPSK) is **not removed from the project** — it is **relocated to
where it belongs**: MC-DPSK, the sub-10-SNR / Poor-channel mode whose entire design is differential.

---

## 1. The architecture (why differential-OFDM has no home)

Mode selection is by SNR + fading, not free choice (`connection_policy.hpp:selectLadderRung:249` —
`if (snr_db >= ofdm_floor) return OFDM_CHIRP; else MC-DPSK rungs`). So:

| Regime | Owner | Modulation | Why |
|--------|-------|------------|-----|
| **SNR < 10** (and Poor / heavy fading) | **MC-DPSK** | **differential** (DBPSK/DQPSK) | no phase reference, no pilots, no channel est — survives what the harsh channel destroys (`multi_carrier_dpsk.hpp:9` "differential, no pilots needed"; demod = phase-diff vs `prev_symbols_`, `:221`) |
| **SNR ≥ 10** (AWGN / Good / Moderate) | **OFDM** | **coherent** (QPSK→8PSK→16QAM) | pilots + equalizer track the phase; coherent keeps its ~2–3 dB detection edge |

Differential-OFDM (DQPSK-on-OFDM) tried to bring differential robustness *into* the OFDM band. But:
- **Below SNR 10** (where that robustness matters) → that's already MC-DPSK's job.
- **At/above SNR 10** (where OFDM runs) → coherent beats it, even at the very floor.

**Squeezed out from both sides → no regime → redundant.** Critically, the differential vs coherent
divergence on **fading is about Doppler/fading-RATE, not SNR level** — and the fast-fading regime
where differential wins is *below the OFDM floor* (MC-DPSK territory). On the slow/medium fading the
OFDM band actually owns, coherent's phase tracking keeps up (see §2).

---

## 2. Evidence (GUI faithful gate, forced rung below auto-entry where needed)

Metric that matters: **clean-group rate** (groups decoded 6/6 first-try) and **clean-group
`max_iters`** (low = big decode margin; climbing toward 50 = phase tracking losing the channel).

| Run | RESULT | clean rate | clean `max_iters` | goodput |
|-----|--------|-----------|-------------------|---------|
| Differential DQPSK R1/4 **AWGN@10** | PASS, CRC-OK, 0 retx | — | — | 420 bps (clean channel baseline) |
| Differential DQPSK R1/4 **Good@10** | (stopped mid-run) | **32%** (6/19) | — | — |
| **Coherent QPSK R1/4 Good@10** (fading 0.61) | **PASS**, CRC-OK | **81%** (17/21) | **1–4** | 190 bps |
| **Coherent QPSK R1/4 Moderate@14** (fading 0.71) | **PASS**, CRC-OK | **89%** (17/19) | **1–5** | 320 bps |

**Findings:**
- Coherent decodes groups first-try **~2.5× more often** than differential on the same Good@10.
- **`max_iters` stayed flat at 1–5 even as fading sped up (Good→Moderate)** — coherent's phase
  tracking did *not* start losing the channel. That is the decisive signal.
- All failures are **binary fade-erasure** (`max_iters=50`, `0/2 CWs` — a deep null wiped the frame),
  which hits every modulation equally. Differential does NOT escape it (its Good@10 spread was worse).
- Moderate@14 was actually *cleaner* than Good@10 — higher SNR lifts more fade-nulls above the
  erasure floor (confirms "better at higher SNR" applies to fading too).

The carrier-LDPC air-block fix (commit `9189b70`, `docs/BURST_Z_LDPC_LIFECYCLE_2026_05_31.md`) is what
made the z=81 differential path decode at all this session; with that fixed, the *coherent-vs-
differential* comparison became measurable — and coherent won.

---

## 3. What simplifies in the code (file:line, grounded)

Removing differential from the OFDM path collapses **one whole axis of variation** off the hot path —
the same axis that produced BUG-8PSK-001 *and* this session's carrier-LDPC bug.

1. **~41 differential branches in the OFDM demod/equalizer** (`ofdm_stream_processor.cpp` +
   `channel_equalizer_*`): the `is_differential` gating (`:342, :388`), the **magnitude-only `|H|`
   channel-estimate path** (the differential alternative to full mag+phase tracking), and the
   `differential_prev_erased_` state + its clear sites (`:379, :472, :844, :928, :1045`).
2. **Control-first peek profile dance** (`streaming_ofdm_decode.cpp:633-647`):
   `profileForDataMode(saved_mod, coherent_ofdm_control_profile_enabled_)` picks DQPSK control for
   differential links / coherent QPSK control for coherent links, then `configure()`-switches and
   restores. Coherent-only → one fixed control profile; the flag + `switched_profile` branch + the
   switch/restore drop out of the hot decode path.
3. **Carrier-LDPC dual behavior** (`ofdm_chirp_waveform.cpp`): the coherent-skips-inverse vs
   differential-runs-inverse split (the eligibility branch that *masked* this session's air-block
   miscount) becomes one path. **The entire bug class disappears.**
4. **Single LLR path** (phase-difference LLRs vs equalized-constellation LLRs → one) and the ladder's
   DQPSK-vs-QPSK decision for OFDM (`connection_policy.hpp:181`, `recommendDataMode`) goes away.

## 4. KEEP — do NOT over-cut (footgun list)

- **MC-DPSK keeps ALL its differential machinery** (`multi_carrier_dpsk.hpp`) — its design, untouched.
- **`Modulation` enum `DQPSK/DBPSK/D8PSK` stay** — MC-DPSK uses them.
- **The DD (decision-directed) tracker `dd_qam16_*` STAYS** (`channel_equalizer_equalize.cpp:636+`).
  It is the **COHERENT** 8PSK/16-QAM channel tracker (despite the "DD" name), NOT differential —
  coherent-only OFDM *relies* on it for the higher rungs. The "remove differential" framing must not
  tempt a cut here. (It carries the BUG-8PSK-001 history — a *coherent* bug, separate workstream.)

---

## 5. How it fits the other two workstreams (the roadmap)

Three interrelated threads are now in flight. They share seams; sequencing matters so one doesn't
undo another.

```
A. Coherent-only OFDM        (this doc)        — collapse the coherent/differential axis
B. SyncController refactor   (SYNC_ACQUISITION_FIX_PLAN §7) — consolidate sync + z-state + CFO
C. Ladder / mode-selection rework  (pending)   — entry floors, SNR<10→MC-DPSK, rate selection
```

**Shared seams (where they touch):**
- **Control-profile / frame-class→config:** A deletes the differential control profile; B's §7.7 #2
  consolidates "frame-class → config" into one derivation. **A makes B's hardest piece trivial** —
  with one OFDM modulation family, the config derivation stops forking on differential.
- **Carrier-LDPC:** lives in the soft-bit extraction path that B's §7.6 z-state work touches; A
  removes its coherent/differential fork. Do A first → B inherits a single-path carrier-LDPC.
- **Ladder rungs:** A flips the OFDM rungs DQPSK→QPSK (`connection_policy.hpp:181`); that IS part of C.

**Recommended order: A → C → B.**
1. **A first** (lowest risk, highest leverage): lock with §6 multi-seed, flip rungs to coherent,
   delete the differential-OFDM branches. Now the OFDM path has one modulation axis.
2. **C next**: the broader ladder/floor rework (entry floors, the SNR<10 MC-DPSK boundary, rate
   selection) on the simplified rungs.
3. **B last** (riskiest — refactoring working sync): the SyncController consolidation, built on the
   already-simplified base. B's frame-class→config, control profile, and carrier-LDPC are all cleaner
   because A ran first.

**Rationale:** B is the dangerous one (rewriting a working path). Doing A first shrinks B's surface
area and removes a known bug class before the refactor, instead of carrying the coherent/differential
fork *through* the refactor. Refactor against the simplest possible target.

Cross-links: `SYNC_ACQUISITION_FIX_PLAN_2026_05_31.md §7.7` (frame-class→config),
`BURST_Z_LDPC_LIFECYCLE_2026_05_31.md` (carrier-LDPC + z-state), `ADAPTIVITY_AUDIT_2026_05_29.md`
(BUG-8PSK-001 / the DD tracker), `REMOVAL_BACKLOG.md` (deletion scope once locked).

---

## 6. Validation gate (before ANY deletion)

Single-seed runs are locators, not proof; removing a modulation path is Tier-0. Gate:
- **3 seeds × {Good@10, Moderate@14, Moderate@~1.0-fading (the harsh end — the one untested gap)}**
  coherent QPSK R1/4 — clean-rate ≥ differential, `max_iters` stays bounded (no climb to 50).
- **No regression** on AWGN@10/@20 + the coherent QPSK R3/4 AWGN@20 path.
- The harsh-Moderate (~fading 1.0) run is the real ceiling test — 0.71 was the low end of the band.

Higher-rung coherent (8PSK/16QAM at their SNRs) is a throughput question for C, **not** a gate for
removing differential-OFDM — don't conflate.
