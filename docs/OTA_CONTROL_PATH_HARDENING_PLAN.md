# OTA Control Path Hardening Plan

## Scope
Real HF two-station logs showed post-handshake collapse caused by:
- RX sample drops (`Buffer overflow, dropped ...`)
- repeated OFDM data-sync rejects after connect
- responder stuck in handshake waveform because handshake confirmation never arrived
- optimistic initial OFDM code-rate selection from chirp-era metrics

This plan hardens the control/data transition for OTA use while preserving existing simulator behavior.

## Objectives
1. Eliminate decode-thread stale-loop behavior that causes pointer drift and overflow storms.
2. Keep responder from being trapped in handshake waveform when first post-ACK frame is lost.
3. Reduce false-negative data-sync rejects on OTA fading captures.
4. Make initial OFDM rate selection less optimistic before first real OFDM evidence.
5. Clarify GUI mode telemetry so users can distinguish peer-reported SNR from local quality.

## Implementation Steps

### 1) Decoder wake discipline (critical)
- In `StreamingDecoder::processBuffer()`, return immediately on `wait_for` timeout when no new audio arrived.
- Avoid running `searchForSync()` repeatedly on stale samples.

Acceptance:
- No repeated search churn without new input.
- Lower risk of correlation pointer drift while idle.

### 2) Buffer pointer invariants and overflow control
- Add guard in `feedAudio()`:
  - if computed `unsearched` is nearly full-buffer after wrap, snap `correlation_pos_` to `write_pos_`.
- Keep overflow-drop fallback but throttle logs.
- Track cumulative overflow count in stats.

Acceptance:
- No overflow log storms during connect transitions.
- If overload occurs, logs are sparse but informative.

### 3) Responder handshake fail-safe
- Keep existing "confirm on first valid post-ACK frame" behavior.
- Add fail-safe timer for responder:
  - if no valid post-ACK frame arrives within grace window, force handshake complete and switch TX to negotiated waveform.
- Ensure timer is reset on disconnect/reset and when handshake confirms.

Acceptance:
- No long sessions stuck in `TX: Handshake mode -> last_rx_waveform_=4`.
- Responder eventually transitions to negotiated waveform/control profile.

### 4) Handshake confirmation correctness
- Confirm responder handshake only after frame validity checks (magic/header/destination), not on arbitrary bytes.

Acceptance:
- Noise bytes cannot falsely confirm handshake.

### 5) OTA-adaptive light-sync threshold
- For connected OFDM data sync:
  - keep stricter threshold for coherent modes.
  - use adaptive threshold (with floor) for differential modes using local SNR/fading hints.
  - add reject-streak backoff to prevent hard lockout during deep fades.

Acceptance:
- Fewer `DATA sync rejected` storms in OTA logs.
- No runaway false-lock behavior.

### 6) Conservative initial OFDM bootstrap rate
- Add bootstrap cap for initial OFDM data rate based on chirp-era SNR/fading:
  - avoid aggressive R2/3/R3/4 starts on borderline channels.
- Forced user rate still takes precedence.

Acceptance:
- Better first-frame stability after CONNECT/CONNECT_ACK on real HF.

### 7) GUI telemetry clarity
- Update mode log strings to explicitly show:
  - `peer_snr`
  - `local_fading`
- Keep waveform/mod/rate as negotiated mode display.

Acceptance:
- Operators can see why two stations may show different quality labels without assuming mode mismatch.

## Implemented in this pass
- Step 1: implemented.
- Step 2: implemented.
- Step 3: implemented.
- Step 4: implemented.
- Step 5: implemented.
- Step 6: implemented.
- Step 7: implemented.

## Validation Checklist
1. Re-run failing OTA pair logs and verify:
   - no overflow storms
   - responder exits handshake mode
   - lower sync-reject density
2. Run simulator regression:
   - connection + data + disconnect flow still passes
3. Run 10-seed/30-seed baseline in known-good profiles and confirm no major regression in success/retx metrics.

## Notes
- This pass prioritizes field robustness and handshake/control continuity.
- If throughput drops in marginal channels due to bootstrap cap, add later promotion logic once OFDM quality is measured from actual data frames.
