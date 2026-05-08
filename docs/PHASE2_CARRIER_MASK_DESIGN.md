# Phase-2 Per-Frame Carrier Mask — Design (Codex 2026-05-06)

This is the concrete design document for backlog #5 phase-2,
authored by the Codex expert review pass on 2026-05-06 after the
night-session attempts failed (see `docs/archive/SESSION_2026-05-05_NIGHT.md`
for what didn't work and why).

**Status:** Design only. Not implemented. Tag
`experimental/per-carrier-attempt-1-failed-2026-05-05` is the
prior-attempt forensic snapshot.

---

## 1. Wire Format

Optional PHY mask header immediately after LTS/training and
before the OFDM payload. Present only when payload mask !=
all-on. **All-on frames remain legacy and bit-identical**, so
peers without `PHY_MASK_V1` never see a header byte differ.

Header is one existing hardened R1/4 1-CW control-profile
codeword, DQPSK, unmasked, all carriers. Protection is R1/4
LDPC + CRC16 + inverted CRC. No MODE_CHANGE involvement.

20-byte PHY header info word:

| Byte | Field |
|---:|---|
| 0-1 | Magic `0x50 0x4D` (`"PM"`) |
| 2 | Version/scheme: high nibble `1`, low nibble `1` = bitmap mask + interleaver v1 |
| 3 | Flags, v1 must be `0` |
| 4 | Payload profile: bits `7..5 = cw_count - 1`, bits `4..2 = mod_id`, bits `1..0 = rate_id` |
| 5 | Interleaver id: `0` = CarrierLDPC v1 |
| 6 | Masked carrier count, valid `1..8` |
| 7 | Reserved, must be `0` |
| 8-15 | `active_mask_le64`; bit `k` is logical data carrier `k`, `1 = transmit`, `0 = erase`; bits `59..63` must be `0` |
| 16-17 | CRC16-CCITT over bytes `0..15`, big-endian |
| 18-19 | `crc16 ^ 0xFFFF`, big-endian (catches stuck-bit failures) |

`rate_id`: `0=R1/4`, `1=R1/2`, `2=R2/3`, `3=R3/4`.
`mod_id`: `0=DBPSK`, `1=DQPSK`, `2=D8PSK`, `3=BPSK`, `4=QPSK`.

**Forward compatibility:** advertise `PHY_MASK_V1` in
CONNECT / CONNECT_ACK. If absent, TX never emits this header.
Unknown version/scheme/interleaver IDs ⇒ reject the frame, do
not guess.

## 2. Interleaver (chosen over codebook)

Codex's reasoning: HF notches move; a tiny fixed mask book will
miss real interference. Use a full bitmap with a mathematically-
vetted interleaver that spreads any allowed mask across LDPC
parity checks.

For `Ncw ∈ 1..8`, define `N = 648 · Ncw`, `C = 59 carriers`,
`Q = bits_per_carrier`. For original coded bit index
`i = cw·648 + ldpc_bit`, transmit at air-grid index:

```
a = (307 · i) mod N
symbol  = a / (C·Q)
carrier = (a % (C·Q)) / Q
bit_lane = a % Q
```

`307` is coprime to `648·Ncw` for every supported `Ncw` (Codex
verified inline). RX uses the modular inverse to de-interleave.

**Why 307?** For DQPSK, same-carrier erasure over successive
OFDM symbols advances by 11 LDPC base columns and 1 circulant
row; adjacent-carrier erasure advances by 1 base column and 11
rows. Both strides are coprime to the 24-column / 27-row
802.11n QC-LDPC structure, so contiguous carrier erasures
spread across codeword columns, rows, and CWs instead of
forming clustered variable-node erasures (which would cause
stopping-set decoder failure, the hazard Codex flagged in the
morning review).

Mask v1 caps erasure to 8 carriers.

## 3. TX-Side Mask Selection

TX uses the most recent reverse-direction OFDM frame
LTS/pilot estimate: `γ_k = |H_k|² / σ²_k`. **This is an
optimization input only; correctness comes from the
transmitted header**, so no reciprocity assumption needed.

Per-peer rule:

1. Maintain latest `γ_k` (dB); expire after 3 seconds.
2. Compute median over 59 carriers.
3. Candidate bad carrier if `γ_k < median − 8 dB`.
4. Mask carrier immediately if `γ_k < median − 14 dB`;
   otherwise require two consecutive reverse frames below
   threshold.
5. Sort by worst `γ_k`, mask at most 8.
6. Do not emit a mask unless ≥ 2 carriers qualify, or one
   carrier is below median by 14 dB.
7. Remove a carrier after three consecutive observations
   above `median − 5 dB`.
8. Minimum 2 payload frames between non-all-on mask changes.

AWGN stays all-on (no carrier crosses the −8 dB threshold).

## 4. RX-Side Decode Flow

1. Sync + LTS unchanged.
2. Try to decode one R1/4 PHY mask header from the first
   post-LTS payload slot.
3. If magic + CRC pass and capability is negotiated: consume
   header, validate fields.
4. **If header decode or CRC fails: assume no PHY header,
   all-on mask, legacy payload start. NEVER use last-known
   mask.** (This is the line that prevents the closed-loop
   stale-mask trap.)
5. If header validates: demodulate payload grid using the
   advertised profile.
6. Build full all-carrier LLR grid. For masked carrier
   positions, insert `LLR = 0`.
7. Apply CarrierLDPC inverse interleaver.
8. Split CWs and LDPC decode.
9. Frame CRC / header CRC rules unchanged.

**Persistent state:** negotiated capability, latest reverse
`γ_k`, hysteresis counters, HARQ key including `mask` and
`interleaver_id`. NO persistent decode mask.

## 5. Acceptance Criteria

**AWGN regression** (must be a strict no-op):
- 5 KB and 20 KB, SNR 15 and 20, 5 seeds each.
- No PHY header emitted, all-on mask.
- 0 retx, throughput within 1 % of baseline.

**Watterson Good SNR = 15 hardware** (the gain target):
- Mac↔Pi5, 20 KB, seeds 1–5.
- 5/5 pass, every seed ≥ 1631 bps baseline.
- Total retx ≤ 5 across all seeds.
- No panic downshift.

**Synthetic notch test** (the architecture justifier):
- 20 KB, SNR = 15, 4 adjacent carriers attenuated 25 dB.
- 10 seeds.
- Mask selected within 2 payload frames.
- 10/10 pass, median ≥ 1500 bps, total retx ≤ 10.

**Interleaver unit gate** (the LDPC-legality proof):
- For `Ncw ∈ 1..8`, masks of every contiguous 1..8 carriers
  touch all available CWs and ≥ 8 distinct LDPC base columns
  per erased carrier over the frame.

## 6. Risks + Mitigations

1. **Header false decode or loss:** CRC + inverted CRC; failed
   header falls back to all-on; ARQ handles the loss.
2. **Puncturing overload:** hard cap at 8 carriers; offline
   erasure-spread tests across all supported rates before
   merge.
3. **Selector flapping:** two-frame add rule, three-frame
   remove rule, two-payload-frame cooldown.

## 7. Implementation Phasing

Each sub-phase ships something independently testable.

1. **Wire structs only** (`src/protocol/frame_v2.*`, protocol
   tests). Verify byte layout, CRC, reserved-field rejection.
   No PHY changes.
2. **CarrierLDPC interleaver** (`src/fec/`, new tests).
   Verify bijection (RX·TX = identity) and erasure spread for
   the unit gate above.
3. **OFDM mask plumbing** (`src/ofdm/`,
   `src/gui/modem/streaming_encoder.cpp`,
   `streaming_decoder.cpp`). Verify all-on bit identity and
   masked LLR-zero insertion. AWGN regression must hold here.
4. **TX selector** (streaming RX metrics + encoder policy).
   Verify AWGN no-op and synthetic-notch trigger.
5. **Hardware gate**: run the three acceptance suites above
   before enabling by default.

---

## Open questions for the user

None at this stage — Codex committed to specifics throughout.
The design is implementable. Sub-phase 1 is the natural
starting point and is small enough for a single Codex round.
