# OTASim audit findings — 2026-05-19

Following Codex's Phase 1 rate-ladder investigation
(`docs/RATE_LADDER_INVESTIGATION_2026_05_19.md`), this doc enumerates
**all** OTASim fidelity gaps surfaced while reading the code, not just
the half-duplex blackout.

The principle: **the simulator is the measurement infrastructure.**
Every floor, every ladder threshold, every multi-seed verification we
ever do is only as honest as OTASim's HF physics. Fix the simulator
first, then re-measure, then iterate on the modem.

## Categorization

**Server fixes** = `ota_channel_core` + `ota_simulator_service`
(`SessionContext`, `SampleIndexedMixer`, channel models, the
gRPC/UDP service).

**Client fixes** = `OtaAudioPort` and the modem protocol code
(`selective_repeat_arq`, ARQ timing, etc).

## Tier A — must-have for valid floor/ladder measurements

### A1. Half-duplex RX blackout not wired in `OtaAudioPort` (server + client)
- Status: **in progress** in current Codex round on branch
  `fix/rate-ladder-groundup-2026-05-19`.
- `SessionContext::enqueueTransmit` accepts a `StationTxAudioState`
  with `tx_active` and respects it (`rx_blackout_inbox` machinery).
  In-process `VirtualAudioPort` wires this via `setRxBlackoutCallback`.
- `OtaAudioPort` does NOT override `setRxBlackoutCallback` nor pass
  `tx_active` over the wire. Result: server never applies blackout
  even though it has the logic.
- Without this fix, every OTASim measurement is full-duplex-leaky:
  ACKs can arrive while the receiving station is still transmitting,
  which real HF physically cannot do.

### A2. SACK uses application `MORE_FRAG`, not physical-burst state (client)
- Lives in `src/protocol/selective_repeat_arq.cpp` / `arq_policy`.
- Receiver decides when to transmit SACK based on whether the
  application sees a fragmented message ending. But
  `Connection::sendMessages()` pipelines multiple application
  messages into one continuous physical TX burst.
- Effect: receiver can schedule a SACK mid-burst → collision with
  sender's continuing TX on real radio. Today on full-duplex OTASim
  the collision is invisible; once A1 lands, the SACK is dropped
  during sender's blackout and we see the cost.
- This is a CLIENT fix, not OTASim. Separate but coupled workstream.

### A3. No PTT switching delay / TX→RX recovery tail (server)
- Real radios take 50-200 ms after PTT release before RX is clean
  (relay settle, ALC recovery, mic preamp recovery, antenna T/R switch).
- Simulator switches instantly. Once A1 lands, ACK timing gets tight
  because the receiver can immediately switch to TX and start the
  ACK with zero turnaround. Real radio cannot.
- Real radio physics has TWO sub-phases:
  - T/R switching transient (~10-30 ms): genuinely deaf both ways
  - RX recovery (rest of ~100-200 ms): RX is open (AGC settling),
    TX is still locked because operator/state machine prevents
    quick re-key
- Current `TRANSITION` state is binary deaf — matches phase 1 but
  is too aggressive for phase 2.
- Recommended: split into `TX_TR_SWITCH` (deaf both ways, ~10-30 ms)
  and `TX_COOLDOWN` (RX open, TX locked, rest of tail). Default
  values to be measured from real radios.

### A4. Client-side defense-in-depth: AudioPort should not deliver RX during own TX
- Today the half-duplex blackout is server-enforced. Client trusts
  the server to drop peer audio from its RX outbox during own TX.
- A small client-side guard would short-circuit any RX delivery while
  the client's own `isInRxBlackout()` is true. This:
  - Belt-and-suspenders: any tiny server timing gap doesn't leak
  - Matches real radio hardware (RX front-end is physically
    disconnected during TX, not just muted)
  - Saves CPU: skip RX processing entirely during TX
  - Simpler contract: client doesn't trust server for half-duplex
- Recommended change: `OtaAudioPort::getRxSamples()` and
  `VirtualAudioPort::getRxSamples()` both early-return silence
  when `rx_blackout_callback_()` is true.

## Tier B — must-have for hardware A/B confidence

