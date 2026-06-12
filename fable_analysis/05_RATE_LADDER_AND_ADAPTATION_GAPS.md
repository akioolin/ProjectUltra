# 05 — Rate ladder & adaptation gaps

The adaptive ladder (gate + ssthresh, 06-11, 5/5 PASS Good@20) is architecturally
settled and good work. But it optimizes within a family that cannot reach the target.

## 1. Structural: modulation is frozen at connect

- `RateController`'s ladder is **CodeRate-only** {R1/4…R5/6} (`rate_controller.hpp:88-94`).
- Every adaptive `requestModeChange` passes `data_modulation_` **unchanged**
  (`connection.cpp:1783`). No modulation-promotion mechanism exists anywhere.
- `kCoherentLadder` (`waveform_selection.hpp:122-134`): QAM16 rungs present but ALL
  `kRungDisabledDb`; **no 8PSK/QAM8 rung exists at all** ("thread C to restore",
  `docs/RATE_LADDER_ANCHORS.md:43`). Forced knobs (`ULTRA_FORCE_DATA_MOD`) bypass the
  ladder; the plumbing (enum, pilot profiles, demap) is intact.
- The MODE_CHANGE wire format already carries modulation → adding (mod, rate) rungs is
  a **policy + signal problem, not a wire change**. Verify the peer-side MODE_CHANGE
  handler accepts modulation changes before building (open question from the audit).

**Consequence:** even a perfect ladder converges to QPSK R3/4 (rarely R5/6) ≈
1.9-2.05 kbps e2e. See 01 §6 for what 8PSK/16QAM rungs would deliver.

## 2. R5/6 pilot-spacing fall-through (top-rung bug)

`recommendedPilotSpacing` (`include/ultra/ofdm_link_adaptation.hpp:46-64`) has explicit
cases for R3/4 and R2/3 but **no `R5_6` case** — the top climb rung (added 2026-05-28)
falls into the `default` "channel is rough" dense-pilot branch (spacing 5, 47 data).
Result: R5/6 yields **+1.5-2.4% over R3/4 instead of the +11%** the rate ratio promises
— the R3/4↔R5/6 oscillation that ssthresh was built to kill was thrashing over a ~1.5%
prize. Fix = add `case CodeRate::R5_6: spacing 8`. Both TX and RX derive spacing from
the same function at MODE_CHANGE time, but **verify both sides recompute on the same
(mod,rate) inputs** before shipping (a one-sided change desyncs carrier geometry — the
exact 0/8 failure mode that killed unilateral rate flips, CHANGELOG 06-09).
Caveat honestly: R5/6's 17% redundancy already loses to Good's ~23% instantaneous fade
erasure at QPSK (`RATE_LADDER_ANCHORS.md:89-91`) — this fix matters mostly as a
clean-channel/AWGN rung and for the future 8PSK R5/6 rung, not as a Good@20 lever.

## 3. Entry-rung misclassification (~25-35% of small-file airtime)

The fading classifier cannot discriminate Good from Moderate (16-seed evidence:
identical distributions). Entry split on Good@20: R3/4×6 / R2/3×8 / R1/2×2 — **~60% of
sessions enter 1-2 rungs low**, and the climb costs ~4 group-ACKs (~40-45 s) per rung
(EMA α=0.4, midpoint reset 0.475, streak 3; `rate_controller.hpp:47-67,222-225`).
On a 21 KB file that's ~25-35% of airtime below the best rung. Mitigations, in order:
1. Wire the **CIR delay-spread metric** (commits e4405f2/3be0975 — frequency
   autocorrelation of the LTS H → coherence BW → τ_rms; separates Good ~0.12 ms from
   Moderate ~0.25-0.38 ms) into class selection. It is the right *frequency-axis*
   signal; it still needs a **Doppler companion** (temporal autocorrelation across
   symbols/frames) for coherence-time decisions (CW cap, interleave depth currently key
   off the broken `fading_index` via `designDopplerForFadingIndex`,
   `connection_policy.hpp:50-54`).
2. Faster ramp: first-climb after 2 consecutive good groups (vs 3) when entry was below
   the class anchor — bounded, reversible.
3. For principled MOD selection (the 16QAM gate), neither suffices: the decision needs
   the **per-carrier post-EQ SNR/EVM distribution** vs per-constellation thresholds
   (the genie bit-load logic already used exactly this form: "85% of carriers
   16QAM-capable"). Keep LDPC-iteration headroom as the closed-loop trim.

## 4. WIP risk: sticky escape-drop ceiling is never reset

The uncommitted working tree adds `Connection::maybeEscapeStuckFrame()` (escape a frame
stuck at retry ≥5 by forcing a one-rung drop — targets the Moderate@18 max-retries
deaths). Mechanically sound (seq-preserving: `setCodeRate` rewinds `tx_next_seq_` to
base on both sides). **But** `noteRungFailed()` sets a sticky ceiling that is never
re-probed (`rate_controller.hpp:188-199`) and `rate_controller_.reset()` has **zero
call sites** — and `Connection` is a value member of `ProtocolEngine`
(`protocol_engine.hpp:275`), i.e. it lives as long as the app: one transient fade
event caps the rate across every later transfer AND every later connect/disconnect
cycle until the program restarts. Fix before commit: call
`reset()` (or clear stickiness) on new-transfer start or DISCONNECT/CONNECT, or make
the sticky ceiling decay after N clean groups.

## 5. Harness watchdog will fight the future ladder

`gui_qso_scenario.sh`'s unexpected-mode watchdog hard-fails any run whose modulation
drifts from `--expect-mod` (`tools/gui_qso_scenario.sh:216-231,329-338`). A legitimate
QPSK→8PSK promotion under a future (mod,rate) ladder **will be killed as
`unexpected_data_mode`**. Before enabling modulation rungs, add an "any coherent mod"
expectation option. (Forced runs already disable the watchdog, `:253-256`.)

## 6. Entry floors / stale facts

Code entry floors: AWGN 8, Good 10, Moderate 14, Poor 1e9 (`waveform_selection.hpp:29-38`)
— CLAUDE.md's "AWGN 10 / Good 12 / Moderate 14 / Poor 18" is stale. Also
`rate_controller.hpp:9-11` claims the RECEIVER runs the controller — stale; the sender
runs it on the receiver's 3-bit `rate_hint` (LDPC-iteration headroom quality signal,
`streaming_burst_interleave.cpp:652-659` → `connection.cpp:251-253,1635-1639`).
