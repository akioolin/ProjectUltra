# ProjectUltra — Modem Infrastructure Map (current valid infra)

**Last updated:** 2026-05-30 · **Branch verified:** `feat/oneway-arch-2026-05-27`

**Why this doc exists:** ~6 months of AI iteration left active code interleaved with scar
tissue, dead paths, and stale comments. Before any cleanup we need ONE place that says, per
stage, *what it is, where it lives (file:line), when it runs, and whether it is current valid
infra or removable.* This is that place. Built from a 4-way parallel code audit (RX chain /
FEC+framing / protocol+ARQ / TX+waveforms), file:line-verified, not written from memory.

> Maintenance: when you add/remove a stage or flip a knob's default, update the relevant row
> AND the §7 cleanup register. Keep classifications honest — a wrong 🟢/🔴 here misleads a
> future deletion.

## Classification legend
- 🟢 **ACTIVE** — on the production default path right now.
- 🟡 **EXPERIMENTAL** — env-gated, default OFF; in-flight / discovery scaffolding.
- 🟠 **LEGACY / FORCED-ONLY** — reachable only via a force flag or non-default config; not auto.
- 🔴 **DEAD / RESERVED** — unreachable in production (enum-only, test-tool-only, superseded).

---

## 1. RX signal chain  (raw audio → soft LLRs)

Producer/consumer split: `StreamingDecoder::feedAudio()` (audio thread, ring-buffer write only)
→ `processBuffer()` decode thread, 5-state machine `SEARCHING → SYNC_FOUND → DECODING`
(+`BURST_ACCUMULATING`, `MCDPSK_BURST_CONTINUING`). Note: `modem_rx_decode.cpp` is **not** DSP —
it's post-decode delivery/routing only.

| Stage | Purpose | Where | When | Class |
|------|---------|-------|------|-------|
| Ring-buffer write + overflow recovery | audio→buffer, force-reset on overflow | `streaming_decoder.cpp` (`feedAudio`) → `sync_controller_.ring_.writeSamplesToRingLocked`; **ring now owned by `sync::SyncRingBuffer` inside SyncController** (§7 C3) | every callback | 🟢 |
| Activity RMS gate | presence detector (HIGH 0.030/LOW 0.010) | `streaming_decoder.cpp:412` | every callback | 🟢 (instrumentation) |
| Tone-burst ACK monitor | reverse-channel FSK ACK detect (armed-only); scans the §15.5 SNR staircase durations `{12,25,50,100}` ms (2026-06-15 — was 25 ms-only, blind to the shorter SNR-adaptive ACK) | `streaming_decoder.cpp:404` | event-gated | 🟢 (§15 ACK) |
| RMS/signal-presence search gate | per-mode adaptive gate w/ noise-floor tracker | `sync_controller.cpp::acquireSearchWindow` (§7 C3: moved out of searchForSync) | SEARCHING hop (~100 ms) | 🟢 |
| Warm/light search-window planning | predict next-frame arrival, shrink LTS window | `sync_controller.cpp::acquireSearchWindow` → `::planWarmSearch` → `arrival_policy::planWarmSearchWindow` | connected light preamble | 🟢 core (warm-handoff now default; flag removed) |
| Connected light-LTS dispatch + §16.4 re-anchor | run detectDataSync, accept (pos-gate), arm full-chirp on reject streak | `sync_controller.cpp::detectConnectedLightSync` (§7 C3: moved from searchForSync) | connected data acq | 🟢 |
| Full-anchor light-LTS fallback | descriptor-chirp miss → control-threshold light DATA fallback | `sync_controller.cpp::detectFullAnchorFallback` (§7 C3) | connected re-anchor | 🟢 |
| **Cold dual-chirp sync (wideband)** | up+down chirp matched filter → timing+coarse CFO | `chirp_sync.hpp:353` (`detectDualChirp`) ← `ofdm_chirp_waveform.cpp:433` | cold acq / MC-DPSK handshake | 🟢 |
| Dual-listen narrowband chirp | 1250–1750 Hz chirp → `BandwidthMode::NARROW` | `streaming_sync_acquisition.cpp:824` | cold, disconnected | 🟢 (OFDM_NARROW entry) |
| Warm/light LTS data sync | LTS autocorrelation, no chirp, trusts `last_cfo_` | `ofdm_chirp_waveform.cpp:517` (`detectDataSync`) | connected data | 🟢 |
| CFO drift clamp + anti-replay | clamp per-frame CFO to established; dup-pos reject | `signal_policy::limitConnectedCFODrift` `streaming_sync_acquisition.cpp:930` | connected | 🟢 (INVARIANT: trust-but-clamp on fading) |
| Schmidl-Cox plateau `process()` state machine | S-C plateau→coarse CFO→LTS timing | `ofdm_stream_processor.cpp:243`; primitives `ofdm_sync.cpp:118/230/386` | — | 🟠 **forced-only** (OFDM_COX gone from auto; S-C *primitives* reused by warm-LTS, keep those) |
| toBaseband (NCO down-convert + CFO) | mixer + running `freq_correction_phase` | `channel_equalizer_baseband.cpp:97` | every symbol | 🟢 |
| extractSymbol (CP strip + FFT) | PocketFFT forward | `channel_equalizer_baseband.cpp:174` | every symbol | 🟢 |
| **LTS channel estimate** | per-carrier LS `H=rx/tx` over 2 LTS + guard-bin noise var | `channel_equalizer_lts.cpp:174` (`estimateChannelFromLTS`) | warm/cold preamble | 🟢 |
| Pilot LS update + α-smoothing | per-symbol pilot LS, α{1.0/0.5/0.1/0.9} | `channel_equalizer_pilot.cpp:513` | per data symbol | 🟢 |
| Pilot interpolation (de-sloped linear) | pilots→data carriers, phase-slope compensated | `channel_equalizer_pilot.cpp:1209` / `:945` | per symbol | 🟢 |
| Wiener/LMMSE 2-D interpolation | time-then-freq 1D LMMSE | `channel_equalizer_pilot.cpp:201` (`estimateWienerChannel`) | when scattered pilots negotiated | 🟢 (params env-overridable `ULTRA_WIENER_*`) |
| Decision-directed channel tracking | DD refine on flat carriers only (95% χ², 9:1 odds) | build `channel_equalizer_equalize.cpp:646`; merge `channel_equalizer_pilot.cpp:934` | QAM8/QAM16 flat | 🟢 (gate `last_fading_index<0.15`, BUG-8PSK-001). **`dd_qam16_*` naming = adaptivity "tell", rename candidate** |
| MMSE equalize (coherent) | `conj(H)·rx/(|H|²+σ²)` + per-carrier post-eq var | `channel_equalizer_equalize.cpp:444` | per symbol | 🟢 (coherent-only since thread A — the differential MMSE early-return was deleted; LMS/RLS branch gated by `config.adaptive_eq_enabled` → cold on default path) |
| DD per-carrier phase tracking | QPSK/BPSK phase corrections | `channel_equalizer_equalize.cpp:730` | coherent | 🟢 |
| Per-carrier adaptive LLR scaling | mag-EMA var tracking, fading carriers inflate noise | `ofdm_symbol_demap.cpp:303` | per symbol | 🟢 |
| Soft demap → LLRs | dispatch to `soft_demap::demap{…}` (coherent: BPSK/QPSK/QAM8–256) | `ofdm_symbol_demap.cpp:373` | per symbol | 🟢 (coherent-only since thread A — the differential DBPSK/DQPSK/D8PSK cases, the DD phase tracking, and the 2-pass D8PSK/DQPSK helpers were deleted) |
| Noise-variance estimate | guard-bin / `|H1−H2|²/4`; empirical floor off | `channel_equalizer_lts.cpp:662` | preamble + carried | 🟢 (`ULTRA_LLR_NOISE_EMP_FLOOR` diag, off) |
| **Doppler-coherence discriminator (Good/Moderate)** | per-frame `|H|²` snapshot (LTS mag²) → snapshot-lag-1 autocorr → coherence score (≥0.5 = Good); measures coherence TIME, which `fading_index` (fade DEPTH) cannot. `dopplerHz()` σ readout. | class `ofdm/doppler_coherence_estimator.hpp`; **hosted in `StreamingDecoder`** (member, reset per connection), fed in `streaming_sync_acquisition.cpp` `populateDecodeMetrics` (~per decoded frame); surfaced via decoder atomics → ModemEngine → `Connection::setChannelCoherence`; consumed gated in `connection_handlers.cpp` via `connection_policy::coherenceAdjustedFadingIndex` | per decoded OFDM frame; `valid()` after ≥24 snapshots | 🟢 (2026-06-16, UNCOMMITTED). Read-only + gated (raw `fading_index` until valid; invalid at CONNECT → zero default-path change). Hosted in the DECODER not the demod because burst transport rebuilds the OFDMDemodulator per group. GUI-proven good 0.70/moderate −0.11. PRECONDITION for `ULTRA_RATE_ADAPT`: BUG-DOPPLER-COHERENCE-MODECHANGE-WIPE. Design: `docs/CHANNEL_DISCRIMINATOR_DESIGN_2026_06_15.md` |
| MC-DPSK soft decode (Mode 1) | differential demod → LLRs → LDPC | `streaming_ofdm_decode.cpp:2290` (`decodeMCDPSKFrame`); demod core `multi_carrier_dpsk.hpp::demodulateSoft` | below-OFDM-floor/heavy fade | 🟢 (INVARIANT: `reset()`+`setCFO(frame.cfo_hz)` per frame). 2026-06-15: residual-carrier tracking added in `demodulateSoft` — (a) clock-offset+dial regression (per-carrier residual vs carrier freq), (b) decision-free M-th-power common-phase jitter tracker (coherence/activity/lock-gated). Config flag `MultiCarrierDPSKConfig::track_clock_offset` (default true); strict deadband no-op when idle (no-op on the shared-clock sim). Tolerates real soundcard ppm + slow carrier jitter. Also 2026-06-15: ratiometric guard at the `pre_ldpc_llr_reject` path (~`streaming_ofdm_decode.cpp:1166`) — a data-bearing (ratio not silent) MC-DPSK frame with weak 1-CW-peek LLRs falls through to the 4-CW wait gate instead of being mis-emitted as a PING (was PONGing a low-level CONNECT_ACK on real hardware). |