### B1. RX buffer overflow silently drops audio (server)
- `kMaxRxQueuedAudioMs = 200` in `session_context.cpp`. If a client
  drains its RX outbox slower than the session clock advances,
  samples are dropped silently inside the server.
- For the modem this looks like impulsive RX dropouts that aren't
  in any real-HF physics. They are pure simulator-induced.
- Recommended: surface overflow via a counter the client can read,
  and consider larger default cap.

### B2. No client/server clock-drift modeling (server)
- Sample rate is assumed perfect 48 kHz everywhere. Real two-radio
  setups have 10-100 ppm sample-rate drift between soundcards.
- Modem CFO tracker handles this in production, but the simulator
  never produces drift to test the tracker.
- Recommended: optional per-station `ppm_offset` field that is
  modeled via fractional resampling on TX or RX path.

### B3. No ALC / TX peak limiting (server)
- Real radios apply automatic level control on TX (compression /
  clipping near peak power). Simulator passes audio through
  linearly.
- A clean simulator burst at 0.5 RMS will hit a real radio's ALC
  and emerge differently shaped, often with sub-symbol distortion.
- Recommended: optional ALC nonlinearity model on the TX path,
  defaulting off but documented.

## Tier C — fidelity / future-proofing

### C1. Fading reciprocity between link directions (server)
- In `advanceSessionClock`, `channel_->process()` is called
  sequentially: once for receiver A on B's audio, then for receiver
  B on A's audio. Channel object internal state (fading taps, CFO
  phase, RNG) evolves between calls.
- Result: A→B and B→A see decorrelated fading evolution. Real HF
  has reciprocal multipath (same atmospheric path, both directions).
- Low priority: doesn't matter when TX windows don't overlap, which
  is what half-duplex enforces.

### C2. CFO is global per channel, not per-station-pair (server)
- Real HF CFO depends on which TX-side crystal vs which RX-side
  crystal. Simulator applies one CFO globally.
- Low priority unless multi-station scenarios.

## Priority and sequencing

| Tier | Item | Effort | Blocks |
|------|------|--------|--------|
| A1 | Half-duplex blackout (server+client) | M | floor/ladder accuracy |
| A2 | SACK physical-burst (client) | M | once A1 lands |
| A3 | PTT switching delay (server) | S | once A1+A2 land |
| B1 | RX overflow visibility (server) | S | spurious dropouts |
| B2 | Clock drift (server) | M | hardware A/B credibility |
| B3 | ALC modeling (server) | L | hardware A/B credibility |
| C1 | Fading reciprocity (server) | M | symmetric link tests |
| C2 | Per-station CFO (server) | S | multi-station |

Realistic time: Tier A in 1-2 focused days, Tier B in 2-3 more,
Tier C in another week+. Each fix invalidates prior measurements,
so re-measure after each tier.

## What's blocked / valid as of this writing

- **All "OTASim-verified" floor numbers in CLAUDE.md** (MC-DPSK 5,
  OFDM R1/4 12, R1/2 14 dB, etc) are full-duplex-optimistic.
  Their PHY portion (sync, decode, soft-bit) is real, but the ARQ /
  ACK / retransmission characterization is not real-HF.
- The Layer 1-5 audit PHY work is unaffected (the receiver decode
  path doesn't change with half-duplex enforcement).
- The 13-commit audit branch (`bcfa716` merged to main) stays valid;
  its claims are about PHY thresholds, not operational throughput.

## Next steps (recommended sequence)

1. Codex completes A1 (half-duplex blackout).
2. Run full ctest baseline on the fixed OTASim. Investigate any
   regressions — they likely indicate A2 (SACK) issues.
3. Land A2 (SACK physical-burst) in a separate commit/round.
4. Land A3 (PTT switching delay) in a separate commit/round.
5. Re-run multi-seed floor sweeps on OTASim post-A. Compare to
   CLAUDE.md numbers. Document the delta as "OTASim full-duplex
   bias = N dB" so historical reads stay interpretable.
6. Tackle Tier B / Tier C only if the post-Tier-A numbers still
   don't match field expectations.
