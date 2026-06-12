# 04 — Airtime efficiency ledger (where 38% of every cycle goes)

**Half-duplex first:** the ACK gap is the other station's turn — NOT reclaimable sender
airtime. Every lever below shrinks overhead *length*; none pretends to pipeline across
the turnaround. PA duty: the only mechanism in the code is the 8600 ms per-key-down
ceiling (`kMaxBurstAirtimeMs`, `connection.cpp:3122-3131`); no cooldown gap exists and
`isTurnaroundActive()` is unwired (`modem_carrier_sense.cpp` — no callers).

## 1. Cycle anatomy (QPSK R3/4, cw=8, group=5, Good@20, zero retx)

| Component | ms | % of 8.97 s cycle | Source |
|---|---:|---:|---|
| Data symbols (5 × 51 sym) | 5 950 | 66.3% | `connection_policy.hpp:397-408` |
| **BURST_HEADER descriptor (full dual-chirp 1200 + ~210 control)** | **1 410** | **15.7%** | `streaming_encoder.cpp:530-562`, `chirp_sync.hpp:33-34` |
| **Tone-ACK key-down (675 ACK + 150 lead + 50 tail)** | **875** | **9.8%** | `tone_burst_constants.hpp:181-184`, `modem_engine.cpp:719-726` |
| LTS light preambles (5 × 2 sym) | 233 | 2.6% | `ofdm_chirp_waveform.cpp:340-349` |
| Lead-in/tail on data key-down | 200 | 2.2% | `modem_engine.cpp:719-726` |
| RX decode + ACK-detect latencies | ~300 | 3.3% | est.; detect cadence 100 ms `streaming_decoder.cpp:173-175` |
| In-payload framing (19 B hdr/CRC + 5 B FILE_DATA per 480 B) | — | ~3.3% | `frame_v2.hpp:1029`, `file_transfer.hpp:79` |

Payload 2 280 B/cycle → **~2 030 bps zero-retx; 62% protocol efficiency**. Measured top
end (1910) = 94-96% of this — the structure, not decode quality, caps current QPSK.

## 2. The levers, ranked (respecting half-duplex + duty)

1. **Descriptor anchor amortization — +250-360 bps at QPSK, bigger share at dense
   mods. ⚠ Adversarially corrected: a build, not a knob.** Every multi-frame burst
   pays a full 1200 ms dual-chirp anchor for its BURST_HEADER even with warm sync live
   (`streaming_encoder.cpp:319,548`; measured 67 680 samples = 1.41 s,
   `PHY_ADAPTATION_DESIGN §14.31:1361`) — and single-frame key-downs pay a full anchor
   too (`connection.cpp:3530-3553`). BUT: (a) `ULTRA_BURST_HEADER_ONCE` is a
   **documented FAILED experiment** — the header carries the per-burst `group_seq` the
   GROUP_ACK matches on; header-less inner bursts mislabel as group 0 and the sender
   loops forever (§14.32, commits bd35ad7/251f12b). What it falsified is "one header
   per FILE", not "smaller anchor per burst". (b) The short-chirp TX emission was
   REMOVED ("R4 … superseded by warm-handoff", `streaming_encoder.cpp:139`; revert
   note `modem_engine.cpp:604-606` — it broke frame-stride timing); only the timing
   constants survive. So the *short-anchor descriptor* (keep header+group_seq every
   burst, shrink only its chirp) requires building TX emission AND RX short-chirp
   detection, honoring the §14.25 fixed-stride caution. Biggest prize, medium build.
   KEEP the full chirp on RESENDS — the dcfbe00 lost-tail rescue depends on it.