**CFO correction chain (the load-bearing invariant — 5 stages, all 🟢):**
1. Chirp coarse CFO — `chirp_sync.hpp:457` (gap-error). Coarse; ±2 Hz on fading.
2. Apply pre-correction + initial phase — `ofdm_chirp_waveform.cpp:915` → `setFrequencyOffsetWithPhase` → `toBaseband` NCO. INVARIANT: `processPresynced` must NOT reset the pre-set CFO/phase (`ofdm_stream_processor.cpp:824`).
3. LTS residual refine — `channel_equalizer_lts.cpp:310` (phase between 2 LTS symbols, ~0.3 Hz threshold).
4. Per-symbol pilot CPE tracking — `channel_equalizer_pilot.cpp:601` (±15° clamp).
5. **Feedback to cached `last_cfo_`** — `ofdm_chirp_waveform.cpp:997`. **INVARIANT: do not remove** — else wrong chirp CFO re-injects every frame → progressive drift → CW failures.

> Estimate-stage in-flight work (tasks #8/#9): `ULTRA_LTS_CFO_AVG` (lever ① CFO-clean 2-LTS
> averaging) and `ULTRA_LTS_DFT_DENOISE` (interim Gaussian smoother — being replaced by an
> active-band delay-domain LS denoise). Both gated, default OFF. See
> `docs/CHANNEL_ESTIMATE_REINFORCEMENT_DESIGN_2026_05_30.md`.

---

## 2. TX path

| Component | Where | When | Class |
|-----------|-------|------|-------|
| Constellation mapper (BPSK/QPSK/QAM8/16/32/64/256) | `modulator.cpp:85` (`mapBits`) | TX coherent | 🟢 QPSK/QAM8/QAM16; 🔴 QAM32/64/256 (no auto rung reaches them) |
| ~~Differential encoder (DBPSK/DQPSK/D8PSK)~~ | ~~`modulator.cpp:454`~~ | — | ❌ **REMOVED** thread A (commit `65b27b6`) — OFDM is coherent-only; the TX differential branches + `dbpsk_prev_symbols` are gone (the RX twin `lts_carrier_phases`/`lts_phase_offset` removed in `2f3c2ce`). MC-DPSK keeps its own differential modulator (`multi_carrier_dpsk.hpp`). |
| OFDM symbol builder (map→pilots→IFFT→CP) | `modulator.cpp:232` (`createOFDMSymbol`) | every TX symbol | 🟢 |
| `generateTrainingSymbols(count)` — **production LTS source** | `modulator.cpp:608` | preamble gen, always **count=2** | 🟢 |
| `generatePreamble()` — Schmidl-Cox STS×4+LTS×2 | `modulator.cpp:551` | — | 🔴 **DEAD** (test tools only; OFDM_COX legacy) |
| `createSchmidlCoxSTS()` | `modulator.cpp:332` | — | 🔴 DEAD (only caller is dead `generatePreamble`) |
| `generateProbe()` | `modulator.cpp:662` | — | 🔴 likely DEAD (no production caller — verify) |
| **Full chirp preamble** `[chirp][LTS×2]` | `ofdm_chirp_waveform.cpp:336` | PING/CONNECT, first frame, group-start, RESEND anchor | 🟢 |
| **Light data preamble** `[LTS×2]` | `ofdm_chirp_waveform.cpp:353` | connected DATA / burst members | 🟢 (saves ~1.2 s/frame) |
| Short re-anchor preamble `[short-chirp][LTS×2]` | `ofdm_chirp_waveform.cpp:364` | fading data re-anchor | 🟡 gated `adaptive_short_data_preamble_`; force-off when warm-handoff on |
| Per-burst LDPC Z push to demod | `ofdm_chirp_waveform.hpp:69` (`setActiveLDPCLiftingZ`) | burst encode | 🟢 (INVARIANT: must propagate Z to demod or deinterleaver gets 1296≠3888 bits → throws) |
| `encodeBurstLight()` — **the burst engine** | `streaming_encoder.cpp:429` | OFDM file/multi-frame burst | 🟢 (transport env-gated) |
| `encodeToneBurstAck()` — 4-FSK reverse ACK | `streaming_encoder.cpp:733` | reverse channel | 🟢 (§15; symbol duration SNR-adaptive via §15.5 staircase `symbolMsForSNR`, picked at `app.cpp` ACK callback off a lock-free in-band-SNR cache — 675→324 ms at ≥18 dB) |
| PAPR reduction | `streaming_encoder.cpp:108` | post-modulate | 🟡 PARKED (skipped for all coherent mods → inert on data path) |

**Production OFDM data config** (`StreamingEncoder()` ctor `streaming_encoder.cpp:64`):
`fft_size=1024, num_carriers=59, sample_rate=48000, center_freq=1500, cp_mode=LONG,
modulation=DQPSK, code_rate=R1_4, use_pilots=true, pilot_spacing=10`.
⚠️ `pilot_spacing=10` is the **default only** — the waveform overwrites it per-rung via
`ofdm_link_adaptation::recommendedPilotSpacing(mod,rate)` (mod/rate-adaptive), synced back at
`streaming_encoder.cpp:218/903`.

**Burst-group INVARIANTS** (load-bearing, from inline comments):
- Group members use plain light LTS, **no per-frame chirp** — RX accumulates at fixed
  `burst_min_block_` stride; a chirp prefix misaligns FFT → 0/8 deinterleave (§14.25).
- BURST_HEADER descriptor declares group size / cw-per-frame / mod-rate / interleave flags /
  lifting Z so cross-station RX self-configures (fixed cross-station 0/8).

---

## 3. FEC + framing + interleaving

**LDPC (🟢).** 802.11n QC-LDPC, base matrices `src/fec/ldpc_802_11n.hpp:33` (+custom R1/4 `:89`),
`expand(rate, Zp)` `:165`. **N = Z·24**: Z=27→**648** (default), Z=81→1944. Min-sum decoder
`ldpc_decoder.cpp:312` (`decodeSoft`); **LLR convention: negative=bit 1, positive=bit 0**;
min-sum factor 0.75 default / **0.9375 on fixed-frame path** (`frame_v2.cpp:23` — two factors,
intentional). Max iters rate-dependent (`ldpc_codec.hpp:86`: R1/4→50 … R1/2→80). `setRate()`
skips rebuild when unchanged (hot-path, ~75 ms/rebuild on Pi 5).

Rate→K (per `getCodeParams`, `ldpc_encoder.cpp:39`): R1/4=162, R1/2=324, R2/3=432, R3/4=486,
**R5/6=540**, of 648. ⚠️ **Table duplicated 5×** (`ldpc_encoder.cpp:39`, `ldpc_decoder.cpp:25`,
`ldpc_codec.cpp:36`, `frame_v2.hpp:885`, `frame_v2.cpp:1872`) — consolidate.

