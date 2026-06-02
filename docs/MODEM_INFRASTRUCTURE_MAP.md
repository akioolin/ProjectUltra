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
| Tone-burst ACK monitor | reverse-channel FSK ACK detect (armed-only) | `streaming_decoder.cpp:404` | event-gated | 🟢 (§15 ACK) |
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
| MC-DPSK soft decode (Mode 1) | differential demod → LLRs → LDPC | `streaming_ofdm_decode.cpp:2290` (`decodeMCDPSKFrame`) | below-OFDM-floor/heavy fade | 🟢 (INVARIANT: `reset()`+`setCFO(frame.cfo_hz)` per frame) |

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
| `encodeToneBurstAck()` — 4-FSK reverse ACK | `streaming_encoder.cpp:733` | reverse channel | 🟡 (§15; wired, not default transport) |
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
| **BurstInterleaver** (cross-frame N-group byte permute) | `burst_interleaver.*`; TX `streaming_encoder.cpp:530`, RX `streaming_burst_interleave.cpp:496` | 🟡 default-OFF (`burstCrossFrameInterleaveOn()`); group *structure* still forms |

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

**ARQ — two subsystems:**
- `SelectiveRepeatARQ arq_` (`connection.hpp:434`, non-burst): window per mode — MC-DPSK
  `mcDpskWindowSizeForTiming` (1–5), OFDM_NARROW **3** (hardcoded), OFDM wideband
  `ofdmWindowSizeForChannel` **8 default, up to 16** on near-AWGN DQPSK/D8PSK ≥R1/2. 🟢
- `BurstStopAndWaitController burst_transport_` (`connection.hpp:510`): true one-way
  stop-and-wait, **🟢 UNCONDITIONAL — burst is THE OFDM-wideband file path (2026-06-02; the
  `ULTRA_BURST_TRANSPORT` env gate was REMOVED). `use_burst_transport_=true` always; the RX
  default `burst_transport_rx_` is now `true` (was `false`) so every StreamingDecoder owner
  (GUI ModemEngine, raw ultra_tnc/measure_ack_fer) gets burst-RX without a separate enable
  call.** The legacy `!use_burst_transport_` windowed-file branches are now dead (R1 deletion
  follow-up). SR dispatch `formAndSendBurstGroupSR`
  (`connection.cpp:2632`) when `burst_interleave_off_` (`connection.cpp:2051`, default true).
  GROUP_ACK is now **tone-burst** (`connection.cpp:413`); OFDM 1-CW GROUP_ACK only behind
  `ULTRA_LEGACY_OFDM_GROUP_ACK`.

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
| `ULTRA_BURST_INTERLEAVE` | cross-frame interleave ON→whole-group ACK / OFF→SR masks | **OFF** | `connection_policy.hpp:95` | FEAT |
| `ULTRA_BURST_GROUP_FRAMES` | burst group size, clamp [2,32] | code **6** (`kBurstInterleaveGroupFrames` `:64`, reconciled 16→6 on 2026-05-30 — mask-width-matched to the 6-bit SACK frame_mask; the old 16 was un-SR-addressable on the default interleave-OFF path) | `connection_policy.hpp:74` | FEAT |
| `ULTRA_BURST_DESCRIPTOR` | emit BURST_HEADER descriptor | **ON** (escape hatch) | `modem_engine.cpp:530` | FEAT |
| `ULTRA_BURST_HEADER_ONCE` | descriptor only on group 0 | OFF | `modem_engine.cpp:544` | FEAT |
| ~~`ULTRA_S16_WARM_HANDOFF`~~ | warm light-LTS group-start | **REMOVED 2026-05-31** — promoted to production default (always on); all 7 gate sites unconditional | — | ✅ codified |
| `ULTRA_S16_TRACE_WARM_WINDOW` | trace warm-sync window | off | `streaming_sync_acquisition.cpp:356` | DIAG |
| `ULTRA_LOCK_RATE` | hold data rate fixed for transfer | OFF | `connection.cpp:2342` | FEAT |
| `ULTRA_MAX_OFDM_RATE` | cap initial+adaptive rate | unset | `connection.cpp:673` | FEAT |
| `ULTRA_FRAME_CW` | override CW/frame | unset | `connection.cpp:727` | FEAT |
| `ULTRA_LDPC_Z` | Z=81→n=1944 vs 27→n=648. **16→1 sites (2026-05-30, DONE)**: RX via descriptor (`activeBurstLiftingZ()`), TX consumers via `ldpc_lifting_z_`, chunker + `makeFixedDataFrame` + `modem_engine` via `Connection::selectBurstLiftingZ()` (app/cli push it to the encoder). Z is now code-derived (traffic-class: bulk/file burst→81, gated on `use_burst_transport_`), written to the descriptor, read back. The 1 remaining read is the discovery override INSIDE the policy. The GUI harness no longer pins `ULTRA_LDPC_Z=81` (dropped 2026-05-30) — the traffic-class policy derives it (81 for file bursts, now that burst transport is the default). | 27 (policy) | `connection.cpp:4342` (override only) | FEAT (derived) |
| `ULTRA_LEGACY_OFDM_GROUP_ACK` | old OFDM GROUP_ACK vs tone-burst | OFF | `connection.cpp:445` | FEAT (A/B) |
| `ULTRA_SHORT_REANCHOR_CHIRP_MS` | short re-anchor chirp ms [100,300] | 100 | `connection_policy.hpp:380` | FEAT |
| `ULTRA_WIENER_DELAY_SPREAD_S` | Wiener freq-corr delay spread | **hardcoded 1e-3 (Moderate-HF) — NOT derived; adaptivity gap** | `channel_equalizer_pilot.cpp:28` | ADAPT |
| `ULTRA_WIENER_DOPPLER_HZ` | Wiener time-corr Doppler | **hardcoded 0.5 Hz (Moderate-HF) — NOT derived; adaptivity gap** | `channel_equalizer_pilot.cpp:35` | ADAPT |
| `ULTRA_DD_FADING_MAX` | fading-index gate for DD (BUG-8PSK-001) | 0.15 | `channel_equalizer_pilot.cpp:927` | ADAPT |
| `ULTRA_QPSK_DD` | opt-in DD tracking for QPSK | OFF | `channel_equalizer_pilot.cpp:891` | ADAPT |
| `ULTRA_COHERENT_DD_OFF` | disable all coherent DD | DD on | `channel_equalizer_pilot.cpp:901` | DIAG/AB |
| `ULTRA_REL_FADE_ONSET` / `_MAX` | relative-fade LLR inflation (QPSK/QAM8) | code default | `channel_equalizer_equalize.cpp:104/111` | ADAPT |
| `ULTRA_LLR_NOISE_EMP_FLOOR` | empirical post-eq noise-var floor | 0 (off) | `channel_equalizer_equalize.cpp:618` | DIAG |
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
   Z is code-derived via `Connection::selectBurstLiftingZ()` (traffic-class: bulk/file OFDM
   burst→81, gated on `use_burst_transport_`), written to the BURST_HEADER descriptor, read back
   by RX (`activeBurstLiftingZ()`); the app/cli push it to the TX encoder. The 1 remaining read is
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
  to 16** on near-AWGN DQPSK/D8PSK ≥R1/2. Only `BurstStopAndWaitController` is true window=1.
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