2. **Shorten the tone-ACK — +~85 bps upper bound. ⚠ Adversarially corrected: NOT
   TX-only, and 12 ms is design-flawed.** The production monitor is constructed with
   `{25 ms}` ONLY (`streaming_decoder.cpp:176-178`) — the {12,25,50,100} scan lives
   only in a default config used by unit tests; a TX-only change ⇒ **silent ACK loss**.
   And 12 ms/sym breaks the 4-FSK design: symbol rate 83.3 Hz > 75 Hz tone spacing
   (orthogonality), and 2400 Hz × 12 ms = 28.8 cycles (non-integer → leakage,
   `tone_burst_constants.hpp:11-24,60-68`). A sound fast rung is **13.33 ms** (integer
   relationship with 75 Hz spacing) or fewer symbols at 25 ms. Requires: symbol_ms/SNR
   plumbing through `Connection::TransmitToneBurstAckCallback` + both call sites
   (`app.cpp:626` AND `tools/ultra_tnc.cpp:503`), monitor scan-list update, and an
   AWGN+Watterson FER cell below 18 dB BEFORE enabling — each lost ACK costs an 11.2 s
   timeout cycle (`connection.cpp:3152-3180`), so a 5% loss rate erases the win.
3. **Group 5→6 frames — +~105 bps, fix a phantom.** The budget model charges
   (n−1)×100 ms for a short re-anchor the encoder NO LONGER EMITS
   (`connection_policy.hpp:461-483` vs `streaming_encoder.cpp:139` "R4: removed");
   removing the phantom lets 6 frames fit the same 8600 ms ceiling. Beyond 6: the
   6-bit tone-ACK `frame_mask` binds (`tone_burst_constants.hpp:122-146`), and for
   16QAM the **window policy binds next** — QAM16 is excluded from the
   high-throughput-window predicate so its uncapped ARQ window is 8, not 9-10
   (`connection_policy.hpp:304-318`). Widening the mask via the 4 reserved bits is
   free airtime-wise but those bits sit OUTSIDE the CRC-12 coverage (bits 0..15 only,
   `tone_burst_payload.hpp:59-60`) — a coordinated wire-format + CRC redefinition.
4. **Lead-in trim — +~50 bps.** 150 ms AGC lead-in × 2 key-downs/cycle
   (`modem_engine.cpp:719-726`); 50-100 ms is defensible at least on the ACK side.
   (Real radios need some settle — verify against the AGC/ALC fidelity rules before
   shrinking the data-burst lead-in.)
5. **R2/3 pilot sparsification — +8.5% on that rung.** R2/3 pays 12/59 pilots vs
   R3/4's 8/59; `ULTRA_R23_PILOT_SPACING` exists for the A/B. Needs worst-seed fade
   validation (sp5 was chosen for the fragile rungs deliberately).
6. **Header compression (19→~10 B post-connect) — ≤+2%, protocol churn. Do last.**

Stacked 1+2+3: QPSK R3/4 → ~2.5 kbps (still short — see 01 §5); **8PSK R3/4 → ~3 430;
16QAM R3/4 → ~4 170**. The levers are the *multiplier* on the modulation fix, not a
substitute.

## 3. Explicit non-levers (measured dead ends — do not re-propose)

- z=81/n=1944 long LDPC for Good@20 throughput: <1 dB robustness, ~0 or negative
  density at R3/4, architecturally unplugged on the unified path
  (`connection.cpp:3090-3096` gated on `!kUnifiedSeqEnabled()` which is hardcoded true
  at `:32`). The in-code "~3 dB more FEC margin" claim (`frame_v2.hpp:565-568`) is
  wrong by finite-length theory (~0.5-0.8 dB), and the "1.8× coherence interval" claim
  (`connection.cpp:3068-3070`) fails the code's own Tc math at Good's 0.1 Hz (a 1944-bit
  CW spans ~0.1×Tc). ⚠ Footgun: `ULTRA_LDPC_Z=81` env (`connection.cpp:3065-3067`)
  bypasses the unified gate and would desync CW geometry. The Moderate/Poor (>Tc)
  time-diversity case was never settled — park it there, not on Good.
- Burst group 8→4 (rejected 05-27: link-death cliff), T/R turnaround reclaim as a
  throughput lever (~2% — fidelity fix only), header field shaving beyond #6.
- Per-CW CRC addition: would COST 2-3%; syndrome + frame CRC suffice.