| FEC/framing component | Where | Class |
|-----------------------|-------|-------|
| LDPC encoder / decoder | `ldpc_encoder.cpp:136` / `ldpc_decoder.cpp:312` | 🟢 |
| `LDPCCodec`/`ICodec`/`CodecFactory` wrapper | `ldpc_codec.*`, `codec_factory.*` | 🟢 thin/partly vestigial (RX calls `LDPCDecoder` directly for `setMinSumFactor`) |
| `CodecType::{LDPC_5G,CONVOLUTIONAL,TURBO,POLAR}` | `codec_factory.hpp:22` | 🔴 reserved enum stubs |
| SoftCombineBuffer (HARQ chase-combine) | `soft_combine.hpp:82` | 🟢 but **default-OFF** (`enabled_=false`; `Connection::setSoftCombiningHARQ`) |
| Fixed-CW data frame (4-CW default, 1–8) | `frame_v2.hpp:608`, `makeFixedDataFrame` `frame_v2.cpp:2535` | 🟢 |
| 1-CW control frame (20 bytes) | `frame_v2.hpp:439` | 🟢 |
| Code rate on wire — DataFrame flags (2-bit, R1/4–R3/4 only) | `frame_v2.hpp:272` (`RATE_MASK`) | 🟢 ⚠️ can't express R5/6 or R1/3 on this path |
| Code rate on wire — MODE_CHANGE / BURST_HEADER (full byte) | `frame_v2.hpp:537` / `:588` | 🟢 |
| BURST_HEADER descriptor (0x25): size/cw/mod/rate/interleave/Z | `makeBurstHeader` `frame_v2.cpp:450` | 🟢 default ON (`ULTRA_BURST_DESCRIPTOR`) |
| GROUP_ACK 0x26 / GROUP_NACK 0x27 (+fast-NACK §14.30) | `frame_v2.hpp:245` | 🟢 |
| DATA_START/CONT/END (0x31–33) | `frame_v2.hpp` | 🔴 scaffolding (file-segment types not fully used) |
| **FrameInterleaver** (intra-frame across CWs) | `frame_interleaver.*`; TX `frame_v2.cpp:1944` | 🟢 **always on for OFDM** (skipped when cw=1) |
| **ChannelInterleaver** (within-CW time×freq) | `include/ultra/fec.hpp:133`; `frame_v2.cpp:1914` | 🟢 **default ON** |
| **carrier_ldpc_interleaver** (air-grid carrier spread V1) | `carrier_ldpc_interleaver.*`; `ofdm_chirp_waveform.cpp:73` | 🟡 default-OFF, **auto-engages on masked carriers** (likely removal candidate) |
| **BurstInterleaver** (cross-frame N-group byte permute) | `burst_interleaver.*`; TX `streaming_encoder.cpp:530`, RX `streaming_burst_interleave.cpp:496` | 🟢 **default-ON for dense coherent mods (≥16QAM)** via `burstCrossFrameInterleaveOn(mod)` (2026-06-14); OFF (per-frame SR-ARQ) for QPSK/8PSK. TIME diversity → +47% 16QAM R2/3 Good@20. `ULTRA_BURST_INTERLEAVE` overrides. |

**ARQ-profile INVARIANT** — `connection_policy::burstCrossFrameInterleaveOn()`
(`connection_policy.hpp:94`, default **false**) is the SINGLE source of truth deriving (a) TX
byte-permute, (b) TX ARQ semantics, (c) on-wire `BURST_FLAG_INTERLEAVE` bit. **OFF → per-frame
SR-ARQ masks; ON → whole-group ACK/NACK.** They disagreeing was the QAM16 offset-skip bug
(fixed d4b80b3) — do not re-introduce a second source.

---

## 4. Protocol / operating modes / ARQ

**Three modes:** MC-DPSK (`0x04`, handshake + low-SNR floor, DQPSK R1/4) 🟢 · OFDM_CHIRP
(`0x05`, primary throughput) 🟢 · OFDM_NARROW (`0x06`, 500 Hz/21-carrier) 🟢. `isOFDMMode()`
(`frame_v2.hpp:107`) = `OFDM_CHIRP||OFDM_NARROW`.

**Selection flow:** handshake in MC-DPSK → dual-listen sets BandwidthMode → SNR measured →
`Connection::negotiateMode` (`connection_handlers.cpp:840`) → `recommendDataMode`
(`connection.cpp:654`) → `capInitialOFDMRate` → `recommendCWCountForChannel` →
`enterConnected` (`connection.cpp:4468`) → `configureArqForCurrentDataMode` (`connection.cpp:4114`).

**ARQ — ONE subsystem (Transport Merge, 2026-06-06):**
- `SelectiveRepeatARQ arq_` (`connection.hpp`): the SINGLE data transport for ALL modes — MC-DPSK
  data, OFDM_NARROW data, OFDM-wideband file/message, and all control ACKs. Window per mode —
  MC-DPSK `mcDpskWindowSizeForTiming` (1–5), OFDM_NARROW **3** (hardcoded), OFDM wideband
  `ofdmWindowSizeForChannel` **8 default, up to 16** on near-AWGN ≥R1/2, with the unified file/message
  burst bounded per-key-down to an **8.6 s** airtime budget (`prepareUnifiedBurstWindow` →
  `burstAirtimeBudgetFrames`; `kMaxBurstAirtimeMs` 7000→**8600** on 2026-06-07 = **5 frames** at
  QPSK R2/3, derived from live per-frame airtime; env `ULTRA_MAX_BURST_AIRTIME_MS` overrides). A
  timeout-repair RESEND re-anchors with a **full chirp+LTS** (`force_full_preamble=true`, 2026-06-07)
  so a missed warm-handoff group re-acquires deterministically. 🟢
- **The OFDM file/message burst** = TX framing (`sendNextFileChunk`/`sendNextFragment` →
  `flushBurstBuffer` → `transmitFrameBatch` → `encodeBurstLight` + BURST_HEADER descriptor) + RX
  group assembly (`burst_transport_rx_` collector, default `true` → `onBurstGroupReceived` →
  `processArqFrame` → `endGroupReceiveAndAck`) that **`arq_` drives**. ACK is a **tone-burst**
  (`connection.cpp` `setEmitToneBurstSackCallback` + `onToneBurstAck`). 🟢
- **REMOVED 2026-06-06 (R1b):** the separate `BurstStopAndWaitController burst_transport_` group
  controller (own seq space, group stop-and-wait, GROUP_ACK/GROUP_NACK control frames,
  `formAndSendBurstGroup`/`SR`, `startBurstFileTransfer`, `burst_transport.hpp`). `use_burst_transport_`
  remains a bool = "burst framing on" (always true; gates Z/descriptor/rescue/rate — collapse is a
  follow-up). GROUP_ACK/GROUP_NACK frame types are now unhandled (no emitter).

**Warm-handoff (§16)** 🟢 PRODUCTION DEFAULT (the `ULTRA_S16_WARM_HANDOFF` flag was removed
2026-05-31; behavior is now unconditional) — light LTS at group-start, BURST_HEADER as anchor
(`streaming_encoder.cpp`); supersedes (and permanently disables) the §16.4 short re-anchor.

**Frame flow:** PING/PONG → CONNECT/CONNECT_ACK → MODE_CHANGE/ACK → DATA/ACK → DISCONNECT/ACK
(all handshake/control in MC-DPSK).

---

## 5. Waveform registry

`IWaveform` (`waveform_interface.hpp:48`); `WaveformFactory::create` (`waveform_factory.cpp:10`);
auto modes `getAvailableModes` = {MC_DPSK, OFDM_CHIRP, OFDM_NARROW}.

| Class | getMode | Class |
|-------|---------|-------|
| MCDPSKWaveform | MC_DPSK | 🟢 (handshake + low-SNR; `supportsDataPreamble()=false` → always full preamble) |
| OFDMChirpWaveform | OFDM_CHIRP | 🟢 primary OFDM data |
| OFDMChirpWaveform (`presets::narrowbandOFDM`) | OFDM_NARROW | 🟢 |
| ToneBurstEncoder | — (not IWaveform) | 🟡 4-FSK reverse ACK |
| OFDM_COX / Schmidl-Cox | enum `0x00` **removed** | 🔴 enum gone; STS code lingers in `modulator.cpp` (test-only) |
| OTFS_EQ/OTFS_RAW (`0x01/0x02`), MFSK (`0x03`) | — | 🔴 reserved; factory returns nullptr+WARN; not in `ModeCapabilities::ALL` |
| SC-DPSK (Barker-13) | `src/psk/dpsk.hpp` | 🟠 not an IWaveform, not in factory (CLAUDE.md table lists it — stale) |

