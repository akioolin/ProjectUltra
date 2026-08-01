# Default-off environment knobs

Status: written 2026-08-01, against `v0.5.1-pre-alpha`.
Scope: every `ULTRA_*` knob that is **off by default**, plus the handful of default-ON
knobs whose *opt-out* is the interesting half. Companion to
`docs/MODEM_INFRASTRUCTURE_MAP.md` (the live stage/knob map) and
`docs/REMOVAL_BACKLOG.md` (the deletion tracker).

Two halves:

* **[THE SHORTLIST](#the-shortlist--built-plausible-never-properly-tested)** — knobs that
  were built, look plausible, and were never properly tested, or were abandoned for
  reasons that no longer hold. This is the valuable half.
* **[Full reference](#full-reference)** — what every default-off knob actually does.

---

## How to read this

### Determining the true default

The default is in the *predicate*, not the name. Five patterns are in use, and three of
them are traps:

| Pattern | Example | True default |
|---|---|---|
| `e && e[0]=='1' && e[1]=='\0'` | `selective_repeat_arq_policy.hpp:287` | **OFF**, strict — only the exact string `1` enables |
| `e && e[0]!='\0' && e[0]!='0'` | `streaming_decode_policy.hpp:53` | **OFF**, loose — `false` / `no` / `off` all *enable* (anything not starting with `0`) |
| `e == nullptr \|\| atoi(e) != 0` | `waveform_selection.hpp:712` | **ON** — an opt-out; `=0` disables |
| `!(e && e[0]=='0')` | `modem_engine.cpp:567` | **ON** — an opt-out |
| `getenv(...) != nullptr` | `streaming_burst_interleave.cpp:512` | **OFF**, presence-only — **`=0` ENABLES it** |
| numeric with a fallback | `connection.cpp:5104` | the **fallback value is the default**; out-of-range input silently falls back |

Three live footguns, verified in the tree today:

* **`ULTRA_ACK_REPEAT_SILENT_MS=0` does not turn the feature off.** The guard is
  `if (v >= 300 && v <= 5000) return v; return 4000u;` (`src/gui/app.cpp:2913-2919`), so
  `0` fails the range test and falls through to the default. Any run ever labelled
  "repeat off" was a **null A/B — both arms ran identical code**.
* **`ULTRA_TNC_ACCUM_DISABLE` is inverted**: `bulk_accum_ = getenv(...) == nullptr`
  (`src/tnc/tnc_session.cpp:177`), so setting it to *any* value including `0` disables
  accumulation.
* **Presence-only knobs enable on `=0`**: `ULTRA_GAMMA_DOMAIN_LOG`,
  `ULTRA_CHEST_DELAY_SCAN`, `ULTRA_CHEST_NMSE_DUMP`, `ULTRA_TNC_ACCUM_PROBE`,
  `ULTRA_TNC_FLUSH_LOG`, `ULTRA_CHANNEL_DELAY_MS` (the `=0` case additionally flips
  `multipath_enabled=false`).

Also: a knob wrapped in a function-local `static const` **latches on first call** and
cannot be toggled within one process. `ULTRA_ITERATIVE_CHEST`, `ULTRA_CHEST_NOISE_SCALE`
and `ULTRA_EVM_DEMOTE_CONFIDENT` were deliberately written *non*-latching so both arms can
run in one binary; `ULTRA_ACE_PAPR` and `ULTRA_RX_AGC` latch, so each arm needs its own
process.

### What "properly tested" means here

The IONOS rig has a **paired A/B standard deviation of 14–36 %**. Consequently:

| n (interleaved pairs) | resolves roughly |
|---|---|
| 3 | nothing |
| 8 | ~15 % |
| 12 | ~12 % |
| 20–30 | ~8 % |

Anything under ~5 % is structurally unmeasurable on this bench — the 16QAM R2/3 Good@20
gate has been observed swinging **0–2060 bps on identical config**
(`MEMORY 2026-06-12`), which is what retired the `eps_H` "wall-mover" as noise.

This is why several entries below say **UNDER-TESTED** rather than "rejected". Two
recorded examples of why n≤3 is not evidence:

* a forced-rung result of "+10.6 % unanimous over 2 pairs" became **+5.2 %, p=0.73 at n=8**;
* `ULTRA_RUNG_DWELL_MS` was built on a **−0.58 correlation that was reverse causation**
  (a rough channel causes *both* the churn and the low goodput).

**A knob is closed** if it was measured over ≥8 pairs, **or** if a *mechanism-level null
control* fired — i.e. the knob demonstrably did what it was designed to do and the outcome
did not improve. That second form legitimately closes a case at n=2
(`ULTRA_ANCHOR_SKIP_KEEP_STREAK_ON_SWITCH`, `ULTRA_EVM_DEMOTE_CONFIDENT`).

### Status vocabulary

| Status | Meaning |
|---|---|
| `DIAGNOSTIC` | logging/instrumentation only, no behaviour change |
| `MEASURED-HARMFUL` | tested adequately and it lost — closed |
| `MEASURED-WASH` | tested adequately, no effect — closed |
| `UNDER-TESTED` | tested at n≤3 or never A/B'd — **candidate** |
| `NEVER-TESTED` | built, plumbed, zero measurement — **candidate** |
| `SUPERSEDED` | the thing it tunes is bypassed or deleted — deletion candidate |
| `FORCE/DEBUG` | a measurement override; a tool, not a shipping option |

### The v0.5.1 rate-control cliff — read before trusting any rate knob

As of `v0.5.1-pre-alpha` the latent-state controller (`ULTRA_LATENT_RATE`) is
**DEFAULT-ON** and returns early from `updateRxAuthorityCommand` at
`src/protocol/connection.cpp:2596`. Everything downstream of that line in that function is
**unreachable on the default path** — the two-crater rule, ratcheting penalties, climb
dwell, EMA hold, the one-rung walk, the predictive climb, the EVM demote clamp,
trust-pick. Their `getenv` sites still exist; they no longer influence anything unless you
set `ULTRA_LATENT_RATE=0`.

A second, older bypass catches people out even more often: `rxRateAuthorityEnabled()` has
been **DEFAULT-ON since 2026-07-05** (`src/protocol/waveform_selection.hpp:712`), and
`applyAdaptiveRateFeedback` — the entire *sender-side* climb/demote machinery — is in the
`else` arm at `src/protocol/connection.cpp:2000-2013`. It has been dead by default for a
month longer than the latent controller.

Those knobs are listed under
**[Superseded / deletion candidates](#superseded--deletion-candidates)**, not as options.

---

## THE SHORTLIST — built, plausible, never properly tested

### Tier 0 — free repairs. Do these before any rig time.

These are not experiments. They are defects found while auditing, each with a one-line or
one-grep fix, and two of them **invalidate future measurements** if left alone.

| # | Item | file:line | What is wrong | Fix |
|---|---|---|---|---|
| R1 | `ULTRA_COMMANDED_GEOMETRY` gates **two** things | `src/gui/modem/streaming_ofdm_decode.cpp:656` and `:426` | Commit `4055831`'s message says "the truncation/alignment guard stays default-ON — it is an independent correctness guard". It is not: there is one knob, and `streaming_decode_policy.hpp:52-55` is default-OFF. Flipping it off for reason (B) also disabled the truncation guard (A) — the half that caused the observed 28.2 s dead-air / RTO timeout. | Split the knob so (A) is unconditional or independently gated. **Correctness, not a throughput bet.** |
| R2 | `ULTRA_ACK_REPEAT_SILENT_MS` opt-out is inoperative | `src/gui/app.cpp:2913-2919` | `atol("0")=0` fails `v>=300` and falls through to `return 4000u`. The feature **cannot be disabled from the environment**; `if (kAckRepeatSilentMs > 0)` at `:2920` can never be false. Introduced by `40d84d0` (`0u`→`4000u`), which left the pre-existing range guard in place. | Accept `0`: `v==0 \|\| (v>=300 && v<=5000)`. Also widen the upper bound — at MPG, Tc≈4.2 s, so the shipped 4000 ms is only marginally decorrelated and the `5000` ceiling makes longer delays unreachable. Fix the two stale comments at `:2909` and `:2913`. |
| R3 | LTS residual-CFO log lies about *why* it skipped | `src/ofdm/channel_equalizer_lts.cpp:554-555` | A 2.71 Hz residual against a 0.3 Hz threshold prints `(below correction threshold)` from an `else-if` catch-all when it actually failed the seed/CV gate. The informative branch at `:548-553` is behind `cfoDebugLogEnabled()`. This is recorded as the reason the root cause took so long to find, and it nearly caused an A/B null control to be read **backwards** (178 vs 38 "LTS residual CFO" lines, of which 176 were *rejections*). | Make the catch-all state the actual rejection reason. |
| R4 | 44 knobs missing from the live map | see [Not in the infrastructure map](#not-in-the-infrastructure-map) | `ULTRA_LATENT_RATE` — the **default rate controller since 2026-08-01** — has zero hits in `docs/MODEM_INFRASTRUCTURE_MAP.md`. So do `ULTRA_BURST_ESCALATION` (default-ON, +24 % rig) and `ULTRA_ECHO_REANCHOR_GATE` (default-ON, +17 % rig). CLAUDE.md makes the map mandatory-live; a wrong 🟢/🔴 causes a wrong deletion later. | Add them. Fix the three stale map/doc rows in the same pass (R5). |
| R5 | Three doc/code disagreements | `MODEM_INFRASTRUCTURE_MAP.md:57,58,426` + `KNOWN_BUGS.md:147,278` say `ULTRA_COMMANDED_GEOMETRY` is DEFAULT-ON (it is OFF since `4055831`); `MAP:233` says `ULTRA_MODE_CHANGE_RETRY_MS` defaults to ~5 s ratiometric (the `getAckTimeout()` floor was restored, so it is ~18.5 s); `KNOWN_BUGS.md:805` says `ULTRA_ACKLISTEN_SUPPRESS_OFDM` is default-OFF (it is ON). | — | Per CLAUDE.md, fix the map first, then proceed. |
| R6 | `ULTRA_LDPC_MAX_ITERS` — close it with a grep, not a sweep | `src/fec/ldpc_codec.hpp:98` | The motivating observation ("21 of 26 failures stopped at exactly 70 iterations") is a **tautology**: `ldpc_decoder.cpp:234-267` has exactly one non-cap exit and it is success, so every failure reports `iters == max`. | Grep the existing forensics corpus for **successful** `iters=` values and `RETRY OK` density. Worst accepted CW on record is 25 of 50 (`docs/CALIBRATION_AUDIT.md:861`). If no successful decode anywhere exceeds ~30, there is no slow-convergence population and the question closes with **zero new runs**. |
| R7 | Rename the `SELF-ECHO` log | `src/gui/modem/streaming_ofdm_decode.cpp:2672` | `ULTRA_ECHO_REANCHOR_GATE` shipped on a mechanism that was later **proven wrong** (`KNOWN_BUGS.md:812`): the signal was the *peer's tone ACK*, and self-echo is physically excluded on both benches. The fix keyed on the right state (monitor armed) and remains valid, but the log line and `ack_listen_self_echo` still name a mechanism that cannot happen. | Rename. It has already cost one investigation. |

### Tier 1 — ranked experiment candidates

Ranked by *expected value net of rig cost*, not by how good the idea sounds.

| # | Knob | file:line | What it would do | Why it is worth a run | Can the rig resolve it? |
|---|---|---|---|---|---|
| 1 | `ULTRA_CRATER_REANCHOR_HOLD` | `streaming_ofdm_decode.cpp:2691` | Two-crater discipline for the receiver's **sync** re-anchor: hold warm sync through crater #1, re-anchor from #2 | Only knob here with a mechanism metric *and* a positive paired result that was never refuted by a clean solo test. The revert is confounded 3 ways (see below) | Marginal. Effect is now smaller than the +44 % that motivated it → **n≥12** |
| 2 | `ULTRA_BURST_INTERLEAVE` | `connection_policy.hpp:239` | Cross-frame codeword interleave: TX permutation + whole-group ACK semantics + wire flag | **Two measurements disagree**, each at n=5–6, and the mechanism is channel-class-dependent *by construction* | Yes, but only **per channel class**, n≥8 each. Do not re-A/B it as a global boolean |
| 3 | `ULTRA_ACK_REPEAT_SILENT_MS` | `app.cpp:2914` | Second, Tc-decorrelated copy of the tone ACK after an unbroken quiet window | Default-ON at 4000 ms with **no goodput A/B at any n** and seven collision-repair patches behind it. Cheapest real A/B in the tree once R2 lands: one env var, no rebuild | Yes — n≥8, and the arms genuinely differ once the guard is fixed |
| 4 | `ULTRA_KEEPALIVE_ACK` (+`_MS`) | `connection.cpp:5093` / `:5104` | Re-emit the cumulative tone ACK after `_MS` of silence during an active receive | Upside is rig-proven and reproducible (max ACK-silence gap **capped at exactly 8.0 s vs 36.7 s off**). Both original blockers named a specific, bounded fix | **Blocked** — two prerequisites first (below). Prize is ~3–4× smaller than the retracted headline |
| 5 | `ULTRA_CONNECT_ACK_RESCUE_DEFER` | `connection.cpp:4635` | Defer, rather than destroy, an armed CONNECT_ACK rescue when an OFDM data sync is accepted | **Do not flip this knob** — 3/3 deadlocks vs ~1-in-20 baseline. But the bug it targets is rig-proven twice and open: permanent half-open, **420 s per attempt** | It is a reliability redesign, not an A/B. Ranks high if "never hangs for 7 minutes" outweighs bps |
| 6 | `ULTRA_LTS_CFO_SEED_OFF` | `channel_equalizer_lts.cpp:469` | Bypass the seed/CV gate blocking the LTS residual-CFO refinement | **Contested** — two independent adversarial passes reached opposite conclusions (below). Mechanism is proven at n=80/cell, 3–5σ, on Moderate | Not on the rig first. There is a **cheap sim experiment already plumbed** that nobody ran |
| 7 | `ULTRA_BURST_RMS_DIAG` (+ `ULTRA_BURST_ERASURE_ABSOLUTE`) | `streaming_burst_interleave.cpp:645` / `:679` | Log the erasure-gate inputs per collected burst frame; the sibling forces the legacy absolute gate for the OFF arm | Closes a measurement **owed since 2026-06-16**. The operating-level-relative erasure gate was justified *analytically*; the 16-of-20 frames it keeps have never been confirmed to actually decode | Yes — **one** instrumented rig transfer, not a campaign |
| 8 | `ULTRA_ACE_PAPR` | `modulator.cpp:285` | Active Constellation Extension: extend outer points outward to shave TX crest | ~1 dB of average power, non-negative by construction at the demapper. CPU cost measured negligible (0.25–0.36 % of a core) | **No** — 0.8–1 dB needs n≈20–30 pairs. Run the free null control instead (below) |
| 9 | `ULTRA_SIM_PAPR_PENALTY` | `app.cpp:3625` | Make the sim TX peak-normalize like hardware instead of RMS-normalizing to a fixed reference | Not a throughput lever. It is a **simulator-fidelity decision**: with it off, OTASim delivers ~10 dB more data-frame average power than any peak-limited PA | N/A — needs a decision, not a measurement |
| 10 | `ULTRA_LATENT_RELAX_DB` | `connection.cpp:2627` | Forgetting rate of the default-ON latent controller's posterior — "THE ONE FREE PARAMETER" | Highest leverage of anything here (it is on every OFDM connection), but its named failure mode was already checked at n=8 and did not fire | **Not yet.** The tie-break probe must be gated first (below), or the sweep is under-powered by construction |

---

### Detail, ranked

#### 1. `ULTRA_CRATER_REANCHOR_HOLD` — `src/gui/modem/streaming_ofdm_decode.cpp:2691`

**Default:** OFF, `e && e[0]=='1' && e[1]=='\0'`. Was DEFAULT-ON 2026-07-21 → 2026-07-23.

**Mechanism.** Default behaviour forces a full chirp+LTS re-anchor
(`expect_full_ofdm_anchor_ = true`) on *every* 0-CW group crater — ~7× per 50 KB transfer,
each costing a ~1.2 s chirp prefix and delaying the ACK into the sender's RTO. The knob
holds warm sync through crater #1 and re-anchors from #2 —
the same restraint the F122 rate rule already uses. The physical argument is real and
code-checkable: a fade attenuates but does not move sample timing, whereas
`expect_full_ofdm_anchor_=true` plus `resetFrameArrivalTrackingLocked` discards the
warm-sync prediction and re-arms a wide blind search. `crater_reanchor_streak_` resets on
any decoded frame (`:2644`), so isolated craters never reach streak 2.

**Evidence.** PRO: F470-481, MPG@20, 6 interleaved pairs — full re-anchors **halved
(25 vs 50)**, delivered goodput +44 % (1.87 vs 1.30 kbps), delivery tied 4/6
(`CHANGELOG:3504`). CON: reverted on F580-591 (`CHANGELOG:3641`) — delivered +11 % but
2/6 vs 1/6 failures = aggregate wash.

**Why the revert does not close it.** Three confounds, all verbatim in `CHANGELOG:3627-3655`:
(1) it was an **all-levers-ON** test (with `ULTRA_RX_EMA_HOLD` and `ULTRA_CHEAP_REANCHOR`)
— no per-lever attribution; (2) the baseline arm ran forced `ULTRA_BURST_INTERLEAVE=1`,
measured at −37 % *the same session*, and the entry itself says the re-anchor gap
"was inflated"; (3) the surviving objection — "the hold levers OVER-HOLD a failing **rung**"
— is an argument about the rate controller, and this is a **sync** knob whose getenv is
nowhere near `updateRxAuthorityCommand`.

**Two things that argue against, and must be priced in.** The crater population it acts on
has **shrunk** since F470-481: `ULTRA_COMMANDED_GEOMETRY` (2026-07-28) removed
stale-geometry 0/N craters, which were never fades and so were never valid targets for this
mechanism, yet were part of the +44 %; and the latent controller cut mode changes 5.0 → 2.2
per run, removing post-switch acquisition craters — and `CHANGELOG:2873` establishes
post-switch as the *riskiest* moment to omit acquisition. Also, the +44 % is over delivered
runs only (4 of 6 pairs) on an epoch the entry itself calls "rough/FAIL-heavy".

**Experiment.**
* Arms: `ULTRA_CRATER_REANCHOR_HOLD=1` **alone** (leave `ULTRA_CHEAP_REANCHOR=0`, so the
  branch is `streak>=2`) vs clean default. Interleaved, same epoch.
* n ≥ **12** — the expected effect is now smaller than the +44 % that motivated it.
* **Primary:** delivered goodput with failed transfers scored as **zero**, plus completion
  rate. Do not report delivered-only means.
* **Secondary:** craters/run and resends/run — the two metrics that killed the sibling
  anchor-skip knob (`CHANGELOG:2873`: re-anchors down, craters 2.0 vs 1.0, resends
  1.0 vs 0.0, goodput a wash).
* **Engagement null control only:** full re-anchors/run. This metric *restates the knob's
  definition* and must never be the primary readout.
* **Pre-committed falsifier:** craters/run or resends/run up → stop.

#### 2. `ULTRA_BURST_INTERLEAVE` — `src/protocol/connection_policy.hpp:239`

**Default:** OFF for all modulations. `if (const char* env = getenv(...)) return env[0]=='1'; return false;` —
note the `Modulation` parameter is **unnamed and ignored**.

**What it controls.** Single source of truth for three coupled things: the TX byte
permutation (each LDPC codeword spread across all N frames of the group), the ARQ semantics
(ON = whole-group ACK/NACK, OFF = per-frame SACK masks via `Connection::burst_interleave_off_`),
and the on-wire `BURST_FLAG_INTERLEAVE` bit. Consumed at `src/gui/modem/modem_mode.cpp:323`.

**Two measurements, opposite signs, neither adequate.**

| Date | Config | n | Result |
|---|---|---|---|
| 2026-06-14 | GUI, Good@20, 16QAM R2/3 | 6 | interleave **ON** +47 % (~1270 → 2033-2240 bps) |
| 2026-07-21 (F440-449) | rig MPG@20, forced 16QAM R1/2 | 5 | interleave **OFF** +37 % (1.55 vs 1.13), 5/5 vs 4/5 delivery, **5× fewer full craters (12 vs 62)** |

They differ in rung, in forcing, and in epoch. The OFF side is stronger because it carries
a mechanism metric, but 5 pairs does not close a question at this bench.

**The mechanism is class-dependent by construction.** Interleaving turns a frozen
*frequency*-selective null into a recoverable ~1/N nick — but couples a *time*-localized
Watterson fade into every codeword of the group at once, converting one dead frame into a
whole-group 0/N crater. That is a delay-spread-vs-Doppler question, and this knob is a
single global boolean whose modulation argument is discarded: exactly the
`if (mod==X)` / one-channel smell the CLAUDE.md adaptivity rule names.

**What changed.** The project now has a working channel-class discriminator it did not have
in July (fixed 2026-07-25: GOOD +0.606 vs MOD/POOR −0.285, 0.89 separation).

**Experiment.** Not another global A/B. Gate the profile on the **measured class** — ON
where delay spread dominates, OFF where Doppler does — and test per class at n≥8
(MPG and MPM). Do **not** re-flip the global default on the 2026-06-14 number.

#### 3. `ULTRA_ACK_REPEAT_SILENT_MS` — `src/gui/app.cpp:2914`

**Default:** **4000 ms, DEFAULT-ON.** Listed here because its *opt-out is broken* (R2) and
because it is default-ON on zero throughput evidence.

**What it does.** On every distinct tone-burst ACK, stashes a decorrelated copy; the GUI
tick (`App::maybeFireAckRepeatIfSilent`, `app.cpp:2830`) fires it once, +4000 ms after
copy-1 airtime ends, and only after an unbroken quiet window. Rationale: the ACK is 4-FSK
across 2400-2625 Hz = a **225 Hz span** (`tone_burst_constants.hpp:63-71`), and ITU Good's
0.5 ms delay spread gives Bc ≈ 2 kHz — so the whole ACK fades **flat**. It has no frequency
diversity by construction; time diversity is its only diversity. A repeat delayed past Tc
rides a decorrelated fade draw (p² instead of p). GUI-only — `ultra_tnc.cpp:509` queues the
tone ACK with no repeat path.

**Evidence.** **No goodput A/B at any n.** The recorded history is six successive
collision-repair patches, each found after the previous version was believed safe: F100
(25 redundant repeats blanked inbound burst heads, 13 craters), F129, F143, F147 (false
chirp locks on idle noise cancelled the repeat; sender went RTO-deaf 40 s), F176/F221
(geometric air-end gate), F222 (dedup-arm), F227 (repeat fired 1.3 s after the peer's burst
SYNC, wiped frame 1, ~3× MC-DPSK goodput loss). The default was flipped 0→4000 in the
2026-07-05 bulk knob graduation (`40d84d0`), not on a paired measurement; the in-code
`// campaign-validated` comment is unbacked.

**Experiment.** After R2: `=0` vs default, n≥8 interleaved at MPG@20, primary delivered
goodput, secondary craters attributable to inbound-burst blanking. Note this keys a second
TX on a half-duplex channel purely on an *inference* (silence ⇒ our ACK died); the record
shows that inference misfiring repeatedly.

#### 4. `ULTRA_KEEPALIVE_ACK` / `ULTRA_KEEPALIVE_ACK_MS` — `connection.cpp:5093` / `:5104`

**Defaults:** knob OFF (`e && e[0]!='\0' && e[0]!='0'`); threshold **8000 ms**, range
[3000,60000]. Was DEFAULT-ON on 2026-07-14 and reverted the same day (`fcfd02e`).
The comment at `connection.cpp:5096` still says "default 25000" — **stale**, and the
5064-5091 block states the default three contradictory ways.

**Upside is real and reproducible.** F290/F291 (MPG@20, `_MS=8000`): max ACK-silence gap
capped at **exactly 8.0 s vs 36.7 s** with it off, across both runs, 13 keepalives/transfer,
cwfails normal. That is an epoch-independent mechanism metric with a null control, not a
goodput draw.

**Three things must be fixed or re-verified before it is worth a pair of rig runs.**

1. **The prize was retracted.** `CHANGELOG:419-452` (null-controlled, ADAPTIVE-RTO logging
   on *both* ends): "The SENDER's configured timeout is 17.4–23.1 s, not 44.7 s. The 44.7 s
   figure was read from the Mac's log, and the Mac is the file RECEIVER." The stall-measurement
   *method* was also withdrawn — it counted gaps between "Burst group complete" events, and
   during a NACK round the sender is transmitting. `KNOWN_BUGS.md:406-410`'s "~+30 %" rests on
   both retracted inputs and was never updated. An 8 s cap against a 17.4–23.1 s RTO saves
   ~9–15 s per stall on only the *idle* fraction.
2. **The collision gate is measured leaky.** `KNOWN_BUGS.md:201-214` (two-station DEBUG
   capture, clocks pinned by three independent ACK arrivals) measured *both* terms of
   `channelBusyForTx() || burstAirSamplesRemaining() > 0` failing at once: CCA read `idle=1`
   mid-burst in a ~7 dB fade trough, and the air-gate is computed from a stale
   `fixed_frame_codewords_` that is zeroed at finalize. The recommended 8 s is *shorter* than
   the 11.5 s escalated burst ceiling, so the leaky gate is the sole protection. **Do not
   adopt the filed refinement** to gate on `burstAirSamplesRemaining` only: that returns 0
   until a group *arms* (`streaming_burst_interleave.cpp:459-469`), and the sync-reject stall
   this knob targets is by definition the case where no group arms — so the geometric term
   reads "clear" for the whole inbound burst.
3. **The rung-neutral keepalive is still required.** The re-emit still stamps the standing
   absolute rung command (`connection.cpp:445-447`, `tba.rate_hint = rx_authority_cmd_ & 0x7`),
   and `noteAnchoredBurstNoGroup` (`:4521-4534`) still drives `observe(cur, k=0, M=5)` into the
   latent posterior mid-stall. v0.5.1 removes the *specific* July driver (a momentary
   usable-8.8 SNR misread) because the latent controller consumes no SNR — but a frozen
   command can be just as depressed, driven by outcome instead. Emit base+bitmap only.

**Experiment (after 2 and 3).** n≥8 interleaved, MPG@20, `_MS=8000` (which is already the
default — passing it changes nothing). **Primary: delivered goodput**, not the silence gap;
F290/F291 already proved the knob does what its name says, which is not the open question.
Carry a collision/self-deafen counter and a `burst_clean_group_streak_` reset count —
`noteArqRoundOutcome` zeroes that streak at `connection.cpp:3628`, and it gates the
group-size escalation lever that shipped at +24 %.

#### 5. `ULTRA_CONNECT_ACK_RESCUE_DEFER` — `src/protocol/connection.cpp:4635`

**Default:** OFF, strict. **Do not flip it.**

Rig 2026-07-30: with the defer on, **3/3 handshake attempts deadlocked** against a ~1-in-20
baseline; P(3 of 3 | 1/20) = 1.25e-4, so n=3 is adequate here — this is a
catastrophic-failure-rate change, not a 15 % goodput delta. Mechanism: releasing the rescue
fires up to 6 CONNECT_ACK re-sends of 8.3 s each (~50 s keyed up) while the initiator is
transmitting its own CONNECT retries — **both stations keyed at once on a half-duplex
medium**.

**But the defect is open and expensive.** BUG-CONNECT-ACK-RESCUE-DISARM: a sync correlation
is *not* proof of a peer state transition. Measured at corr=0.59 and corr=0.80 with the
initiator still retrying nine times into a responder that had already cleared
`connect_ack_frame_` and would never answer — permanent half-open, **420 s per attempt**
(`KNOWN_BUGS.md:733`).

**The work item is a redesign, not a knob:** a *cheap* rescue (a short control-frame re-ACK,
not an 8.3 s MC-DPSK blast) plus a carrier-sense hold that actually covers the peer's TX
window. Practical hook already in the data: in the first case every decode after the
"accepted sync" read **1.1–2.7 dB EVM with delay spread rejected** — a real data burst does
not look like that, so EVM + delay-spread validity is an available discriminator needing no
wire change.

#### 6. `ULTRA_LTS_CFO_SEED_OFF` — `src/ofdm/channel_equalizer_lts.cpp:469`

**Default:** OFF (`e != nullptr && e[0] != '\0' && e[0] != '0'`), non-latching.

**What it does.** Production applies the LTS residual CFO only when
`coherent_residual && (trusted_cfo_seed || flat_lts_channel)`. The knob forces `seed_ok`
true, leaving only the coherence test (≥0.70) and the 0.3–5.0 Hz magnitude window.

**This entry is contested — two adversarial reviews reached opposite conclusions.** Both
are recorded because the disagreement is the useful information:

* **Keep it open (UNDER-TESTED).** The FER tier is closed-quality: `measure_ack_fer`,
  QPSK R1/2 Moderate, seeds 7+11, **n=80 frames/cell** — gate ON 65/80 at 20/28/**60** dB
  (a floor flat across 40 dB of SNR is a receiver impairment, not an outage), gate bypassed
  78/80 @20, 80/80 @28, 80/80 @60. Full 12-cell sweep: AWGN byte-identical, Moderate
  **+31/320 (3–5σ)**. The goodput tier was answered at **n=6** against a baseline spread of
  410–1620 bps (4×) — i.e. not answered. Corroborated independently by `KNOWN_BUGS.md:450`
  (BUG-ANCHOR-CFO-KILL) which already names the gate as structurally off on fading.
* **Close it (MEASURED-WASH on the shipping metric).** Good is **−7/320, consistently
  negative** across all four rungs at n=80/cell — the harmful direction on the channel we
  actually operate on. And two **mechanism-level null controls already ran**:
  `ULTRA_LTS_CFO_COH` at 0.70/0.85/0.93 → identical results; `ULTRA_LTS_CFO_MIN` at
  0.3/1.0/1.8 Hz → identical results. Verbatim conclusion: *"the Good cost is a genuine,
  coherent, large rotation whose correction still hurts… A principled fix would weight the
  correction by its persistence (e.g. a per-symbol tracked phase rate rather than a
  frame-constant CFO), not gate it on channel flatness."*

**What both agree on.** The bypass is the wrong *shape*: it applies a frame-constant
extrapolation of an instantaneous two-symbol phase slope across a 26-symbol payload, which
over-reaches when the true offset is ~0 (Good) and helps when drift dominates (Moderate).
Neither coherence nor magnitude separates the two cases. A **persistence** gate is the
named successor, and it has nowhere to live today: `last_lts_residual_cfo_hz` is zeroed in
`OFDMDemodulator::reset()` (`ofdm_stream_processor.cpp:1229`), called per decode, so it
must be hosted in `StreamingDecoder` and pushed down (the pattern already used for
`doppler_coherence_`).

**The cheap experiment nobody ran.** The claim that "the sim cannot answer this because
OTASim has no CFO" is **false**: `SimulatedChannel::setTxCFO` exists at
`src/ota_channel_core/channel.cpp:46-47`, applied at `:107/:119` *independently* of the
Watterson `cfg.cfo_hz` that `:173` zeroes, and `ModemEngine` already plumbs
`config_.tx_cfo_hz` (`modem_engine.cpp:66,270`). The decisive question — does a real nonzero
TX CFO (the rig ppm case) change the verdict — is a **harness plumb-through of an existing
facility**, minutes of work, not an n≥8 GUI campaign. Do that first. Then, if it is still
open, re-A/B the knob *unmodified* on the v0.5.1 default path on **both** Moderate and Good
(Good has never been run through the GUI gate, and its −7/320 cost is the design risk).

**Unstated interaction to price in:** every applied residual is pushed into the persistent
CFO prior via `ingestPilotResidual` (`cfo_tracker.cpp:34-42`), clamped only at 2.0 Hz, and
that file carries a standing warning that ingest happens *before* any LDPC verdict exists.
The knob multiplies the ingest rate ~11× (2→22 applied on one seed).

#### 7. `ULTRA_BURST_RMS_DIAG` + `ULTRA_BURST_ERASURE_ABSOLUTE`

`streaming_burst_interleave.cpp:645` and `:679`. Neither is a lever; together they are one
owed measurement.

The burst erasure gate became **operating-level-relative** on 2026-06-16 —
`max(0.055f * burst_anchor_rms_, 0.005f)` (`streaming_burst_interleave.cpp:674-693`) —
because the legacy absolute 0.015 floor implicitly meant "~25 dB below the *sim* anchor
0.27" and on IONOS became only ~5 dB below the anchor, erasing recoverable frames (measured:
data frames 2-6 erased every group at RMS 0.0038-0.0145 → header invalid → ARQ stall). The
fix was justified **analytically** (k=0.055 reproduces 0.015 at the sim anchor, hence zero
sim regression by construction) and the CHANGELOG states plainly that a sim A/B of its
*benefit* is structurally blocked — lowering sim TX level enough to bite the gate also
craters SNR. So the fix that keeps 16 of 20 previously-erased frames has **never been
confirmed to decode them**. `KNOWN_BUGS.md:1279` still carries the owed action.

**Experiment.** One instrumented rig transfer, Mac→Pi5 MPG, `ULTRA_BURST_RMS_DIAG=1`. Read
the kept-vs-erased separation on `next/anchor` and `next/noise`, and confirm the kept frames
actually decode. Use `ULTRA_BURST_ERASURE_ABSOLUTE=1` as the OFF arm in the same session.
Cost: one transfer.

#### 8. `ULTRA_ACE_PAPR` — `src/ofdm/modulator.cpp:285`

**Default:** OFF, latching static (each arm needs its own process).

Active Constellation Extension on DATA symbols only (gated on `genie_capture`, so
LTS/probe/preamble stay pristine), coherent mods only. Points move only *outward*, so a
memoryless demapper cannot do worse. Unit-proven in `tests/test_papr_ace.cpp` (CTest
`PaprAce`): 0.7–0.9 dB PAPR reduction plus both safety invariants.

**What is settled.** Rig A/B F273-F278 (interleaved, natural 16QAM, MPG@30): +1.20 / +0.34 /
−0.65 kbps, mean +8 %, recorded verbatim as "INSIDE the ±25 % fade-epoch noise, so
inconclusive (as predicted: 0.8 dB is too small to resolve in 3 pairs)". CPU: **measured
54–76 µs/symbol against 21333 µs of symbol airtime = 0.25–0.36 % of one core** — the
"unmeasured Pi5 CPU cost" blocker is closed, negative.

**What is not settled, and caps the upside.** Two findings that must travel with this knob:

* **The gain saturates at ~1.06 dB.** Only DATA symbols are ACE'd, so the un-ACE'd chirp
  anchor floors the burst peak that per-burst normalization sees. Measured on a real `ptx`
  burst: chirp envelope peaks at 0.708 vs data peak 0.800, so the chirp sits 1.06 dB below.
  Whole-burst gain was **+0.26 dB** on a short single-frame burst (the chirp is ~40 % of it);
  data-segment gain +0.53 dB; synthetic 60-symbol data burst +1.11/+1.13 dB — already at the
  ceiling.
* **"Zero-risk" is false.** `ULTRA_SOFTWARE_ALC` is **default-ON** and declares a burst
  CLIPPED when RX crest factor falls below 6.5 dB (`connection_policy.hpp:304-311`; verdict
  at `streaming_burst_interleave.cpp:996-1006`), a threshold explicitly set ≥2.5 dB below the
  healthy 9–14 dB floor. ACE removes **1.11–1.13 dB** of crest — ~44 % of that designed
  margin. One false CLIPPED verdict costs a −2 dB `tx_drive` down-step, larger than ACE's
  entire gain.

**Next step is not a throughput campaign** (0.8–1 dB needs n≈20–30 pairs). Run the **free
null control**: one transfer with `ULTRA_ACE_PAPR=1`, grep the receiver's `[ALC-RX]` line
(`streaming_burst_interleave.cpp:1017`) for `cf_db` and verdict. If `cf_db` stays ≥8 dB and
no CLIPPED verdict appears, the zero-risk framing is restored and a many-pair campaign is
justified; if CLIPPED appears, the crest threshold needs retuning before ACE can ship at any
n. **Must run on the rig or with `ULTRA_SIM_PAPR_PENALTY=1`** — OTASim's TX is
RMS-normalized and structurally cannot show a PAPR benefit.

#### 9. `ULTRA_SIM_PAPR_PENALTY` — `src/gui/app.cpp:3625` (+ `ULTRA_SIM_TX_PEAK`, `:3633`)

Not a throughput knob — it makes the sim *harder*. It is on the shortlist because it is an
unresolved **fidelity** decision, and CLAUDE.md makes simulator fidelity non-negotiable.

Default sim TX RMS-normalizes every burst to the fixed in-band reference
(`normalizeTxBurstToReference`, `app.cpp:3652`), so a ~14-15 dB-PAPR coherent OFDM data
frame rides at the *same average power* as the low-PAPR chirp/control frames. With the knob
on, the sim runs the same peak normalization as hardware (`app.cpp:3643`), and data-frame
effective SNR drops by exactly its PAPR back-off (~10.26 dB measured for coherent QPSK by
`tools/papr_tx_measure.cpp`).

Consequence: **every PAPR-class lever is structurally invisible to the faithful gate**, and
the 2026-07-02 sim-based PAPR disable was invalidated by exactly this. It is also the only
path on which the `ULTRA_SOFTWARE_ALC` closed loop is exercisable rig-free.

**Decision needed, not a measurement:** either flip the sim default and re-baseline every
sim number, or record in the map *and* CLAUDE.md that OTASim's TX is intentionally
power-optimistic and that **no PAPR or level claim may be made on it**. Leaving it as an
unnoticed knob is the "the simulator does it differently because it's a simulator" failure
mode.

#### 10. `ULTRA_LATENT_RELAX_DB` — `src/protocol/connection.cpp:2627`

**Default:** 0.35 dB, range [0.0,5.0]. Consumed once per group verdict at `:2633`
(`latent_ctl_.relax(kRelaxDb)`); sole call site outside tests.

This is now **the most load-bearing single constant on the default rate path** — the
controller's own header calls it "THE ONE FREE PARAMETER" and warns that too-small a value
freezes the posterior and *looks perfect on OTASim* (stationary Watterson) while failing on
the rig (`latent_rate_controller.hpp:244-251`). It was set once, never swept.

**But its named risk was already checked.** `CHANGELOG:180-186`, over the same 8 interleaved
rig pairs: "On the rig the posterior stayed live: sd 1.4-2.1 throughout, tracking
k = 3/5..5/5. The run that could catch it did not fire it." That is the exact
spread-and-tracking readout a sweep would produce as its first step.

**Prerequisite that blocks the sweep — and is itself the more urgent item.** The tie-break
probe fires **unconditionally** on the shipped default path (`latent_rate_controller.hpp:319-333`,
`kTieBreakPeriod=4`, `kTieBreakMarginFrac=0.15` at `:193-194`) with no env gate. It was
measured at **+16.5 % mean, sd 27.3 % over 7 pairs**, bimodal (+33..+39 % when it reaches
8PSK, −14/−27 % when it does not) — versus 14.0 % sd without it. The p=0.022 result that
justified shipping the controller is the **pre-probe** configuration
(`CHANGELOG:95,123`: "SHIP CANDIDATE IS THE PRE-PROBE CONTROLLER… The probe stays OFF"),
so the binary that is default-ON is not the variant that was validated.

Any relax sweep sits on top of that variance source, and the two are *coupled* — widening
the posterior lowers the 25th percentile (`kDecilePessimism=0.25`) and changes which rungs
fall inside the 15 % near-tie margin. **Gate the probe on posterior confidence first**
(`CHANGELOG:125-129` already names and argues this; it consumes `spreadDb`, the same
observable relax controls), then sweep relax at 0.15 / 0.35 / 0.7, n≥8.

---

## Full reference

### Rate control / ladder

| Knob | Default | file:line | What it does | Status | Evidence |
|---|---|---|---|---|---|
| `ULTRA_LATENT_RELAX_DB` | 0.35 dB, [0,5] | `connection.cpp:2627` | Forgetting rate of the latent posterior | UNDER-TESTED | Controller as a whole: +14.5 %, 8 pairs, p=0.022. Value never swept; named risk checked (sd 1.4-2.1 on rig) |
| `ULTRA_ENTRY_EVM_CAP` | OFF strict | `waveform_selection.hpp:445` → consumed `connection_handlers.cpp:376-398` | Caps the CONNECT-time entry rung using usable-domain EVM floors vs the data-aided connect reading; cap-only | NEVER-TESTED, **blocked** | No A/B. Blocked by `CHANGELOG:2793` pending the §3 anchor re-measure — precondition **unmet** (`kOfdmLegacyAnchorScaleOffsetDb = 8.70f` still live at `connection_policy.hpp:43`). Fights `ULTRA_CONNECT_AFFINE_BASIS` (default-ON, deliberately **slope-0**: `connection_policy.hpp:876-881`), applies a hard threshold with **no confidence gate** against a 3.14 dB sample σ and 1.1–3.1 dB rung spacing, and is absent from the manual-accept path (`connection.cpp:600-670`) |
| `ULTRA_ENTRY_QAM16_SNR` | unset = OFF | `waveform_selection.hpp:937` | Enter the ladder directly at 16QAM R2/3 on a Good channel above a dB threshold | MEASURED-HARMFUL | F73 (`KNOWN_BUGS.md:809`): a **cold** 16QAM entry decodes marginal (quality 0.35, no warm channel estimate) and its slow decode widened the handshake race, collapsing the ladder to R1/4. Mechanism-level, not a bps delta. Now *worse*: the latent controller seeds its prior from the entry rung |
| `ULTRA_MAX_OFDM_RATE` | unset = no cap; string | `connection.cpp:679`, `:3939`; `connection_handlers.cpp:415`, `:607` | Caps the OFDM code rate | FORCE/DEBUG — **broken** | Caps the **entry only**: `maybeObeyAuthorityCommand` (`connection.cpp:3195-3240`) applies no cap, and `applyAdaptiveRateFeedback` is dead by default. Unparsable values map silently to the AUTO no-cap sentinel — a typo'd arm looks valid and is not. Use `ULTRA_LOCK_RATE` / `ULTRA_RATE_ADAPT=0`, which `:3202` honours |
| `ULTRA_FORCE_DATA_RATE` (+`_MOD`) | unset = AUTO | `waveform_selection.hpp:971`, `:1117` | Pins the code rate; also makes `capInitialOFDMRateImpl` return the candidate unchanged | FORCE/DEBUG | The cautionary example: "+10.6 % unanimous over 2 pairs" → **+5.2 %, p=0.73 at n=8**. Two usage notes: set it on the station holding rate authority; under the latent controller a forced rung also freezes the posterior's evidence stream, so it no longer measures the shipped system |

### ARQ, timeouts, and the ACK plane

| Knob | Default | file:line | What it does | Status | Evidence |
|---|---|---|---|---|---|
| `ULTRA_INFLIGHT_RTO` | OFF strict | `selective_repeat_arq_policy.hpp:287`; table `connection.cpp:5721-5747`; consumed `selective_repeat_arq.cpp:2274` | Sizes the ARQ RTO on frames outstanding instead of the window max | SUPERSEDED — **code fix, not a campaign** | n=2, "+65 % / −19 %, inconclusive" (`609fc71`). Premise is false on the live path: burst transport is unconditional (`connection.cpp:35`) and `prepareUnifiedBurstWindow` already re-derives a **frames-parameterised** timeout every burst (`connection.cpp:5998` → `connection_policy.hpp:1606`) — reproduced exactly against the rig's own `configured=17420 / 23144`. The table is drawn from the superseded windowed model and `min()`s against it, so at full window it arms **1.5 s shorter** than the burst deadline. It is also consulted **only on first transmission** — `retransmitFrame` re-arms from the scalar (`selective_repeat_arq.cpp:1676`, `:1330`), which is the NACK-driven tail the motivating 57 s observation came from. Real fix: re-issue `unifiedBurstAckTimeoutMs(submitted_this_call)` after the submit loop and route the retransmit re-arm through it |
| `ULTRA_ADAPTIVE_RTO` | OFF strict | `selective_repeat_arq_policy.hpp:255`; `:314-321`; call `selective_repeat_arq.cpp:2313` | Stops `configured_ack_timeout_ms` being the RFC6298 estimator **floor** as well as its ceiling (the legacy `clamp()` collapses to a point) | UNDER-TESTED, low | n=1 paired: 1.12 vs 1.50 kbps. Mechanism measured with a null control (53 `ADAPTIVE-RTO ENGAGED` firings): the adaptive floor moves the RTO by **0–20 %**, not the 7.5× the synthetic unit test implies, because measured srtt is 10.7–12.6 s. **Do not pair it with `ULTRA_INFLIGHT_RTO`**: where INFLIGHT acts (tail, table[1]=8.0 s) ADAPTIVE is inert (min(8.0, ~18)=8.0); where they interact, min(table[16]=47.7 s, ~20.5 s)=20.5 s sits **under the ~21.5 s burst airtime** — a guaranteed spurious mid-burst retransmit. Blast radius is wider than the RTO: it also scales the hole-probe timers (`:1301`,`:1462`), the DATA_REPAIR guard (`:1544`) and the post-fast-retx re-arm (`:1330`) |
| `ULTRA_KEEPALIVE_ACK` | OFF | `connection.cpp:5093` | Re-emit the cumulative tone ACK after `_MS` of silence | UNDER-TESTED → see [shortlist #4](#4-ultra_keepalive_ack--ultra_keepalive_ack_ms--connectioncpp5093--5104) | Reverted 2026-07-14 on a mechanism-level cause (re-emits re-assert the rung command) |
| `ULTRA_KEEPALIVE_ACK_MS` | **8000** ms, [3000,60000] | `connection.cpp:5104` | Silence threshold | sub-parameter | 8000 has a cap measurement with a two-point null control (36.7 → 25.0 → 8.0 s). No room below it: normal group cadence is 9-10 s. Comment at `:5096` says "default 25000" — **stale** |
| `ULTRA_ACK_REPEAT_SILENT_MS` | **4000 ms, DEFAULT-ON**; opt-out broken | `app.cpp:2914` | Tc-decorrelated repeat of a distinct tone ACK after an unbroken quiet window | UNDER-TESTED → see [shortlist #3](#3-ultra_ack_repeat_silent_ms--srcguiappcpp2914) | No goodput A/B at any n; six collision-repair patches |
| `ULTRA_ACK_CCA_DEFER_MS` | **2500 ms, DEFAULT-ON**, [0,10000] | `app.cpp:739` | Listen-before-ACK: defer a tone ACK while the channel reads busy; send on 3 consecutive quiet ticks or drop at the deadline on decoder evidence | UNDER-TESTED, **do not revisit as a throughput lever** | Enforces the half-duplex rule ("WE DO NOT TRANSMIT WHILE SIGNAL IS ARRIVING", `app.cpp:2962`). Four recorded failures were all the modem keying over inbound audio (F124/F127/F129/F176). Two accuracy notes: the comment claiming `=0` restores immediate-key is **wrong** since F176 (the condition is `(knob>0 && busy) \|\| air_rem>0`, so the geometry gate still defers unconditionally); and 2500 is a bare constant on a path whose every other timing term is ratiometric |
| `ULTRA_MC_ACK_REPEATS` | 0 = derived (3 on fading, 1 on AWGN), [1,3] | `connection.cpp:5447` | Pins the number of staggered MODE_CHANGE-ACK copies | NEVER-TESTED, low | No measurement. Motivating observation: up to 5 receptions per climb. Less urgent now — the latent controller cut mode changes to 2.2/run from 5.0. If a stall ever traces to a lost MODE_CHANGE ACK, measure repeats=1 vs 3 counting **round-trip completions**, not goodput |
| `ULTRA_MODE_CHANGE_RETRY_MS` | 0 = derived (~18.5 s wideband); [1000,60000] pins | `connection.cpp:5398` | Pins the MODE_CHANGE retransmit timer | FORCE/DEBUG | Worked as a bisect instrument: W4 recorded the defect (~74 s of a 328 s transfer in MODE_CHANGE dead air), W5 at ~5 s **livelocked** (72 receptions, no move committed), W5b at ~9 s stalled at 25 %, W6 pinned at 18500 ran clean (1.88 kbps). The fast-timer hypothesis was falsified. **`MAP:233` is stale** — it claims the default is the ~5 s ratiometric round trip; `connection.cpp:5425-5430` restored the `getAckTimeout()` floor |
| `ULTRA_SACK_SALVAGE` | OFF strict | `connection.cpp:379` | On a rate/CW-change abort, salvage the file byte ranges of discarded-but-SACKed slots so the requeue skips them (~1 s/transfer) | NEVER-TESTED, low | Disabled 2026-07-07 on one forensic incident (F181) whose attribution was later **refuted** (`KNOWN_BUGS.md:508-530`, `d5cbfb8`: the real destroyer was a non-monotone receiver byte map). But a second argument replaced it and holds: a sender-local decision to *permanently* never send a byte needs monotone, byte-domain, era-independent evidence, and `slot.acked` is frame-domain, era-relative and retractable. Gating it on trustworthy evidence makes it a no-op; receiver-authored byte evidence needs a wire bit and the tone ACK payload is saturated at 44/44 bits. Prize ~1 s; downside a stranded run |
| `ULTRA_CONNECT_ACK_RESCUE_DEFER` | OFF strict | `connection.cpp:4635` | Defer rather than destroy an armed CONNECT_ACK rescue on accepted data sync | MEASURED-HARMFUL → see [shortlist #5](#5-ultra_connect_ack_rescue_defer--srcprotocolconnectioncpp4635) | 3/3 deadlocks vs ~1-in-20 |
| `ULTRA_DROP_RX_SEQ` | −1 = off | `connection.cpp:4542` | One-shot: drop the first receipt of DATA seq=N so selective repeat can be proven on demand | FORCE/DEBUG — **keep** | The only deterministic single-hole injector. Right regression instrument for the SACK/durability family (BUG-SACK-DURABILITY-RESIDUAL, `d5cbfb8`) |
| `ULTRA_ACK_MONITOR_GAPLESS` | **DEFAULT-ON** (`=0` opts out) | `streaming_decoder.cpp:219` | Closes a blind hole in the tone-ACK tail sweep: all cadence passes in one `feedAudio(count)` scan the same end-anchored window, so a large append leaves a permanent gap | correctness fix, closed | BUG-POSTTX-ACK-MISS (`KNOWN_BUGS.md:800`). Two alternatives refuted by a capture ledger (fade ruled out — tone arrived at rms 0.033-0.091; bin mismatch ruled out — all ACKs symbol_ms=12). 10-run batch F78-F87, 0 misses |
| `ULTRA_ACKLISTEN_SUPPRESS_OFDM` | **DEFAULT-ON** (`=0` opts out) | `streaming_sync_acquisition.cpp:377` | While the ACK monitor is armed, suppress warm DATA-sync acceptance so the ACK tone cannot false-lock the OFDM searcher | correctness fix, closed | **Half-duplex-provably safe**: the peer physically cannot be sending OFDM in our ACK window. ~280 ACK exchanges, 0 misses. `KNOWN_BUGS.md:805` still says default-OFF — **stale** |
| `ULTRA_ECHO_REANCHOR_GATE` | **DEFAULT-ON** (`=0` opts out) | `streaming_ofdm_decode.cpp:2672` | While our ACK monitor is armed, suppress the full re-anchor a spurious 0-CW decode would force (which would re-arm a 120000-sample blind search and make us miss the crater-demote tone ACK) | closed, +17 % | Rig F65-F68 with clocks aligned. `KNOWN_BUGS.md:812`: the "own burst echo" attribution was **wrong** (it was the peer's tone ACK); the fix keyed on the right state and stands. See R7 |
| `ULTRA_BURST_ESCALATION` | **DEFAULT-ON** (`=0` opts out) | `connection.cpp:5882` | After 2 consecutive clean groups at a **dense** rung, raise the burst airtime ceiling 8600 → 11500 ms (N=8 frames) | shipped winner | F208-F217, alternating ON/OFF, same hour and channel: **1.74 vs 1.40 kbps = +24 %**, 4/5 pairs positive, 10/10 PASS. Dense-rung gate added after streak-alone over-escalated on Moderate (moderate@16 ON FAIL 690 vs OFF PASS 1030). Sibling `ULTRA_BURST_ESC_STREAK` is **harmful on Moderate** (31 craters vs 2) — keep off. **Missing from the map** |

### Burst transport, sync, and anchoring

| Knob | Default | file:line | What it does | Status | Evidence |
|---|---|---|---|---|---|
| `ULTRA_CRATER_REANCHOR_HOLD` | OFF strict | `streaming_ofdm_decode.cpp:2691` | Two-crater discipline for the receiver's sync re-anchor | UNDER-TESTED → [shortlist #1](#1-ultra_crater_reanchor_hold--srcguimodemstreaming_ofdm_decodecpp2691) | F470-481 +44 %, re-anchors 25 vs 50; revert confounded 3 ways |
| `ULTRA_CHEAP_REANCHOR` | OFF strict, **and inert unless HOLD=1** | `streaming_ofdm_decode.cpp:2709` | On a crater: hold warm timing, roll CFO back to the last certified value (`certifyWarm()` fires unconditionally at `streaming_burst_interleave.cpp:1398-1399`), full chirp only every 4th | NEVER-TESTED, low | **Trap:** setting it alone is a strict no-op — the dispatch at `:2714-2721` makes `force_chirp` unconditionally true when HOLD is off, and the rollback lives in the unreachable else-arm. Target is only ~1.2 s/run once HOLD lands (HOLD already cuts re-anchors to ~1/run), i.e. **1.3–1.7 % of transfer time** — an order of magnitude below rig resolution. Also: the documented "1 chirp per 4 craters" is **wrong** — the streak resets on any decoded frame, so it is "no chirp unless 4 **consecutive**", and riding warm through 3 craters injects up to 3 extra `k=0` observations into the latent posterior. Fix the doc (`CHANGELOG:3674`, `MAP:412`); do not spend rig time |
| `ULTRA_COMMANDED_GEOMETRY` | OFF since 2026-07-29 (`4055831`), loose, non-latching | `streaming_decode_policy.hpp:53`; readers `streaming_ofdm_decode.cpp:426` (B) and `:656` (A) | **(A)** truncation guard — never arm a group from a group-start frame that `std::min(frame_len, available)` shortened. **(B)** on a missed BURST_HEADER, slice with the rung *this* receiver commanded, gated by cadence + demote-only + not-declined | UNDER-TESTED; **(A) is a silently-disabled correctness guard** → R1 | (A): unit-pinned only (knob=0 → 0/3, knob=1 → 3/3, `trunc_holds=1`); **zero rig measurement in either direction**. (B): rig 8 interleaved transfers, `cmd_arms=0` in 7 of 8 → the +23 % arm-mean is a self-declared **null control**. Suppression cause known and unfixed: three `have_burst_descriptor_` stale-TRUE leaks, `MAP §7b:352-360`. Do not score a (B) A/B without `cmd_arms > 0` |
| `ULTRA_BURST_INTERLEAVE` | OFF for all mods | `connection_policy.hpp:239` | TX permutation + whole-group ACK semantics + wire flag | UNDER-TESTED → [shortlist #2](#2-ultra_burst_interleave--srcprotocolconnection_policyhpp239) | Two contradicting measurements at n=5-6 |
| `ULTRA_BURST_ERASURE_ABSOLUTE` | OFF | `streaming_burst_interleave.cpp:679` | Forces the legacy fixed 0.015 broadband-RMS erasure floor | FORCE/DEBUG — keep as the OFF arm | Only recorded use is ctest triage (UltraTncSimAudio fails identically with `=1`, proving the failure is unrelated) |
| `ULTRA_BURST_RMS_DIAG` | OFF | `streaming_burst_interleave.cpp:645` | Logs the erasure-gate inputs per collected frame | DIAGNOSTIC → [shortlist #7](#7-ultra_burst_rms_diag--ultra_burst_erasure_absolute) | Owed since 2026-06-16 (`KNOWN_BUGS.md:1279`) |
| `ULTRA_HARQ_PROVISIONAL` | OFF (opt-in) | `streaming_ofdm_decode.cpp:3756` | On CW0 header-peek failure, key the HARQ soft-combine buffer by the ARQ-mirror predicted seq so CW0-dead frames still accumulate LLR energy | UNDER-TESTED, **parked** | Shipped default-ON on a sim A/B (0/212 key mispredictions, combines 28 → 289/run), flipped to opt-in the same evening on **n=1** (one poison-loop). The named blocker **is structurally gone** — Phase F fresh-only rescue is unconditional (`frame_v2.cpp:2237-2280`) plus a provisional-key accumulator reset capping poison at one round. But it is gated to `getBitsPerSymbol >= 4` and the only selectable ≥4 bps rung is QAM16 R2/3, enabled solely in the Good column at an **extrapolated 26.0 dB** — so on the default ladder it cannot fire. Both prior measurements forced 16QAM, which measures a configuration deliberately not shipped (51.4 % FER on ITU Good). **Make it a dependent of the 16QAM unlock, not an independent rig candidate.** Two stale claims to fix now: `streaming_ofdm_decode.cpp:3752-3754` and `MAP:257` both still name the re-enable precondition as unmet. Any A/B must carry decoder `backlog_ms` and shed-seconds (BUG-DECODE-BACKLOG-COLLISIONS is open) |
| `ULTRA_ANCHOR_SKIP_KEEP_STREAK_ON_SWITCH` | OFF strict, documented permanent | `streaming_encoder.cpp:687` | Preserve the #69 reactive-anchor-skip clean streak across a descriptor mode/rate switch | MEASURED-HARMFUL (closed at n=2) | Stopped on a pre-committed falsifier: chirp SKIP rate **7.7 % ON vs 28.9 % OFF** — the knob's entire purpose was to raise it and it more than halved it. Craters 2.0 vs 1.0/run, resends 1.0 vs 0.0. The recool was doing double duty as a **post-switch acquisition guard**. A future attempt is a different design (explicit post-switch holdoff). **Keep** the `anchor_reason` plumbing (None/Resend/ModeSwitch) — it is what made this measurable |
| `ULTRA_BURST_HEADER_ONCE` | OFF | `modem_engine.cpp:582` | Emit the BURST_HEADER descriptor only on `group_seq == 0` | MEASURED-HARMFUL, **delete** | `PHY_ADAPTATION_DESIGN_2026_05_26.md §14.32`. The gate worked (1 header sent); BRAVO then reported every inner group as `group_seq=0` and re-ACKed seq 0 forever. The header does **three** jobs — format, chirp/sync, **and group sequence number** — and (3) is required every burst. The claimed saving is illusory anyway: the header's airtime is almost entirely chirp+LTS. **KEEP** `ULTRA_BURST_DESCRIPTOR` — the descriptor is the only RX source of the LDPC lifting Z |
| `ULTRA_WARM_TURNAROUND_OFF` | knob OFF ⇒ **feature DEFAULT-ON** | `modem_engine.cpp:1185` | Setting it to 1 **restores the slow behaviour** | FORCE/DEBUG — **do not set** | The feature is rig-proven on two classes: Good MPG@20 turnaround 2.71 → **1.54 s (−43 %)**, Moderate MPM@20 1.59 s with zero burst-timeout stalls. The old per-ACK echo-clear called `clearRxBuffer` → `reset()`, wiping warm-sync state every turnaround. The name reads like something you would switch on to *get* warm turnaround. Keep the knob — the faithful gate structurally cannot regression-test this (the sim TX returns before the echo-clear, `app.cpp:3040`), so it is the one-line bisect if a rig regression implicates turnaround |

### Channel estimation, LLR, and RX front end

The three closed nulls in this group (`ULTRA_LTS_DFT_DENOISE`, `ULTRA_CHEST_NOISE_SCALE`,
and the COH/MIN pair) are the reason several other estimator knobs rank low. Read
`CHANGELOG:1203-1265` before proposing anything on the channel-mean axis.

| Knob | Default | file:line | What it does | Status | Evidence |
|---|---|---|---|---|---|
| `ULTRA_LTS_DFT_DENOISE` | OFF | `ofdm_demodulator_setup.cpp:34` | Gaussian across-carrier smoothing of the LTS channel estimate | **MEASURED-WASH — the family null control** | Both halves measured. The estimate really improves: truth-referenced NMSE 0.02675 → 0.00660 = **4.05×** (QPSK R3/4), 3.97× (16QAM R2/3). That 4× buys nothing: ITU Good, 4 seeds × n=60 = **240 frames per cell**, eight cells across 11/14/20 dB, deltas +1/−1/0/+3/0/0/0/+2 against 1σ≈7.7. Low-SNR cells are exactly zero. Conclusion: **channel-MEAN estimation is not the throughput limiter**. Explicitly *not* refuted: estimator **uncertainty** semantics (`error_var` drives modelled uncertainty toward zero in a notch — a reliability-*weighting* lever, still unexplored) |
| `ULTRA_LTS_DFT_DENOISE_TAPS` | 0 → effective **W=2** | `ofdm_demodulator_setup.cpp:36`; used `channel_equalizer_lts.cpp:688-692` | Gaussian half-width W (also sets σ = max(0.5, W/1.5)) | UNDER-TESTED, none | Parent is a measured wash, so tuning its width cannot recover an absent benefit. **Free action:** reconcile the contradiction — `channel_equalizer_lts.cpp:667-669` says "harness optimum for Good is W~3, SNR-independent"; `:688`, 21 lines later, says "W=2 is the harness-proven SAFE width (W≥3 smears deep nulls → 16QAM poison)". Both cite the same harness; one is wrong |
| `ULTRA_CHEST_NOISE_SCALE` | OFF, non-latching | `channel_equalizer_lts.cpp:28` | Corrects a real **3.01 dB** scale error: `lts_noise_var = σ²_bin/2` (the averaged estimate's variance) is fed to the MMSE denominator, the LLR σ² and the erasure gate, all of which want per-symbol σ²_bin. The meter path already doubles; the decode path does not | MEASURED-WASH | ITU Good@20, 4 seeds × n=100 = **400 frames/cell**: QPSK R3/4 93.5→93.5, 8PSK R2/3 82.2→81.0, 16QAM R2/3 51.0→49.8, R3/4 27.8→26.8 — all inside σ≈2. Plus a four-arm discriminator (base / noise-fix / LTS-freeze / both) that refuted the mis-scaled-MMSE hypothesis. **Do not "clean up" the inconsistency** — it is kept default-OFF precisely so the meter-vs-decode 2× disagreement stays recorded |
| `ULTRA_ITERATIVE_CHEST` | OFF, non-latching | `iterative_chest.hpp:93`; armed `streaming_burst_interleave.cpp:747-752` | Post-FEC data-aided channel estimation carried **forward across frames** of a burst group: re-encode a fully-verified frame via the production path, push H_dd = Y/X into the existing Wiener | **MEASURED-WASH by family null** | Mechanism is sound and measured (2592 accepted observations/frame; re-modulation bit-exact over 576 configurations; Pi5 CPU 0.49-1.40 % of frame airtime; the inter-frame common phase is real, −75° to +23°). Benefit is not: the gui_qso A/B was 4 seeds of which **2 are unusable** to BUG-GUI-GATE-EARLY-EXIT-FLAKE → n=2, +4.9 %, inside noise, with an explicit "do NOT quote it as a win". It is a channel-mean lever, and `ULTRA_LTS_DFT_DENOISE`'s strictly larger 4× improvement bought exactly zero FER at n=240/cell. Upper bound: at ITU Good, ρ = J₀(2π·0.1·0.35) ≈ 0.988, so a one-frame-old *exact* observation still carries 1−ρ² ≈ 0.024 irreducible error ≈ the entire measured baseline NMSE. Removal scope with an anti-footgun KEEP list is already in `REMOVAL_BACKLOG.md:287` |
| `ULTRA_LTS_CFO_SEED_OFF` | OFF loose, non-latching | `channel_equalizer_lts.cpp:469` | Bypasses the seed/CV half of the residual-CFO gate | **contested** → [shortlist #6](#6-ultra_lts_cfo_seed_off--srcofdmchannel_equalizer_ltscpp469) | Moderate +31/320 (3–5σ) at n=80/cell; Good −7/320 consistently negative; goodput arm n=6 |
| `ULTRA_LTS_CFO_COH` | 0.70, (0,1] | `channel_equalizer_lts.cpp:450` | Coherence threshold for accepting the LTS residual CFO | **MEASURED-WASH (mechanism-level null control)** | Swept 0.70 / 0.85 / 0.93 → **identical results**. Designed to test "the harmful Good corrections are low-coherence garbage" and **refuted** it: they are coherent |
| `ULTRA_LTS_CFO_MIN` | 0.3 Hz, [0,5) | `channel_equalizer_lts.cpp:484` | Minimum residual magnitude worth applying | **MEASURED-WASH (mechanism-level null control)** | Swept 0.3 / 1.0 / 1.8 Hz → **identical results**. The harmful corrections are also **large**. Together with COH this is a positive finding: the Good cost is a genuine, coherent, large rotation whose correction still hurts → the successor is **persistence** weighting, not thresholding |
| `ULTRA_LTS_CFO_AVG` | OFF | `ofdm_demodulator_setup.cpp:39` | Phase-align the N LTS symbols to the last symbol's frame (ML common rotation) before averaging, giving ~−3 dB LS noise; also de-biases the LTS noise estimate on the same alignment | NEVER-TESTED, none | Estimator-level only ("~−3 dB across SNR 8/12/16/20", `tools/lts_estimate_mse.cpp:255-279`). Both of its axes are covered by larger measured nulls: the −3 dB channel-mean axis by DFT_DENOISE's 4×, the σ² axis by CHEST_NOISE_SCALE's full 2× at n=400. And on Good the true offset is ~0, so the de-bias term |H|²(2−2cos φ) vanishes by mechanism. On the rig the *refine* is more likely to fire (chirp CFO ≥0.75 Hz becomes true with ppm offset), which re-estimates from scratch and erases the knob's rationale. `MAP:293` still parks it as "FEAT (task #9, in-flight)" since 2026-05-30 |
| `ULTRA_REL_FADE_MAX` (+`_ONSET` 0.25) | 30.0 = 15 dB cap, [1,1000] | `channel_equalizer_equalize.cpp:112` | Caps the LLR noise-variance inflation applied to carriers in relative fades (inflation = onset/rel, clamped) | NEVER-TESTED, low | No measurement. Both constants were tuned for the **dense 12-pilot** baseline and made env-tunable for a **sparse-pilot** experiment that may never have run — production runs pilots ~every 10 carriers with per-symbol LS tracking, so the premise may not apply. Also: LLR calibration is a recorded **exhausted** lever (≤1.21×, double-counts `softGrayZoneNoiseInflation` immediately below in the same file). If reopened, run `ULTRA_NULL_DIAG` first and read the per-depth-bin reliability — measure, then tune |
| `ULTRA_RX_AGC` | OFF, latching | `streaming_decoder.cpp:427` | Slow, amplify-only, deadbanded RX level normalizer on the ring path (after the ACK monitor, before sync) | NEVER-TESTED → **redesign or delete** | Two *controls* were run, the benefit never was: a no-op control (byte-identical, 0 engagements) and an engagement control at a forced low level (+13.6 dB). Since then two of its three targets were fixed elsewhere (relative erasure gate; `ULTRA_SOFTWARE_ALC` default-ON), and the third — CCA — **was never on its path**: `modem_rx.cpp:176/181-183` hands the raw buffer to the detector in parallel, and the CCA has been ratiometric since 2026-05-23. **Its own trigger is mis-sized:** the EMA only updates above `chunk_rms > 0.030` and engagement needs `level_est < 0.0587`, a 5.8 dB window — while the motivating IONOS frames sat at **0.0038-0.0145**, two to eight times *below* the activity gate. The residual real defect is elsewhere and needs no knob: the sync gate's **absolute clamp bounds** (`sync_controller.cpp:579` lower 0.006, `:593` `clamp(nf*2.2, 0.015, 0.040)`) should be derived from the measured operating level. Fix the false CCA claim at `streaming_decoder.hpp:930` and `MAP:247` |
| `ULTRA_LDPC_MAX_ITERS` | unset = per-rate table (R3/4 60, R2/3 70, R1/2 80, R1/3 60, R1/4 50); [10,1000] | `ldpc_codec.hpp:98` | Overrides the BP iteration cap for all rates | NEVER-TESTED, low → **R6** | The "21 of 26 failures at exactly 70" signature is a tautology (one non-cap exit, and it is success). A strictly stronger experiment is already default-on: `frame_v2.cpp:2167-2270` runs up to 12 extra full BP passes (4 factor + 5 perturbation + 3 fresh) "to break trapping sets from multiple angles", and logs per-CW `RETRY OK (factor=…, iters=…)`. The ~1000× headroom is measured on the *clean* population (median 4 iters); on the affected population it is ~4×, and `frame_v2.cpp:2206-2211` already records a measured backlog regression from too many retries ("200 ms per failed frame → 5-10 s audio backlog → sync degradation"). Downside is not bounded at CPU: `kIterRef=80` is hardcoded at `streaming_burst_interleave.cpp:1377-1380`, so a *successful* group above 80 iterations grades **quality = 0.0**. `CALIBRATION_AUDIT.md:861-864` already says "raising the iteration ceiling is not supported by this evidence" |

### TX PAPR and level

| Knob | Default | file:line | What it does | Status | Evidence |
|---|---|---|---|---|---|
| `ULTRA_ACE_PAPR` | OFF loose, **latching** | `modulator.cpp:285` | Active Constellation Extension on data symbols | UNDER-TESTED → [shortlist #8](#8-ultra_ace_papr--srcofdmmodulatorcpp285) | Unit-proven; rig n=3 inconclusive; CPU measured negligible; gain ceiling ~1.06 dB; collides with default-ON ALC clip detection |
| `ULTRA_COHERENT_PAPR_DB` | 0.0 = OFF; [4,12] sets the soft-clip threshold | `streaming_encoder.cpp:86` | Bounded soft-clip of the coherent TX waveform; also flips `paprReductionWouldCorruptCoherentPilots` | MEASURED-HARMFUL, **do not re-test in this form** | Sim: 9 dB target → 181 deinterleave-fails vs 30 linear; 12 dB → goodput 2210 → 1320. Rig (interleaved, MPG@30): **channel-state-dependent — +39 % marginal, −58 % clean, net −12 %**. Both signs have a named mechanism (clipping buys power where power binds; buys EVM where EVM binds), which is what legitimately closes it. The 4.5 dB power gain is real and **hardware-only**. Successor filed: an **SNR-gated** clip firing only when RX usable is within ~2 dB of the constellation floor — needs peer-margin plumbing that does not exist |
| `ULTRA_SIM_PAPR_PENALTY` (+`ULTRA_SIM_TX_PEAK`) | OFF, latching | `app.cpp:3625` / `:3633` | Make the sim TX peak-normalize like hardware | FORCE/DEBUG → [shortlist #9](#9-ultra_sim_papr_penalty--srcguiappcpp3625--ultra_sim_tx_peak-3633) | Confirmed its own mechanism and **disproved** the simple story it was built for (even ON, the sim does not reproduce the IONOS stall — the missing deficit was the cheap-card anchor-to-data gap, which OTASim does not model) |

### Simulator and offline harness

| Knob | Default | file:line | What it does | Status | Notes |
|---|---|---|---|---|---|
| `ULTRA_CHANNEL_DELAY_MS` (+`ULTRA_CHANNEL_DOPPLER_HZ`) | unset = model default; **presence-only, `=0` also sets `multipath_enabled=false`** | `ota_channel_core/channel.cpp:184` | Overrides the sim delay spread | DIAGNOSTIC / test apparatus | `DELAY_MS=0 DOPPLER_HZ=0` gives **truth H = 1 at every carrier** — an identity control. If an estimate-vs-truth comparison is still wrong under it, the fault is in the **tool**. Worth documenting prominently: this project has already had an analyzer that manufactured a 2.5 s latency figure from two hardcoded constants |
| `ULTRA_MEASURE_BURST_FULL_ANCHOR` | OFF | `tools/measure_ack_fer.cpp:1172` | Forces a full chirp+LTS anchor at each burst chunk's group start | FORCE/DEBUG — **do not promote to default** | It reproduces production's **resend / session-first** anchor regime, not the shipped §16.8 warm handoff (production sends a full-anchor BURST_HEADER *descriptor* then a **light-LTS** group start; `streaming_encoder.cpp:620-626`, `streaming_ofdm_decode.cpp:1268-1311`). Forcing it every chunk would emit two full anchors back to back in descriptor-on configs and would pin the anchor-skip clean streak at 0 forever. The harness's real fidelity defect is its **descriptor-off default** (`args.burst_descriptor=false`, `:65`) — fix that instead. The 0-frames-recovered symptom was a missing `burst_group_callback_`, fixed unconditionally at `:1201` |
| `ULTRA_MEASURE_BURST_NO_ANCHOR_WAIT` | OFF | `tools/measure_ack_fer.cpp:1102` | Skips the harness's `expectFullOFDMAnchorOnce()` demand so a light-LTS group start (corr ~0.26 vs the 0.52 threshold) is accepted | FORCE/DEBUG | The RX-side alternative to the above. Use to isolate "acquisition-threshold artifact" from "geometry/decode problem" |
| `ULTRA_CHEST_DELAY_SCAN` | OFF, **presence-only (`=0` enables)** | `tools/measure_ack_fer.cpp:752` | Prints residual-vs-delay over ±2560 samples in 64-sample steps | DIAGNOSTIC | The production fine search (±1200 at 0.5-sample steps) runs regardless. The coarse scan is how you would detect the fine search converging to a local minimum — its window is **narrower** than the coarse one. Hygiene: normalise the enable test |
| `ULTRA_CHEST_NMSE_DUMP` | OFF, presence-only | `tools/measure_ack_fer.cpp:689` | First frame only: dumps estimate-array geometry plus \|est\| at **deliberately unoccupied** bins (0, 100, 500, 900) and adjacent carriers | DIAGNOSTIC | A well-built sanity check: energy at an unoccupied bin means the array is not what the caller believes. Guards the same class of assumption error that produced a real stale-geometry mistake (12.67 ms/symbol from the 512-FFT spec on a 1024/128 modem — actual 24.0 ms) |
| `ULTRA_LOG_LEVEL` | unset | `tools/measure_ack_fer.cpp:1275` | Harness verbosity | DIAGNOSTIC | Related and actionable elsewhere: `tools/gui_qso_scenario.sh:497,507` **hardcodes** `--log-level debug --log-category all`, BRAVO emits ~11700 DEMOD lines/run, and that throttles the real-time sim below wall-clock pace. Identical in both arms so A/B validity holds — but it caps seeds per session, which is a direct mechanical contributor to why several knobs here sit at n=3-6 |

### TNC / host interface

| Knob | Default | file:line | What it does | Status | Notes |
|---|---|---|---|---|---|
| `ULTRA_TNC_ACCUM_DISABLE` | **accumulation ON**; `getenv(...) == nullptr` ⇒ **any value, incl. `0`, disables** | `tnc_session.cpp:177` | Opts out of flush batching, reverting to legacy per-chunk flush; also clears the sticky-transport ordering latch | correct by first principles; **hazard** | Default-ON is right on the half-duplex constraint (fewer turnarounds), verified functionally (20 KB JPEG byte-identical, clean teardown). **Hazard:** clearing the sticky latch while `route_burst` (`:517-518`) still bursts any single flush > 4096 B means a host that writes one >4 KB block then a smaller tail reassembles **out of order** against the bursted prefix — the exact corruption the ordering invariant at `tnc_session.hpp:104-108` exists to prevent |
| `ULTRA_TNC_ACCUM_PROBE` | OFF, presence-only | `tnc_session.cpp:178` | Measures accumulated body size per flush — and **forces `bulk_accum_ = true`**, then `std::_Exit(0)` at the first burst-routed flush, before any air TX | FORCE/DEBUG, **not diagnostic** | It **silently overrides `ULTRA_TNC_ACCUM_DISABLE`**. Anyone setting both — natural when A/B-ing accumulation while measuring body size — gets bulk mode in *both* arms and a null result that reads as "accumulation does not matter". Never leave it in a rig launch script |
| `ULTRA_TNC_FLUSH_LOG` | OFF, presence-only | `tnc_session.cpp:179` | Per-flush logging (why each body flushed: burst-worth vs host idle) | DIAGNOSTIC | The true no-side-effect observer for the live path, and how you verify batching is not degenerating to per-chunk because the host trickles |
| `ULTRA_EXPERT_PHY` | OFF | `tools/ultra_tnc_config.cpp:413` | Env alias for `--expert`, applied before argv parsing so an explicit flag composes | FORCE/DEBUG | Exists because the TNC runs headless (service, or launched by Winlink/PAT) where injecting argv is awkward. Document next to whatever `--expert` gates |

---

## Superseded / deletion candidates

These are cleanup, not options. Every one sits downstream of a default-ON bypass, or its
only reader is gone. Route them through `docs/REMOVAL_BACKLOG.md`.

| Knob | file:line | Why it is dead | Anti-footgun: what must NOT be over-cut |
|---|---|---|---|
| `ULTRA_TRUST_LADDER_PICK` | `connection.cpp:3048` | ~400 lines **after** the latent early return at `:2596`. Its hypothesis *won* — the latent controller shipped the same thesis (stability in the estimator, not the actuator) at +14.5 %, p=0.022, vs this arm's +20.7 % (n=6, sign p=0.69) | The F145 lesson: **never do raw arithmetic on rung indices** — disabled anchor rows are holes; snap via `snapRungIndexDownToEnabled`. Keep the enabled-ladder snap and catastrophic-crater escape as the audited-minimum safety set |
| `ULTRA_RUNG_DWELL_MS` | `connection.cpp:3113` | Unreachable (after `:2596`); measured **−13.8 %**; and its premise was refuted — the −0.58 correlation was **reverse causation** | The physics note is correct and worth keeping: the loop runs *inside* the coherence time (MPG Tc ≈ 4.2 s vs an 8-10 s burst), and "crater ⇒ rung too high" is a category error on Rayleigh. 8PSK R2/3 wins on Good@20 **including** its crater rate |
| `ULTRA_RUNG_CLASS_ANCHOR` | `waveform_selection.hpp:597` | **Dead code** — sole reader `rungPredictedSustainable()` has no production caller since the predictive climb was retired (`connection.cpp:2816`); only `tests/test_rx_authority.cpp` calls it. Also measured harmful with a structural cause: the Good-vs-AWGN anchor gaps are non-uniform (QPSK R3/4 +5 dB, 8PSK R2/3 +1 dB) and the climb scans highest-first, so it **inverted the ladder ordering** (12-CW frame usage tripled, 15 → 48) | `rungClassAnchorDb` itself has another caller at `connection.cpp:2770`. And record the finding underneath: the anchor table's Good column claims 8PSK R2/3 holds at 17.0 while QPSK R3/4 needs 20.0 — the two columns are **different kinds of quantity** (throughput crossover vs FER floor) and must never be mixed |
| `ULTRA_FER_FLOOR_ANCHOR` | `waveform_selection.hpp:610` | Same dead reader. Unlucky: it was the *correct* successor, built to dodge the inversion, and its consumer was retired ~2 days later. Never measured | **KEEP `kMeasuredFerFloor` / `rungFerFloorDb`** (`waveform_selection.hpp:538-564`): real data, 9 rungs × 3 classes × SNR 6-28 step 2 × seeds {7,11} × n=40 = **n=80/point**, pinned by `tests/test_waveform_policy.cpp:480`. Includes the honest finding that Moderate has an SNR-independent FER floor. It is the measurement to fit a per-class θ_r from |
| `ULTRA_DENSE_FAST_DEMOTE` | `connection.cpp:77` | After `:2596`; **and** the asymmetry it countered is gone — its whole justification was unwinding the predictive climb's multi-rung jumps (measured idx 3→8), and that climb was retired. A one-rung-up/one-rung-down loop does not have this failure mode; enabling it would only weaken the F122 two-crater grace | The F122 grace itself: a single crater at a ~10 s decision quantum against Tc 2-4 s is an irreducible deep null, not rate evidence |
| `ULTRA_RX_EMA_HOLD` | `connection.cpp:58` | Half (the censored ring feed, `:2500`) still executes but the ring is **never read** by the latent controller (which logs "NO SNR CONSUMED"); half (the hold gate, `:2760`/`:2893`) is after the return. Measured twice at 6 pairs with opposite verdicts (+8.5 % floor-improving vs aggregate wash with 2/6 failures = over-holding a failing rung) | The censoring **insight** is correct and was independently vindicated — a failed group feeds no fresh SNR so the ring reads a stale crest (26-32 dB on a 20 dB channel). The latent controller solves it structurally by fitting to outcomes, so no censoring rule is needed |
| `ULTRA_EVM_DEMOTE` | `connection.cpp:94` (clamp `:2934-3006`) | After `:2596`. Measured harmful with a mechanism-level root cause: it **doubled** the churn it existed to remove (15 vs 7 rung commands/run), because the ladder steers on dial-equivalent SNR while the clamp reads usable dB — measured mean disagreement **14.3 dB**, an irreconcilable second driver (the F128 pattern) | **Keep the estimator and the floor table.** The plumbing is already unconditional (`setBurstEvmObservation` fires whenever `modem.hasLastEvmSnr()`, `modem_protocol_binding.hpp:139-145`), so `burst_obs_evm_snr_db_` is free to any future consumer. The forward path is EVM as an **input** (e.g. the latent prior seed), never an output clamp. The 2026-07-24 "+21 % on 3 pairs" note is **superseded — do not re-cite** |
| `ULTRA_EVM_DEMOTE_CONFIDENT` | `connection.cpp:2965` | Inside the above block. Measured harmful at n=2 **with a null control that proved the gate worked as designed** (8.0 firings/run vs 0) and the outcome still degraded — churn 17.0 vs 7.5 | **Keep the form in the record:** `mean + 1.645·se < floor − margin` over a ring is correct for *any* rung decision fed by a σ≈3 dB estimator against 1.2-3.1 dB spacing; z=1.645 is a probability constant, same family as −ln(0.05) and ln(9). Reuse it if the tie-break probe is ever gated on confidence. Keep `tests/test_rx_authority.cpp:616-657` |
| `ULTRA_LINEAR_SNR_RING` | `connection_policy.hpp:92`; ring `connection.cpp:2519-2534` | Decision-inert: the ring executes but `snr_avg` is unread by the latent controller, and `ofdmAnchorScaleOffsetDb()` now has exactly one production consumer (`modem_protocol_binding.hpp:129-138`) whose value is used only as a freshness gate. Measured wash: 4 pairs, −2.5 %, sign p=0.688, with sizing independently confirmed neutral (ladder input 23.7 vs 23.6) | **Keep the code and the KNOWN_BUGS entry.** The underlying defect is real and large: per-frame dB-mean 13.89 vs power-mean 16.42 over 1023 readings = **1.74 dB**, the single largest identified term in the ~5.4 dB sim-vs-rig discrepancy against a measured analog loss of 0.36 dB. Any future SNR measurement — the §3 anchor re-measure, a per-class θ_r fit — **must be run in the linear-power domain** or it bakes the Jensen error into the new table |
| `ULTRA_GOODPUT_RATE` | `goodput_rate_controller.hpp:91`; path `connection.cpp:2664-2720` | Its early return sits **below** the latent one, so it needs `ULTRA_LATENT_RATE=0` to run. Same thesis, better implementation shipped: n=3, +8.0 % mean, spread −24 % to +43 %, self-described "NOT a result" | Two findings to salvage into notes: (a) its demote rule credits the rung below with f=1, inherited from a helper whose comment calls that "deliberately conservative" — right for crater regrading, a systematic downward bias as a primary rule; (b) **η(16QAM R1/2) = 2.000 and η(8PSK R2/3) = 2.001** — 16QAM R1/2 is a **dominated rung**, buying zero throughput for a denser constellation and worse PAPR (pinned by `test_dominated_rung_is_always_left`). That fact outlives any controller |
| `ULTRA_QAM16_CALM_FADING` | `connection.cpp:4010` | On the **sender's** `applyAdaptiveRateFeedback`, whose sole production call site is the `else` arm at `connection.cpp:2000-2013` — dead since `rxRateAuthorityEnabled()` went default-ON on 2026-07-05, **~26 hours after the knob was written**. It also gates the *wrong* hop: with the PSK8 ladder default-ON the climb target is QAM8 R2/3, not QAM16, and the QAM8→QAM16 step returns before `:4010`. The over-commit it would block was fixed upstream in the rung table (QAM16 R2/3 Good anchor 20.0 → 26.0, measured 51.4 % FER) | The whole sender-side dense-climb block dies together (`qam16_clean_streak_`, `qam16_r34_clean_streak_`, `qam16_reclimb_cooldown_`, `ULTRA_QAM16_CLIMB`, `ULTRA_QAM16_R34`, `ULTRA_R34_CALM_FADING`, `ULTRA_MAX_OFDM_RATE`'s `:3939` site). **KEEP:** the ACK-silence escapes (collapse escape, stuck-frame escape, RTO machinery) are not in this arm; `last_group_quality_` (`:3706`) still feeds the GUI "Adapt:" bar. **Sequence after** the `ULTRA_LATENT_RATE=0` fallback is dropped in 0.5.2 |
| `ULTRA_UNIFIED_SEQ` | comment-only: `connection.cpp:5816`, `streaming_burst_interleave.cpp:242`, `modem_engine.cpp:530` | **The getenv is gone.** `kUnifiedSeqEnabled()` returns `true`; ~1000 lines of `BurstStopAndWaitController` were deleted | Rewrite the three comments without the dead knob name — a reader grepping it today concludes the unified path is optional. **Preserve** the fast-NACK note at `streaming_burst_interleave.cpp:242` and the burst-sizing note at `modem_engine.cpp:530` |
| `ULTRA_TONE_ACK_INTERACTIVE` | comment-only: `connection.cpp:401` | Same merge, same commit | The merge had to explicitly re-wire `applyAdaptiveRateFeedback` into the unified ACK branch. That is the same now-dead sender arm above — do not treat the comment as evidence the path is live |
| `ULTRA_WARM_TURNAROUND` | not a knob | Prefix false positive; the real knob is `ULTRA_WARM_TURNAROUND_OFF` (`modem_engine.cpp:1185`) | **Methodology note:** a plain substring diff of knob names against the map produces prefix false positives. Of the 61 "undocumented" knobs in the audit set, **seven are not env knobs at all** — this one, `ULTRA_HAVE_LIBHAMLIB`, `ULTRA_OTASIM_AUDIO_DIAGNOSTICS`, `ULTRA_TNC_TESTING` (compile-time `#ifdef`/CMake), `ULTRA_PHY_DIAG_LOG` (a log-format name in a Python tool's help text), plus `ULTRA_UNIFIED_SEQ` and `ULTRA_TONE_ACK_INTERACTIVE` above. True count: **54** |

---

## Diagnostics

Pure logging/instrumentation. No behaviour change; useful when debugging, never shipping
candidates. Listed so the map gap can be closed.

| Knob | Default | file:line | What it shows | Why keep it |
|---|---|---|---|---|
| `ULTRA_NULL_DIAG` | OFF loose | `channel_equalizer_equalize.cpp:593` | Per-relative-depth-bin reliability: count, errors, thermal noise, eps_H contribution, total | The right instrument for the only open question about `ULTRA_REL_FADE_MAX/_ONSET` — do carriers in relative nulls actually get high noise variance, or stay overconfident? **Run before tuning** |
| `ULTRA_PSYM_DIAG` | OFF loose | `channel_equalizer_pilot.cpp:615` | Per-symbol carrier-averaged \|H\| at symbol cadence — \|H\|(t) for within-frame coherence | Frame boundaries recovered by interleaving with COH-DIAG in the same log. **Trap:** two different `coh=` fields exist; filter to the RX-AUTHORITY verdict line |
| `ULTRA_COH_DIAG` | OFF loose | `streaming_sync_acquisition.cpp:178` | Per-frame Doppler-discriminator inputs (snap, h_mag, lts_noise_var, lts_sig_pow), **only when a snapshot is admitted** | A **starvation detector**. The 2026-07-25 bug — 0 snapshots on the burst path, `coh=0.00` on 157/157 verdicts, every rate decision falling back to the blind CV — would appear here as silence. Easy to reintroduce (any change to the admission gate), hard to notice, because a starved discriminator does not error. Related durable fact: the across-carrier fading index converges to ~0.523 for **any** Rayleigh channel, so it is blind by construction as a class discriminator |
| `ULTRA_RX_LAG_DIAG` | OFF strict | `streaming_burst_interleave.cpp:591`, `streaming_decoder.cpp:622` | `[BURST_DRAIN]`: next_available ms, ratio to `burst_min_block_`, frame index | Encodes a **decision rule**, not just numbers: ratio ≥2 ⇒ the frames already arrived and per-wake serialization is the lag (drain is a lever); ratio ~1 while waiting ⇒ inherent airtime, no drain helps. One run answers it. RX lag is not cosmetic — running behind live **defeats warm-sync** |
| `ULTRA_GAMMA_DOMAIN_LOG` | OFF, **presence-only (`=0` enables)** | `streaming_burst_interleave.cpp:512` | Per-carrier gamma before/after in-band renormalization | **Last surviving reader** of a vector whose real consumer (the predictive climb) is gone; the +8.70 dB markup here was deleted 2026-08-01. Consolidation candidate — but **sequence after** the legacy ladder deletion, since the constant still lives on the `ULTRA_LATENT_RATE=0` path |
| `ULTRA_HARQ_DEBUG_SEQ` / `_CW` | −1 = no filter | `soft_combine.cpp:35` / `:39`; also `frame_v2.cpp:80` / `:84` | Filters the HARQ soft-combine debug stream to one seq and/or codeword (AND'ed) | Inert unless `harqDebugLogEnabled`. The only way to read HARQ logs at burst volume |
| `ULTRA_DUMP_CFO_PREFIX` / `_CALLS` | unset = off / 1 dump | `channel_equalizer_baseband.cpp:33` / `:37` | Dumps CFO-correction baseband buffers to disk; call cap enforced by atomic CAS | CFO handling has an explicit invariant set (`docs/CFO_CORRECTION_FLOW.md`); capturing the actual baseband a correction was applied to has standing forensic value |
| `ULTRA_QAM16_GENIE_TIMING_CFO` | OFF loose | `streaming_ofdm_decode.cpp:102`, `streaming_sync_acquisition.cpp:40` | Injects **true** timing offset and CFO | Attribution oracle |
| `ULTRA_QAM16_GENIE_CHANNEL_TWOPATH_LS` | OFF loose | `ofdm_stream_processor.cpp:79` | Substitutes an ideal two-path LS channel estimate | Attribution oracle |
| `ULTRA_QAM16_GENIE_SIGMA_EMPIRICAL` | OFF loose | `ofdm_stream_processor.cpp:75` | Uses the empirical per-symbol noise σ for LLR scaling | Attribution oracle. Its subject is already bounded: LLR calibration is an exhausted lever (≤1.21×), so a large gap here means the gap is **not** in σ estimation |
| `ULTRA_QAM16_GENIE_CHANNEL_DELAY_SAMPLES` | 24 (= 0.5 ms = ITU Good), [0,512] | `channel_equalizer_pilot.cpp:77` | Two-path delay assumed by the oracle | **Set 48 on a Moderate capture** (1 ms) or the oracle silently assumes the wrong channel and its "truth" is wrong |
| `ULTRA_GUI_OPERATOR_LOG_SUPPRESS` / `_LOG_SLOW_MS` / `_EVENT_DRAIN_LIMIT` / `_EVENT_QUEUE_LIMIT` | OFF / 0 / 128 / `MAX_OPERATOR_EVENTS` | `app.cpp:413`, `:414`, `:416`, `:418` | GUI operator-log suppression, slow-write threshold, per-pass drain cap, queue depth (oldest dropped at capacity, `:1741`) | Cannot influence any modem decision or wire behaviour. These four use the **cleanest env parsing in the codebase** (`envFlagEnabled` accepts `0/false/FALSE/off/OFF`; `envUInt` has a `strtol` validity check and a 1e6 clamp) — standardise on them if the knob surface is ever normalised |

**Genie classification note:** the `*_GENIE_*` knobs are neither ACTIVE nor DEAD. They feed
the decoder information no receiver can have, so they can never ship — but misfiling them
as DEAD would delete the only apparatus that can partition a failure into estimation error
vs channel outage. File them as a distinct **EXPERIMENTAL/TOOL** category. Relevant
history: three attempts to build a perfect-CSI oracle failed, and an existing default-off
knob answered the same question in 20 minutes. **This family is that already-built
apparatus** — exhaust it before constructing a new oracle.

---

## Not in the infrastructure map

`docs/MODEM_INFRASTRUCTURE_MAP.md` is mandatory-live per CLAUDE.md, and a wrong or missing
entry causes a wrong deletion later. The list below was produced by grepping each knob name
against the map (`grep -c`, zero hits), not by substring diff.

**44 knobs with a real getenv site are absent.** In rough priority order:

**Critical — default-ON behaviour, or the default rate path:**

| Knob | file:line | Why it matters |
|---|---|---|
| `ULTRA_LATENT_RATE` | `latent_rate_controller.hpp:104` | **The default rate controller since 2026-08-01.** Zero map hits |
| `ULTRA_LATENT_RELAX_DB` | `connection.cpp:2627` | Its one free parameter |
| `ULTRA_BURST_ESCALATION` | `connection.cpp:5882` | Default-ON, +24 % rig, with a known Moderate failure mode |
| `ULTRA_ECHO_REANCHOR_GATE` | `streaming_ofdm_decode.cpp:2672` | Default-ON, +17 % rig |
| `ULTRA_ACK_MONITOR_GAPLESS` | `streaming_decoder.cpp:219` | Default-ON correctness fix |
| `ULTRA_ACKLISTEN_SUPPRESS_OFDM` | `streaming_sync_acquisition.cpp:377` | Default-ON; `KNOWN_BUGS.md:805` additionally claims default-OFF |
| `ULTRA_TNC_ACCUM_DISABLE` | `tnc_session.cpp:177` | Governs the Winlink/PAT data path, with inverted semantics |

**Rate / ARQ knobs (mostly deletion candidates, but the map is the deletion reference):**
`ULTRA_TRUST_LADDER_PICK` (`connection.cpp:3048`), `ULTRA_RUNG_DWELL_MS` (`:3113`),
`ULTRA_RUNG_CLASS_ANCHOR` (`waveform_selection.hpp:597`), `ULTRA_FER_FLOOR_ANCHOR`
(`:610`), `ULTRA_QAM16_CALM_FADING` (`connection.cpp:4010`), `ULTRA_ENTRY_QAM16_SNR`
(`waveform_selection.hpp:937`), `ULTRA_FORCE_DATA_RATE` (`waveform_selection.hpp:971`),
`ULTRA_INFLIGHT_RTO` (`selective_repeat_arq_policy.hpp:287`), `ULTRA_KEEPALIVE_ACK` /
`_MS` (`connection.cpp:5093` / `:5104`), `ULTRA_MC_ACK_REPEATS` (`connection.cpp:5447`),
`ULTRA_CONNECT_ACK_RESCUE_DEFER` (`connection.cpp:4635`), `ULTRA_DROP_RX_SEQ`
(`connection.cpp:4542`).

**PHY / estimator:** `ULTRA_LTS_CFO_SEED_OFF` (`channel_equalizer_lts.cpp:469`),
`ULTRA_LTS_CFO_COH` (`:450`), `ULTRA_LTS_CFO_MIN` (`:484`), `ULTRA_LTS_DFT_DENOISE_TAPS`
(`ofdm_demodulator_setup.cpp:36`), `ULTRA_REL_FADE_MAX` (`channel_equalizer_equalize.cpp:112`),
`ULTRA_LDPC_MAX_ITERS` (`ldpc_codec.hpp:98`), `ULTRA_ACE_PAPR` (`modulator.cpp:285`),
`ULTRA_COHERENT_PAPR_DB` (`streaming_encoder.cpp:86`).

**Diagnostics and tools:** `ULTRA_NULL_DIAG`, `ULTRA_PSYM_DIAG`, `ULTRA_COH_DIAG`,
`ULTRA_RX_LAG_DIAG`, `ULTRA_GAMMA_DOMAIN_LOG`, `ULTRA_HARQ_DEBUG_SEQ`,
`ULTRA_DUMP_CFO_PREFIX`, `ULTRA_QAM16_GENIE_TIMING_CFO`, `ULTRA_CHANNEL_DELAY_MS`,
`ULTRA_CHEST_DELAY_SCAN`, `ULTRA_CHEST_NMSE_DUMP`, `ULTRA_MEASURE_BURST_FULL_ANCHOR`,
`ULTRA_MEASURE_BURST_NO_ANCHOR_WAIT`, `ULTRA_TNC_ACCUM_PROBE`, `ULTRA_TNC_FLUSH_LOG`,
`ULTRA_EXPERT_PHY`.

**Stale rows already in the map (fix, do not add):**

| Location | Says | Actually |
|---|---|---|
| `MAP:57,58,426` + `KNOWN_BUGS.md:147,278` | `ULTRA_COMMANDED_GEOMETRY` DEFAULT-ON | DEFAULT-OFF since `4055831` |
| `MAP:233` | `ULTRA_MODE_CHANGE_RETRY_MS` default ~5 s ratiometric, "replaces the `getAckTimeout()` borrow" | The `getAckTimeout()` floor was **restored** after the W5/W5b/W6 bisect — ~18.5 s |
| `MAP:262` | `ULTRA_TNC_BULK_ACCUM` live | **Deleted**; superseded by `ULTRA_TNC_ACCUM_DISABLE` |
| `MAP:267` | `ULTRA_MAX_OFDM_RATE` at `connection.cpp:673`, one site | Four sites (`connection.cpp:679`, `:3939`; `connection_handlers.cpp:415`, `:607`) |
| `MAP:247` + `streaming_decoder.hpp:930` | `ULTRA_RX_AGC` makes the CCA quiet gate gain-independent | The CCA is on a **parallel raw-audio path** (`modem_rx.cpp:176,181-183`) and has been ratiometric since 2026-05-23 — the AGC cannot reach it |
| `MAP:257` + `streaming_ofdm_decode.cpp:3752-3754` | `ULTRA_HARQ_PROVISIONAL` re-enable precondition unmet | The fresh-only rescue **shipped** (`frame_v2.cpp:2237-2280`) |
| `MAP:291` + `iterative_chest.hpp:49` | `ULTRA_ITERATIVE_CHEST` "MEASURED NULL" | Accurate **but imprecise** — null by the *family* control (`ULTRA_LTS_DFT_DENOISE`, n=240/cell), not by a direct A/B of this knob. Say which |
| `MAP:293` | `ULTRA_LTS_CFO_AVG` "FEAT (task #9, in-flight)" | Parked since 2026-05-30; closed by mechanism-level null controls |
| `MAP:93` | PAPR "skipped for all coherent mods → inert on data path" | Stale since `ULTRA_COHERENT_PAPR_DB` began self-enabling (`streaming_encoder.cpp:155-165`) |
| `MAP:412` + `CHANGELOG:3674` | `ULTRA_CHEAP_REANCHOR` = "1 chirp per 4 craters" | The streak resets on any decoded frame ⇒ "no chirp unless 4 **consecutive**" |

---

## Appendix — knobs deliberately NOT recommended, and why

So nobody re-proposes them from the name alone:

* **`ULTRA_INFLIGHT_RTO`** — the tail RTO defect it describes is real, but the burst path
  already re-derives a frames-parameterised timeout every burst, and the knob only affects
  *first* transmissions while the observed 57 s tail was NACK-driven. Fix
  `unifiedBurstAckTimeoutMs`; do not run the knob.
* **`ULTRA_ADAPTIVE_RTO` paired with `ULTRA_INFLIGHT_RTO`** — where one acts the other is
  inert, and where they interact the composite RTO can land **under** the burst it is
  timing.
* **`ULTRA_ENTRY_EVM_CAP`** — one-shot-ness concentrates rather than reduces risk now that
  the entry rung seeds the latent prior, and on the proposed MPM test channel the connect
  estimator **saturates**, making the cap a systematic over-cap by construction.
* **`ULTRA_CHEAP_REANCHOR` solo** — a strict no-op without `ULTRA_CRATER_REANCHOR_HOLD=1`,
  and its incremental effect after HOLD is ~1.3-1.7 % of transfer time, an order of
  magnitude below rig resolution.
* **`ULTRA_ITERATIVE_CHEST` / `ULTRA_LTS_CFO_AVG` / `ULTRA_LTS_DFT_DENOISE_TAPS`** — all on
  the channel-mean axis, which a 4× NMSE improvement measured at exactly zero FER across 8
  cells at n=240/cell.
* **`ULTRA_MEASURE_BURST_FULL_ANCHOR` as the harness default** — it reproduces the resend
  regime, not the shipped warm handoff; the harness's real defect is its descriptor-off
  default.