Stale factory heuristics `WaveformFactory::recommendMode/getMinSNR/getMaxThroughput`
(`waveform_factory.cpp:112`) — legacy SNR-only tables superseded by `waveform_selection.hpp` +
`connection_policy.hpp`.

---

## 6. Master ENV-knob inventory

Buckets per the env-knobs→runtime-derivation workstream: **[FEAT]** in-flight feature flag ·
**[ADAPT]** channel-adaptive param that should become code-derived · **[DIAG]** diagnostic/oracle
(strippable) · **[FORCE]** test/dev force override.

| Knob | Effect | Default | Key site | Bucket |
|------|--------|---------|----------|--------|
| ~~`ULTRA_BURST_TRANSPORT`~~ | **REMOVED 2026-06-02** — burst transport is now unconditional (THE OFDM file path); the `=0` opt-out was deleted, no env read remains. RX default `burst_transport_rx_` flipped `false→true`. | (gone) | (removed from `connection.cpp`/`app.cpp`/`modem_engine.hpp`) | — |
| `ULTRA_ADAPTIVE_RATE` | BER-driven per-block rate adaptation | OFF | `connection.cpp:358` | FEAT |
| `ULTRA_BURST_INTERLEAVE` | force cross-frame interleave ON→whole-group ACK / OFF→SR masks (overrides the mod-gated default). 2026-06-14: default is now **mod-gated** — ON for dense coherent ≥16QAM (TIME diversity, +47% 16QAM R2/3 Good@20), OFF for QPSK/8PSK | mod-gated (16QAM ON) | `connection_policy.hpp` `burstCrossFrameInterleaveOn(Modulation)` | FEAT |
| `ULTRA_BURST_GROUP_FRAMES` | burst group size, clamp [2,32] | code **6** (`kBurstInterleaveGroupFrames` `:64`, reconciled 16→6 on 2026-05-30 — mask-width-matched to the 6-bit SACK frame_mask; the old 16 was un-SR-addressable on the default interleave-OFF path) | `connection_policy.hpp:74` | FEAT |
| `ULTRA_MAX_BURST_AIRTIME_MS` | per-key-down burst airtime ceiling → group size (frame count DERIVED from live per-frame airtime), clamp [5000,12000] | code **8600** (= **5 frames** at QPSK R2/3; 7000→8600 codified 2026-06-07 after a 20-seed Good@16 sweep tied groups 5/6 on goodput, 5 wins on key-down/SACK margin) | `connection.cpp` `burstAirtimeBudgetFrames` | FEAT |
| `ULTRA_RATE_ADAPT` | ACT on the §14.43 closed-loop quality feedback (receiver LDPC headroom→`rate_hint`→sender RateController). The loop is always WIRED (drives the GUI Adapt bars + diagnostics); this knob gates the actual rate CHANGE. The auto code-rate ladder is now `{R1_4,R1_2,R2_3,R3_4}` (**R5/6 RETIRED 2026-06-17** — measured loser, see RATE_LADDER_ANCHORS; R5_6 still a forcible probe). | **OFF** (2026-06-07 — wired+validated, but the adaptation POLICY churns/freefalls to R1/4; ship visibility, not auto-adapt) | `connection.cpp` `applyAdaptiveRateFeedback`; `rate_controller.hpp` ladder | FEAT |
| `ULTRA_QAM16_CLIMB` | QPSK-R3/4 → QAM16-R2/3 CROSS-MODULATION climb (2026-06-17). When adaptive rate is active and pinned at QPSK R3/4 for `kQam16ClimbStreak` consecutive clean groups (quality ≥ `climb_above`; default 4, env `ULTRA_QAM16_CLIMB_STREAK` [1..64]), the SENDER issues `requestModeChange(QAM16,R2_3)` at a CLEAN send boundary (reuses the `file_send_window_busy` gate → no ARQ-seq renumber/desync). The 8-clean-group streak doubles as a low-variance Good/Moderate proxy (a Moderate channel's fades reset it) — a sender-side stand-in for the receiver-side LTS coherence disc (deaf-while-sending; HW threshold unvalidated → disc-gating + the wire bit it needs is a deliberate later add). On QAM16: HOLD, or asymmetric PROMPT demote → QPSK R3/4 (after `kQam16DemoteBadStreak=2` sub-`drop_below` groups or a NACK), made **sticky** (no re-climb this connection); `maybeEscapeStuckFrame` also demotes QAM16→QPSK R3/4 on a stuck-frame crater. OTASim: no-regress climb-OFF; Moderate@20 correctly does NOT climb (CRC-clean PASS). KNOWN: on a continuous transfer the clean-boundary gate reaches R3/4 late, so the hop rarely fires mid-transfer on small files — needs a large transfer or faster R3/4 attainment. | **OFF** (experimental; zero-margin QAM16 anchor + unvalidated HW disc) | `connection.cpp` `applyAdaptiveRateFeedback` (climb), `maybeEscapeStuckFrame` (escape demote); members `qam16_clean_streak_`/`bad_streak_`/`sticky_demoted_` | FEAT (prototype) |
| `ULTRA_R23_BASIS` | pin the ENTRY rate to QPSK R2/3 (SNR ≥ 18) on any FADING channel (`fading_index ≥ kFadingAwgnMax=0.15`), decoupling entry from the coin-flipping Good/Moderate classifier. Keeps R3/4 on clean AWGN (a blanket pin cost ~11% there: OTASim AWGN@20 R3/4 2150 vs R2/3 1920). Entry-only; the climb path is not gated. | **ON** (2026-06-17, fading-gated) | `waveform_selection.hpp` `capInitialOFDMRate` | FEAT |
| `ULTRA_BURST_DESCRIPTOR` | emit BURST_HEADER descriptor | **ON** (escape hatch) | `modem_engine.cpp:530` | FEAT |
| `ULTRA_BURST_HEADER_ONCE` | descriptor only on group 0 | OFF | `modem_engine.cpp:544` | FEAT |
| `ULTRA_SIM_PAPR_PENALTY` | SIM-FIDELITY: drive the OTASim TX through the hardware peak-normalization (`normalizeTxBurstForHardware`, peak→`settings_.tx_drive`) instead of RMS-to-reference, so high-PAPR coherent-OFDM data frames deliver only (tx_drive/PAPR) average power vs the fixed reference-sized noise → models the peak-limited-PA penalty (~10.26 dB for coherent QPSK) the RMS-norm otherwise hides. Models the PAPR component only, NOT the cheap-card anchor-to-data gap. | **OFF** (byte-identical when off) | `app.cpp` `doQueueRealTxSamples` (sim branch ~2960) | DIAG/ADAPT |
| `ULTRA_SIM_TX_PEAK` | override the `ULTRA_SIM_PAPR_PENALTY` peak target (default = `settings_.tx_drive`); dial low (e.g. 0.12) to reproduce a low RX operating level. Clamped to the hardware peak range. | unset (=tx_drive) | `app.cpp` sim TX | DIAG/FORCE |
| `ULTRA_BURST_RMS_DIAG` | log per-collected-burst-frame gate inputs (`next_rms`/`anchor_rms`/`noise_floor` + ratios) for kept AND erased frames — for calibrating/confirming the relative erasure gate (esp. the next instrumented IONOS run) | OFF | `streaming_burst_interleave.cpp` `tryDemodulateNextBurstFrame` | DIAG |
| `ULTRA_BURST_ERASURE_ABSOLUTE` | force the LEGACY fixed `0.015` burst erasure gate (A/B harness vs the operating-level-relative default) | OFF (default = relative gate) | `streaming_burst_interleave.cpp` erasure gate | FORCE |
| `ULTRA_RX_AGC` | RX operating-level AGC: SLOW, AMPLIFY-ONLY, deadband normalizer in `feedAudio` (ring path only; ACK monitor stays raw) that raises a SEVERELY-low operating level (>9.5 dB below ref) toward the modem reference so the absolute gates (erasure, sync floor, CCA) work regardless of gain staging. Init high → exact no-op at normal level (verified good@20: OFF=ON=1710 bps, 0 engage); engages on a low channel (+13.6 dB at level 0.037). SNR-safe (scales signal+noise together). Labeled prototype — default off until IONOS-proven; production-clean form would sync-gate the level estimate. | **OFF** | `streaming_decoder.cpp` `feedAudio` | FEAT (prototype) |
| `ULTRA_ROBUST_IDLE_PING` | LOW-SNR HANDSHAKE FLOOR fix (#70). A PING is a bare chirp with no data (`encodePing`→`generatePreamble`); the receiver IDs it by a LEVEL test (`data/training RMS ratio < kPingMaxDataToTrainingRMSRatio=0.5`, abs floor `kPingChirpLockMaxDataRMS=0.16`). At low SNR broadband noise floods the silent gap (ratio 0.68–0.88 > 0.5) so a real PING reads as a faded CONNECT → waits for a 4-CW frame that never comes → no PONG → never connects (sim floor never < ~15 dB Good, vs the chirp itself locking solid corr 0.6–0.75 to ~6 dB AWGN). When ON, a solid chirp-lock (corr≥floor, gap OK) emits the PING on chirp signature ALONE — only for frames that already tripped the pre-LDPC false-lock reject (low LLR), so a cleanly-decodable CONNECT (good LLR→CW0 magic peek) is never short-circuited. Gated on `bare_chirp_expected_` (App sets FALSE during CONNECTING) so a faded CONNECT_ACK isn't mis-PONGed (#27-safe on the initiator). PROVEN sim: good@10/12 NEVER-connect→PASS (510/620 bps), good@8 connects, good@20 no-regress (robust path not triggered), 0 mispings. **Default-ON BLOCKED on:** responder-side faded-multi-CW-CONNECT starvation (chirp-lock can't tell a bare PING from a faded CONNECT, emit pre-empts the 4-CW decode — needs a stronger discriminator + rig measurement). | **OFF** (prototype; default build byte-identical) | `streaming_ofdm_decode.cpp` (~488 knob, ~1190 emit); `bare_chirp_expected_` decoder member ← `ModemEngine::setBareChirpExpected` ← `app.cpp` state handler | FEAT (prototype) |
| `ULTRA_CONNECT_RATIOMETRIC_SNR` | CONNECT-TIME RATIOMETRIC SNR fix (#74). The responder's connect-time rate decision consumes `IDLE_IN_BAND` SNR = `10·log10(kModemReferencePower/idle_noise)` — a noise-only meter that ASSUMES the RX signal sits at the sim reference `kModemReferenceInBandRms=0.3048`; on a real radio whose RX level differs it OVER-READS by the level deficit → too-aggressive rate (rig MPG@10: idle 13.1 → R1/2 stall). The level-invariant `MCDPSK_IN_BAND` training SNR (`updateTrainingSNREstimate`, `10·log10(signal/residual)`, both 50–2950 Hz, = `|H|²/noise_var`) already exists for the handshake preamble but `populateDecodeMetrics` only routes it when `connected_`. When ON, the `connected_` gate is relaxed so the ratiometric value drives the connect rate too (rig: connect flips 13.1→8.0, picks robust rung). ESTIMATOR PROVEN sound (`test_mcdpsk_snr_calibration`): AWGN tracks true within ~0.1–0.7 dB to 30 dB (no saturation); the GOOD-channel cap (~12 sim/~18.7 rig) is the physical FADING coherence limit over the ~170 ms training window, correctly reported as effective SNR — NOT a defect. Default-OFF because the faithful gate runs AT the 0.3048 reference where idle is correct (a default-on flip is unprovable on the only level-correct gate; ~−0.45 dB scale offset can shift a boundary rate pick). | **OFF** (prototype; default build byte-identical → `(connected_ \|\| false)==connected_`) | `streaming_sync_acquisition.cpp` `populateDecodeMetrics` (non-OFDM branch) + `connectRatiometricSnrEnabled()` | FEAT (prototype) |
| `ULTRA_FORCE_MCDPSK_RUNG` | DIAGNOSTIC force (`LOW`/`MID`/`ROBUST`/`STANDARD`): pin the MC-DPSK ladder rung, bypassing `selectLadderRung`'s SNR thresholds, to floor-measure each rung (the DQPSK rungs ROBUST/STANDARD are otherwise UNREACHABLE — `robust_floor` > `ofdm_floor`; STANDARD never returned). #72 (2026-06-29) standardized ALL MC-DPSK rungs on **sps=1024** (was ROBUST_LOW 2048 / STANDARD 512) so control==data baud → the handshake is mutually decodable on every rung; gears now vary only constellation (DBPSK/DQPSK)+rate. NOTE (updated #71 2026-07-01): forcing a rung now delivers files end-to-end — rig-proven forced DQPSK & DBPSK both CRC-clean @ MPG@9 — after the #72 handshake + #73 completion fixes; the earlier "forcing breaks the transfer" gotcha is obsolete. | unset | `connection_policy.hpp` `selectLadderRung` (top); `ladderRungForId` | FORCE/DIAG |
| `ULTRA_MCDPSK_WINDOW_CAP` | DIAGNOSTIC override (=N): further cap the MC-DPSK selective-repeat window for A/B round-trip measurement per rung. #71 (2026-07-01): the PRODUCTION default cap was lowered **5→3** (`kMaxRoundTripSafeMCDPSKWindow`) because the airtime-only sizing (`mcDpskWindowSizeForTiming`, 19 s TX-burst budget) was blind to the receiver decode + ACK round-trip — DQPSK's short frames (data_ms 3691) let the window grow to 5, whose ~18.9 s burst's ACK round-trip intermittently exceeded the ~45.7 s RTO → the sender blind-resends the whole window (0 cw_fail, ACKs bitmap=0, all cause=timeout) → spiral. Rig-measured @ MPG@9: DQPSK w5 delivered **1/3**, w3 delivered **3/3** CRC-clean at ~2x DBPSK. DBPSK unaffected (already caps at 3). Knob caps below the computed value; unset = no-op. Companion selector change: Good/AWGN DQPSK **selection floor** lowered `robust_mid_floor+2.5→+1.0` (Good 8.5→7.0, AWGN 7.5→6.0). | unset (default cap = 3) | `connection_policy.hpp` `mcDpskWindowSizeForTiming` + `selectLadderRung` | FORCE/DIAG |
| `ULTRA_TX_LEADIN_MS` / `ULTRA_TX_TAIL_MS` / `ULTRA_TX_ACK_LEADIN_MS` | configurable TX guard timing (#68). `postProcessTx` prepends a lead-in + tail of silence to every TX (PTT/PA-ramp/ALC settling so the chirp isn't clipped at key-up — RX does not need it). The legacy FIXED 150ms lead-in + 50ms tail (= 200ms/TX, on data + ACK + ping; the ACK's copy is in the half-duplex turnaround) is now tunable: `ULTRA_TX_LEADIN_MS`/`ULTRA_TX_TAIL_MS` global defaults, `ULTRA_TX_ACK_LEADIN_MS` overrides JUST the ACK (the turnaround-relevant, lowest-PA-thermal piece). Default-UNCHANGED (unset → 150/50, byte-identical). The configurability is the radio-agnostic fix (150ms is a one-size early-project guess, 2-5× over real radios IC-7300/FT-891 ~15-20ms T/R). FIDELITY CAVEAT: the cheap-card rig has no real 100W PA → a reduction can't be rig-validated (real PA ramp could clip first symbols); also the ACK lead-in gives the data-sender T/R-to-RX time (too short → missed ACK). Conservative-by-default until real-radio-proven. | **150/50/−1** (default-unchanged) | `modem_engine.cpp` `postProcessTx` (lead-in/tail); `transmitToneBurstAck` (ACK lead-in) | FEAT (config) |
| `ULTRA_WARM_TURNAROUND_OFF` (opt-out) | make the connected-OFDM pre-TX echo-clear (`clearRxBuffer(for_tx_echo=true)`, fired before every ACK turnaround) a decoder NO-OP, preserving warm-sync state + ring timeline across the half-duplex turnaround instead of `reset()`-wiping them (which forced a COLD 2.5s re-acquire of every next burst — the ~27% Good/Moderate turnaround tax). The audio side (`setRxMuted`+`stopCapture`+`AudioEngine::clearRxBuffer`) already prevents echo, so the decoder full-reset was harmful overkill; the no-op makes the rig behave like OTASim (which skips this path entirely, app.cpp:3040). Correct by construction — worst case = stale prediction → cold fallback (== old behavior), never a stranded frame. Rig-proven CRC-clean on BOTH Good (MPG@20: turnaround 2.71→1.54s median, −43%) AND Moderate (MPM@20: 1.59s, 0 stalls). The faithful gate CANNOT exercise this path (sim TX skips it) so it's a no-op on OTASim/ctest either way. The OTASim ~0.6s (warm predict-and-wait) is partly a SIM ARTIFACT (±20ms window vs ±260ms real inter-group jitter) — NOT a target. | **DEFAULT-ON** (2026-06-20; `ULTRA_WARM_TURNAROUND_OFF=1` to disable) | `modem_engine.cpp` `clearRxBuffer`; call site `app.cpp:3051` | 🟢 ACTIVE |
| `ULTRA_TNC_BULK_ACCUM` | TNC: hoard a flow-controlled PAT/B2F body (under-report BUFFER to cap 50 while absorbing so PAT keeps feeding past its 7×blocksize throttle, + 20 s BUFFER keepalive vs PAT's 60 s Flush timeout) → flush the whole body as ONE z=81 burst-file instead of <4 KB short-LDPC chunks. Body bursts+decodes CRC-clean; trailing FF blocked on BUG-TNC-B2F-002 | **OFF** (default; experiment) | `tnc_session.cpp` ctor + `onModemBufferLevel`/`tick`/`handleDataBytes` | FEAT (blocked on BUG-TNC-B2F-002) |
| ~~`ULTRA_S16_WARM_HANDOFF`~~ | warm light-LTS group-start | **REMOVED 2026-05-31** — promoted to production default (always on); all 7 gate sites unconditional | — | ✅ codified |
| `ULTRA_S16_TRACE_WARM_WINDOW` | trace warm-sync window | off | `streaming_sync_acquisition.cpp:356` | DIAG |
| `ULTRA_LOCK_RATE` | hold data rate fixed for transfer | OFF | `connection.cpp:2342` | FEAT |
| `ULTRA_AUDIO_BUFFER` | GUI/TNC SDL audio device buffer (samples/period; device double-buffers → total latency ≈ 2×). Smaller = lower half-duplex T/R turnaround latency (faster ACK round-trip → higher file goodput) but more underrun risk on slow hosts. Live pi5tnc QSO vs sim: identical 10 bursts/74.7 s airtime, but turnaround 3.6 s vs 1.1 s ⇒ ~1.3 s is buffer latency at 8192. Clamp [64,16384]. | **8192** (~170 ms @ 48 kHz) | `audio_engine.cpp` ctor → `setBufferSize` | FEAT |
| `ULTRA_MAX_OFDM_RATE` | cap initial+adaptive rate | unset | `connection.cpp:673` | FEAT |
| `ULTRA_ENABLE_QAM16_LADDER` | let the AUTO ladder SELECT 16QAM (Phase 1, 2026-06-12). ON → `selectCoherentOFDM` walks `kCoherentLadderQAM16Exp` with the {QAM16,R1/2} GOOD rung enabled at the Phase-0a measured floor (18 dB, EXPERIMENTAL zero-margin). Default ladder byte-identical when unset. Paired with `maxValidatedCoherentRate(mod)` per-mod rate cap. **2026-06-14: cap lifted QAM16→R2/3 + the 16QAM R2/3 Good@20 rung ENABLED** — cross-frame TIME interleave (auto-on for 16QAM) makes 16QAM R2/3 Good@20 deliver ~2033-2240 (ABOVE QPSK R3/4 ~1860); adaptive ladder GUI-verified to auto-pick 16QAM R2/3. No longer "lateral". | **OFF** | `waveform_selection.hpp` `qam16LadderEnabled()` / `kCoherentLadderQAM16Exp` (R2/3 Good@20) / `maxValidatedCoherentRate`(QAM16→R2/3) | FEAT (Phase 1+2b; 16QAM R2/3 now beats QPSK R3/4 on Good) |
| `ULTRA_FRAME_CW` | override CW/frame | unset | `connection.cpp:727` | FEAT |
| `ULTRA_LDPC_Z` | Z=81→n=1944 vs 27→n=648. **16→1 sites (2026-05-30, DONE)**: RX via descriptor (`activeBurstLiftingZ()`), TX consumers via `ldpc_lifting_z_`, chunker + `makeFixedDataFrame` + `modem_engine` via `Connection::selectBurstLiftingZ()` (app/cli push it to the encoder). Z is code-derived, written to the descriptor, read back. **Transport Merge 2026-06-06: the unified file path uses z=27** — `selectBurstLiftingZ` gates z=81 on `!kUnifiedSeqEnabled()` (always false) so z=81 long-LDPC is now dead (was the legacy `burst_transport_` file behavior); z=27 doubles as the descriptor's "regular frames" signal. The 1 remaining read is the discovery override INSIDE the policy. | 27 (always) | `connection.cpp` (override only) | FEAT (derived) |
| `ULTRA_LEGACY_OFDM_GROUP_ACK` | old OFDM GROUP_ACK vs tone-burst | OFF | `connection.cpp:445` | FEAT (A/B) |
| `ULTRA_SHORT_REANCHOR_CHIRP_MS` | short re-anchor chirp ms [100,300] | 100 | `connection_policy.hpp:380` | FEAT |
| `ULTRA_SHORT_ANCHOR_DESCRIPTOR_MS` | Phase 2a (2026-06-14): warm BURST_HEADER descriptor uses a SHORT DUAL chirp (per-chirp ms, gap 50) instead of the full 500 ms dual — reclaims ~600 ms/burst chirp airtime. Per-chirp ms, clamp [50,600], 0=off. **Channel-gated to R3/4** via `connection_policy::shouldUseWarmShortAnchorDescriptor` (coherent OFDM_CHIRP top rung only — single-chirp/aggressive-shortening crater on fading; the bad seed relocates with duration). Cold acquire / first-burst / resends keep the full dual (miss self-heals). GUI-verified knob=250: Good@20 R3/4 GATE=ON descr 38880 = 2110 bps (+7%); Mod@14 R1/2 GATE=OFF descr 67680 = 1040 bps (crater suppressed). Default OFF / byte-identical. | **OFF** | `ofdm_chirp_waveform.cpp` `shortAnchorChirpMs()` / `getShortAnchorChirpConfig()`; gate `connection_policy.hpp` `shouldUseWarmShortAnchorDescriptor`; wired `streaming_encoder.cpp` (descriptor) | FEAT (Phase 2a; default-off until full mod×rate×channel matrix) |
| `ULTRA_ANCHOR_SKIP_K` | #69 periodic full-chirp anchor + warm-skip — **SKIP not SHRINK** (`project_chirp_anchor_skip_not_shrink`). The full dual chirp (TB=1200 = fade margin) is near-optimal so shrinking it craters; instead emit it only every K groups (`burst_anchor_ordinal_ % K == 0` — a monotonic per-descriptor counter, NOT the ARQ base seq) and give skipped groups a LIGHT LTS-only (chirp-less) descriptor that rides the warm-predict + light-LTS path. Full chirp every K = robustness backstop + drift reset; session-first + resends keep the full chirp. **Wire flag** `BURST_FLAG_NEXT_LIGHT_ANCHOR` (frame_v2.hpp payload[4] 0x04) announces the next group's anchor so the RX full-/light-searches immediately (no grinding through light rejects — the K=3 crawl); K-gated fast §16.4 escalation (4 rejects vs 12) catches unannounced reactive resends. Rig MPG@20 10-run interleaved: **K=2 +10.5% mean / +12.8% median, 0 stalls**; K=3 banks no gain → **K=2 = sweet spot**. **GATED by the REACTIVE clean-streak (see `ULTRA_ANCHOR_SKIP_CLEAN_STREAK`)** — the rig DISPROVED a predicted coherence-disc gate (a Moderate channel read clean-Good for a whole transfer, #57), so the skip engages on OBSERVED delivery instead. **DEFAULT-ON 2026-06-20 (default 2, opt-out `ULTRA_ANCHOR_SKIP_K=1`):** rig-proven safe (MPG@20 + MPM@20, STALL=0, reactive revert holds under load) + safe-by-construction (resend→full chirp→can't be worse than K=1); throughput net tie-to-positive (Good A/B +1.9% mean, +8% clean pair, −3% fady pair). NOT a proven throughput win — default-on for safety + free clean-channel upside + duty savings. | **2 (DEFAULT-ON; =1 opts out)** | `streaming_encoder.cpp` `encodeBurstLight` (skip schedule + reactive streak) + `encodeFrame` (light preamble); `streaming_ofdm_decode.cpp` (stash announcement); `sync_controller.cpp` `noteGroupDelivered`/`detectConnectedLightSync` (arm search + K-gated escalation); `frame_v2.hpp` (flag + parse) | FEAT (#69 prototype; reactive-gated) |
| `ULTRA_REACTIVE_SHORT_CHIRP` | #62 reactive SHORT dual chirp. On a chirp-bearing (non-skipped) group, emit a SHORT dual chirp (duration = `ULTRA_SHORT_ANCHOR_DESCRIPTOR_MS`, e.g. 200/250 vs full 500) on a clean reactive streak, revert to FULL on crater. Reuses the short-anchor infra but REACTIVE-gated (reactive_skip_enabled) not R3/4-gated. RX needs NO change (detectSync auto-falls-back to the short detector, ofdm_chirp_waveform.cpp:478). **OVERNIGHT RIG VERDICT 2026-06-21 (~50 runs MPG@20, all CRC-clean STALL=0): SAFE WASH, not a goodput win.** Works on the real cheap-card rig (RX detects ~81-92%); but a detection miss costs a ~group resend (~14× a chirp's 0.65s saving) so it's net-positive only at ~99%+ detect, and the rig caps at ~92% (8% miss irreducible). Cross-phase short-vs-skip ~−1% (wash within ±25% noise); ~3% airtime saved is below the goodput floor. KEEP env-gated default-OFF (safe duty/thermal option). | **OFF** (default-off; safe wash) | `streaming_encoder.cpp` `encodeBurstLight` (reactive short-anchor gate) | FEAT (#62; investigated to ceiling — wash) |
| `ULTRA_SHORT_CHIRP_DETECT_SCALE` | #62 scales DOWN just the short-anchor fallback detector threshold (`detectSync`, ofdm_chirp_waveform.cpp:479) to catch more (lower-TB → lower-peak) short chirps. **FAILED EXPERIMENT (Phase 5 rig): scale 0.6/0.4 → RX detect count EXCEEDS sent count (180%/167% = FALSE POSITIVES) → mis-anchors → rtx 24→55 → goodput −11/−13%.** Detection can't be pushed past ~92% without false alarms. Keep default 1.0. **FOOTGUN — REMOVAL candidate** (only hurts). | **1.0** (default byte-identical; <1.0 backfires) | `ofdm_chirp_waveform.cpp` `detectSync` (short fallback) | FEAT (#62; proven counterproductive — remove) |
| `ULTRA_ANCHOR_SKIP_CLEAN_STREAK` | #57 REACTIVE anchor-skip gate (radio-agnostic, delivery-driven). Replaces the rig-disproven PREDICTED coherence-disc gate: a Moderate IONOS channel read clean-Good (coherence-area +0.20, raw acf lags1-5 +0.2-0.3) for a whole ~60 s transfer → a predicted label cannot safely gate the skip (a 60 s transfer is ~10-20 fade cycles = too few to pin Doppler; non-stationary). Instead, `anchor_skip_clean_streak_` counts consecutive clean (warm-delivered) groups; ANY resend/cold-start/§16.4 escalation (`warm_descriptor==false`, the connection forces a full chirp) RESETS it to 0; the skip engages only once streak ≥ this value, and reverts to full-chirp-every-group the instant a resend fires. Half-duplex ACK turnaround → `warm_descriptor==false` reflects the prior group's delivery (delivery-driven, not send-optimistic). The streak-reset IS the cooldown + auto-covers cold-start. Sim Good K=2: 5-group full-chirp warmup → reactive=ON → CRC-clean 1940 bps. The Stage-A coherence-area metric (`doppler_coherence_estimator.hpp::coherenceArea`) is kept as READ-ONLY telemetry only. | **4** (only active when `ULTRA_ANCHOR_SKIP_K`>1) | `streaming_encoder.cpp` `encodeBurstLight` (streak gate); `streaming_encoder.hpp` (`anchor_skip_clean_streak_`) | FEAT (#57; the gate that makes anchor-skip default-on safe) |
| `ULTRA_WIENER_DELAY_SPREAD_S` | Wiener freq-corr delay spread | **hardcoded 1e-3 (Moderate-HF) — NOT derived; adaptivity gap** | `channel_equalizer_pilot.cpp:28` | ADAPT |
| `ULTRA_WIENER_DOPPLER_HZ` | Wiener time-corr Doppler | **hardcoded 0.5 Hz (Moderate-HF) — NOT derived; adaptivity gap** | `channel_equalizer_pilot.cpp:35` | ADAPT |
| `ULTRA_DD_FADING_MAX` | fading-index gate for DD (BUG-8PSK-001) | 0.15 | `channel_equalizer_pilot.cpp:927` | ADAPT |
| `ULTRA_QPSK_DD` | opt-in DD tracking for QPSK | OFF | `channel_equalizer_pilot.cpp:891` | ADAPT |
| `ULTRA_COHERENT_DD_OFF` | disable all coherent DD | DD on | `channel_equalizer_pilot.cpp:901` | DIAG/AB |
| `ULTRA_REL_FADE_ONSET` / `_MAX` | relative-fade LLR inflation (QPSK/QAM8) | code default | `channel_equalizer_equalize.cpp:104/111` | ADAPT |
| `ULTRA_LLR_NOISE_EMP_FLOOR` | empirical post-eq noise-var floor (single-symbol hard-decision; measured NET-NEGATIVE 2026-06-12, superseded by `ULTRA_HERR_LLR_K`) | 0 (off) | `channel_equalizer_equalize.cpp:618` | DIAG |
| `ULTRA_HERR_LLR_K` | per-carrier channel-estimate-error (ε²_H) LLR term: `nv=(σ²+k·err_var·|H|²)/(|H|²+σ²)` using the Wiener interpolator's normalized MMSE residual (the pilot-anchored production form of EMP_FLOOR). Down-weights confident-wrong LLRs on poorly-estimated carriers near freq-selective nulls; inert on flat channels (err_var≈0). **GUI-validated 2026-06-12**: 16QAM R2/3 sp8 Good@20 loss 55→45% / +20% goodput 3/3 seeds; QPSK/AWGN no-regress; 16QAM R1/2 +3-8%. **k=1.0 VALIDATED** (k-tune 0.5/1.0/2.0 Good@20: peaks at 1.0; k=2.0 over-inflates, CW-fails spike; k=0.5 under-weights — the Wiener error_var is a calibrated variance, trust it 1:1). **DEFAULT-ON k=1.0 since 2026-06-17 (a10d4ef), SCOPED to QPSK/QAM8** (`!soft_gray_zone_csi`): cuts Mode-B confident-wrong CW-fails ~86% (controlled OTASim moderate@20). QAM16 EXCLUDED — stacks on `softGrayZoneNoiseInflation` → double-count → regression (QAM16 good@24 2720→2020 bps). `=0` disables. | **1.0 (ON, QPSK/QAM8 only)** | `channel_equalizer_equalize.cpp` `equalize()` (gated `!soft_gray_zone_csi`); populated `channel_equalizer_pilot.cpp` `updateChannelEstimate`; member `demodulator_impl.hpp per_carrier_h_error_var_` | ADAPT (Phase 2b; the unified per-carrier reliability model) |
| `ULTRA_LTS_DFT_DENOISE` / `_TAPS` | LTS H freq denoise (interim smoother) | OFF | `ofdm_demodulator_setup.cpp:34` | FEAT (task #8, in-flight) |
| `ULTRA_LTS_CFO_AVG` | CFO-clean 2-LTS averaging (lever ①) | OFF | `ofdm_demodulator_setup.cpp:39` | FEAT (task #9, in-flight) |
| `ULTRA_FORCE_DATA_MOD` / `_RATE` | force mod/rate, bypass ladder | unset | `waveform_selection.hpp:512/525` | FORCE |
| `ULTRA_FORCE_WAVEFORM` | pin the NEGOTIATED data waveform (`OFDM_CHIRP`/`OFDM_NARROW`/`MC_DPSK`) below its auto entry-SNR — floor probing; both ends set it | unset | `app.cpp:1303` (GUI forced-waveform apply) | FORCE |
| `ULTRA_CHANNEL_DOPPLER_HZ` | override Watterson Doppler (sim) | code default | `ota_channel_core/channel.cpp:178` | FORCE/DIAG |
| `ULTRA_*GENIE*` (data-aided, timing/CFO, channel, sigma) | genie oracles for QAM16 isolation | OFF | `channel_equalizer_pilot.cpp:62/134`, `streaming_ofdm_decode.cpp:103`, `modulator.cpp:276` | DIAG (oracle) |
| `ULTRA_FAILURE_ATTRIBUTION`, `ULTRA_CONSTELLATION_DIAG`, `ULTRA_CFO_DEBUG_LOG`, `ULTRA_HARQ_DEBUG_LOG*`, `ULTRA_DUMP_CFO_*`, `ULTRA_CARRIER_SENSE_DEBUG`, `ULTRA_STARTUP_LOG`, `ULTRA_E2E_DEBUG_LOG`, `ULTRA_DIAGNOSTICS_DIR` | logging / dumps | off/unset | various | DIAG |

---

## 7. Cleanup register (actionable — current valid infra vs removable)

> Deletions are tracked as a focused action list in **`docs/REMOVAL_BACKLOG.md`** (the
> demolition list). This §7 covers ALL cleanup (incl. consolidate/rename/codify, which are
> not deletions). When an item here becomes a decided deletion, add it to the backlog with
> its scope + KEEP caveat.

**Safe-to-remove / dead (verify no test-tool dependency first):**
1. Schmidl-Cox TX: `modulator.cpp` `generatePreamble()` `:551`, `createSchmidlCoxSTS()` `:332`,
   `g_logged_tx_pilots` `:122` — test-tool-only, OFDM_COX is gone. `generateProbe()` `:662`
   likely dead (verify).
2. Schmidl-Cox RX plateau state machine `ofdm_stream_processor.cpp:243` — forced-only; **keep
   the S-C correlation primitives** (`ofdm_sync.cpp`) reused by warm-LTS.
3. Reserved enum stubs: `CodecType::{LDPC_5G,CONVOLUTIONAL,TURBO,POLAR}`; waveform
   OTFS_EQ/OTFS_RAW/MFSK; FrameType DATA_START/CONT/END (or finish file-segment impl).
4. `carrier_ldpc_interleaver` — default-off, auto-on only on masked carriers; most likely
   removable diversity layer (confirm masked-carrier path first).

**Consolidate (duplication, not dead):**
5. Rate→K table — 5 copies (§3). One source.
6. Entry-SNR floor table (10/12/14/18) — 2 copies: `waveform_selection.hpp:458` vs
   `connection_policy.hpp:212`.
7. `ULTRA_LDPC_Z` — **DONE 2026-05-30, 16→1** (`docs/LDPC_Z_DERIVATION_DESIGN_2026_05_30.md`).
   Z is code-derived via `Connection::selectBurstLiftingZ()`, written to the BURST_HEADER descriptor,
   read back by RX (`activeBurstLiftingZ()`); the app/cli push it to the TX encoder. **NOTE (Transport
   Merge 2026-06-06):** the unified file path uses **z=27** (n=648) — the z=81 long-LDPC branch was the
   legacy `burst_transport_` file behavior and is now dead (`selectBurstLiftingZ` gates z=81 on
   `!kUnifiedSeqEnabled()`, always false → always returns 27; z=27 is also the descriptor's
   "regular frames, not a separate file-burst" signal). Dropping z=81 long-LDPC is a known merge
   trade-off (fewer FEC margin for fade diversity); revisit if file FER on fading regresses. The 1 remaining read is
   the discovery override inside the policy. No-regression proven (cli short-msg + default
   `--file` at Z=27). **REMAINING: faithful GUI proof of the Z=81 burst flip** (use_burst=1) +
   Codex — cli's burst harness is "not the faithful gate".
8. Rate picker (`selectOFDMCodeRate`/`recommendDataMode`) — the "4 gate arrays × 3 passes"
   over-complexity is real (4 gate arrays/descriptor + QPSK/QAM16/D8PSK/DQPSK passes +
   bootstrap cap + adaptive climb). Per-channel hardcoded D8PSK `if`s `waveform_selection.hpp:612`
   are un-generalized special-cases. Simplify after 3000 (per memory).

**Rename (the adaptivity "tell"):**
9. `dd_qam16_*` on the shared coherent-DD path — functionally generalized to QAM8/QAM16, still
   16QAM-named (`channel_equalizer_equalize.cpp:648`, `channel_equalizer_pilot.cpp:940`).

**Codify (env scaffolding → code-derived, per ADAPT bucket):**
10. `ULTRA_WIENER_*`, `ULTRA_REL_FADE_*`, `ULTRA_DD_FADING_MAX` — channel-adaptive params. NOTE
    (2026-05-30): their *defaults* are NOT derived today — `robustDelaySpreadS()`/`robustDopplerHz()`
    return hardcoded Moderate-HF constants (1e-3 s / 0.5 Hz) with no channel input; the comment in
    `channel_equalizer_pilot.cpp:23` admits this is wrong on Good HF (0.5 ms / 0.1 Hz). So "codify"
    here means *replace the constant with a derivation* `f(negotiated coherence time/BW)` — NOT
    freeze the env default into a `constexpr`. This is a proof-gated PHY change, not mechanical
    cleanup; blocked on the estimate work (tasks #8/#9). Until then they stay env-overridable.

**Hot-path getenv (DONE 2026-05-30, behavior-neutral):** `robustDelaySpreadS()`/`robustDopplerHz()`
were called per-carrier inside the Wiener loop (`:243/:287`), and the DD knobs per pilot-interp call
— `getenv()` is a linear `environ` scan and not thread-safe, a real-time hazard. Converted to the
read-once `static` idiom already used in `channel_equalizer_equalize.cpp` (`kRelFadeOnset`). Proven
neutral: `OFDMWienerInterpolator` passes, `cli_simulator` AWGN R1/4 7/7. The remaining ~90 getenv
sites are read at config/handshake time (not hot) — no further hot-path work needed.

---

## 8. Corrections to CLAUDE.md / MEMORY.md (stale facts found by this audit)

These are wrong in the current top-level docs and should be fixed:
- **OFDM_COX is REMOVED from the protocol enum** (`frame_v2.hpp:30` "formerly OFDM_COX —
  removed"), not "forceable/legacy." It is **not selectable**; only the Schmidl-Cox *sync
  primitive* remains. CLAUDE.md (Waveform Summary table, mode notes) and several MEMORY entries
  still call it forced/legacy.
- **ARQ windows:** CLAUDE.md says "window=1 for MC-DPSK" and wideband "up to 8." Actual:
  MC-DPSK is timing-derived **1–5**, OFDM_NARROW is **3**, wideband OFDM is **8 default and up
  to 16** on near-AWGN ≥R1/2, with the unified file/message burst bounded to a ~7 s airtime budget
  per key-down (`prepareUnifiedBurstWindow`). (`BurstStopAndWaitController` was REMOVED 2026-06-06 —
  R1b transport merge; there is no longer a separate window=1 group controller.)
- **pilot_spacing=10 is a default, not a constant** — overwritten per-rung by
  `recommendedPilotSpacing(mod,rate)`.
- **SC-DPSK** is listed in the CLAUDE.md waveform table but is not an `IWaveform` and not in the
  factory.
- **`decode_bench` + `DecodeBenchReplay` RETIRED 2026-05-30** (commit pending): the headless
  WAV-fixture A/B tool + its replay CTest are gone, along with the 6 `fixtures/ofdm_chirp_*dqpsk*.wav`
  fixtures. The replay path had drifted from the live streaming decoder (`frames_decoded=0` on
  *every* fixture — it never entered the 4-CW data path) and all testing is now done on
  `tools/gui_qso_scenario.sh` (live GUI feedback), the trusted floor gate. Consequence: the old
  "OFDM_CHIRP R1/4 Good 15 dB **locked in DecodeBenchReplay**" floor claim is now UNBACKED —
  re-establish that floor on the GUI gate during the ladder rework. (`fixtures/ota_test_r14_15s.wav`,
  a self-contained manual OTA listening fixture, survives.)
- **`cli_simulator` + `test_waveform_simple` + `SimulatedStation` RETIRED 2026-05-30** (commit
  `207a0af`, ~14k lines). They were a divergent TX wrapper around the shared StreamingEncoder/
  Decoder PHY; the GUI's `ModemEngine` path is the production wrapper and `tools/gui_qso_scenario.sh`
  is now THE faithful full-protocol + fade/throughput gate. `measure_ack_fer`/`ota_simulator serve`
  survive (the `ultra_sim_station` lib was un-bundled — it had glued SimulatedStation to the PHY
  sources; `decode_bench` later retired 2026-05-30, see above). Historical `cli_simulator`
  proof-notes above are kept as record.
