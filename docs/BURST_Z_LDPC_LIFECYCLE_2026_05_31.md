# Burst Z / LDPC Lifecycle + Carrier-LDPC Air-Block Model (2026-05-31)

**Status:** LIVE reference. Born from the low-SNR burst file-transfer debugging
(DQPSK R1/4 AWGN@10 / Good@10). Captures the two-LDPC-flow model, the
carrier-LDPC interleaver air-block counting, the full `z` state lifecycle, the
**control-must-be-z=27** invariant, and the fade-lost-descriptor failure mode —
so none of it is re-discovered. Feeds the **sync refactor**
(`docs/SYNC_ACQUISITION_FIX_PLAN_2026_05_31.md`): the `z`-state is part of the
same scattered-state sprawl the SyncController should consolidate.

> Line numbers are as-of-2026-05-31 and will drift — grep the symbol, not the line.

---

## 0. TL;DR (the mental model, proven correct)

- **Small LDPC (z=27, n=648) is the DEFAULT.** Every control/ACK/BURST_HEADER
  frame is z=27.
- **We LIFT to large LDPC (z=81, n=1944) only when a BURST_HEADER descriptor
  says to** — and only for that group's DATA frames.
- **We DROP BACK to z=27 at group-end.** z=27 is the resting state between groups
  (so the next BURST_HEADER, a 1-CW z=27 control frame, is sized + demod'd right).

This invariant is **already implemented** (it is not a TODO). The corollary that
trips everyone up: **the BURST_HEADER is the sole declaration of z=81. If a fade
destroys it, its z=81 data has no lift and is demod'd at the z=27 default → fails.
That is physics (ARQ resends), not a drop-back bug.**

Two distinct LDPC codeword sizes in one transfer:

| Frame class            | z   | n (coded bits) | CW/frame | Decode path |
|------------------------|-----|----------------|----------|-------------|
| Control / ACK          | 27  | 648            | 1        | control-first peek, explicit 648 |
| BURST_HEADER descriptor| 27  | 648            | 1        | control-first peek, explicit 648 |
| Burst DATA             | 81  | 1944           | 2        | data path, `active_ldpc_block_size` |

`LDPC_CODEWORD_BITS = 648`, `LDPC_CODEWORD_BYTES = 81` (`frame_v2.hpp:880-881`;
mirrored `ofdm_chirp_waveform.cpp:17-18`). K varies with code rate; **n is always
648 at z=27 and 1944 at z=81** — see `[[feedback_ldpc_n_is_fixed_648]]`.

---

## 1. The `z` state variables (≥3 copies — the sprawl)

| Variable | Home | Default | What it sizes | Set by |
|----------|------|---------|---------------|--------|
| `ldpc_lifting_z_` | `ofdm_chirp_waveform.hpp` (~188) | 27 | `getMinSamplesForCWCount()` airtime (`cpp:1240` → `codeword_bits = z==81?1944:648`) | `setActiveLDPCLiftingZ()` |
| `active_ldpc_block_size` | `demodulator_impl.hpp:123` | 648 | soft-bit **extraction** in `processPresynced`/`getSoftBits` | `OFDMDemodulator::setActiveLDPCBlockSize()` (`ofdm_stream_processor.cpp:639`) — the ONLY assignment |
| `have_burst_descriptor_` | `streaming_decoder.hpp:694` | false | gate for `activeBurstLiftingZ()` | set `true` at `streaming_ofdm_decode.cpp:765`; **never cleared** (see §6 latent) |
| `last_burst_descriptor_` | `streaming_decoder.hpp` | — | carries `.lifting_z` (the declared z) | `streaming_ofdm_decode.cpp:764` |

**`setActiveLDPCLiftingZ(z)`** (`ofdm_chirp_waveform.hpp:69-77`) sets BOTH waveform
copies at once:
```cpp
ldpc_lifting_z_ = (z == 81) ? 81 : 27;
demodulator_->setActiveLDPCBlockSize(ldpc_lifting_z_ == 81 ? 1944 : 648);
```

**`activeBurstLiftingZ()`** (`streaming_decoder.hpp:288`) — the **single RX source
of truth** for *what the transfer declared*:
```cpp
return (have_burst_descriptor_ && last_burst_descriptor_.lifting_z == 81) ? 81 : 27;
```
Already used to size burst data assembly/decode at `streaming_ofdm_decode.cpp:2737,
2829, 2893` and `streaming_decoder.cpp:640`, and the assembler at
`streaming_burst_interleave.cpp:461`. **The one place still NOT driven by it is the
waveform's `active_ldpc_block_size`** — that is driven by the toggled
`setActiveLDPCLiftingZ` calls, which is the entire source of the fade-lost-descriptor
mismatch (§5B).

### The three `setActiveLDPCLiftingZ` call sites (the whole lifecycle)
1. **LIFT** — `streaming_ofdm_decode.cpp:771`: on BURST_HEADER consume,
   `setActiveLDPCLiftingZ(bi.lifting_z)` (→ z=81 for a data group).
2. **DROP-BACK** — `streaming_burst_interleave.cpp:734`: at `finalizeBurstGroup()`,
   `setActiveLDPCLiftingZ(27)`. Required so the next BURST_HEADER search + the next
   control/ACK demod are z=27.
3. **TX** — `streaming_encoder.hpp:207`: encoder mirror.

---

## 2. Why CONTROL must be z=27 (the invariant that kills "just persist z=81")

The connected-OFDM **control-first peek** (`streaming_ofdm_decode.cpp:626-704`)
tests "is this incoming frame a control frame?" before treating it as data:
```cpp
630  constexpr size_t CONTROL_LDPC_BLOCK = v2::LDPC_CODEWORD_BITS;   // 648
650  bool control_ok = processWaveformForCodewords(SampleSpan(frame_buffer...), 1);
669  auto control_soft_bits = waveform_->getSoftBits();              // sized by active_ldpc_block_size
670  if (control_soft_bits.size() >= CONTROL_LDPC_BLOCK) { ...decode 648... }
```
`getSoftBits()` is sized by `active_ldpc_block_size`. **If we persisted z=81
globally, every ACK and every BURST_HEADER would be demod'd at 1944 and the 648
control read would be misaligned → the entire control plane breaks.** Hence:

> **Persisting z=81 across the transfer is WRONG.** Control frames (incl. the
> descriptor itself) need active=648. The post-burst drop to z=27 is mandatory.
> (Reverted attempt: `streaming_burst_interleave.cpp:734` → `activeBurstLiftingZ()`
> — do NOT reintroduce.)

---

## 3. Decode dispatch order (one demod, two consumers)

```
process(samples)  [ofdm_chirp_waveform.cpp:358 path]   → builds soft_bits_ at active_ldpc_block_size
  ├─ control-first peek (629-704)   reads 648  → if OK: ACK / BURST_HEADER / control
  └─ data path (1145+)              reads soft_bits → burst accumulation (burst_marker, 1054)
```
- `burst_marker` (`:1054`) = `connected_ && is_ofdm && OFDM_CHIRP &&
  waveform_->wasBurstInterleaved() && (use_burst_interleave_ || burst_transport_rx_)`.
- The group-start frame is detected by a **negated-LTS marker** (TX negates the
  first LTS symbol of each group-start; RX `wasBurstInterleaved()` trips on it,
  `ofdm_chirp_waveform.cpp:934-944` restores it before channel est).
- **Same `process()` output feeds both consumers**, so `active_ldpc_block_size`
  must already be correct *before* `process()` runs — which is why the BURST_HEADER
  (lift) must arrive + decode *before* the data frames it describes.

---

## 4. Carrier-LDPC interleaver — counts in 648-bit AIR-BLOCKS, not z-codewords

The carrier-LDPC interleaver is a frequency-diversity bit shuffle applied as the
final TX permutation before the air grid; RX un-shuffles. **It is a fixed design
over `kLdpcCodewordBits = 648`-sized blocks** (`carrier_ldpc_interleaver.hpp:9-10`,
`kCarrierLdpcMultiplier = 307`):
```cpp
// carrier_ldpc_interleaver.cpp:14-19
size_t carrierLdpcBitCount(size_t Ncw) { return kLdpcCodewordBits * Ncw; }   // 648 * Ncw
// interleaver[i] = (307 * i) % (648*Ncw)   — one permutation over the whole 648*Ncw block
```
So **`Ncw` is the number of 648-bit air-blocks, NOT the number of LDPC codewords.**
A z=81 (1944-bit) codeword is **three** 648-bit air-blocks. A 2-CW z=81 data frame
= 3888 air bits = **Ncw 6** (not 2). Supported range `Ncw ∈ [2,8]`
(`ofdm_chirp_waveform.cpp:20-21`).

| | site | computes Ncw as |
|---|------|-----------------|
| TX forward | `applyCarrierLdpcForward` (`ofdm_chirp_waveform.cpp:73`), called `:414`; count at `:393` | `encoded_data.size() / LDPC_CODEWORD_BYTES` = `/81` → **6** for z=81 ✓ |
| RX inverse | `applyCarrierLdpcInverse` (`:122`), called `:988`; count at `:969` | **FIXED 2026-05-31:** `soft_bits_.size() / LDPC_CODEWORD_BITS` = `/648` → **6** ✓ (was `/active=1944` → **2** ✗) |

For z=27 both reduce to `soft_bits/648` (identical, no regression).

---

## 5. Failure modes (both seen on the GUI faithful gate)

### 5A. Carrier-LDPC Ncw mismatch — **FIXED (commit 9189b70)**
- **Symptom:** z=81 DATA decoded to garbage on the **differential (DQPSK)** path —
  RX got **1296** soft bits not **3888**, LDPC bailed `max_iters=0`, group `0/6`.
- **Coherent QPSK was unaffected** (took a different carrier-LDPC eligibility branch,
  skipped the inverse), which masked it as a coherent-vs-differential split — the
  diagnostic key that cracked it.
- **Root cause:** RX computed Ncw in z=81 codewords (`3888/1944=2`) → `applyCarrierLdpcInverse`
  built `out(2*648=1296)`, dropping 2/3 of the LLRs with the wrong permutation. TX
  shuffled 6 blocks, RX un-shuffled 2.
- **Fix:** RX counts in 648-bit air-blocks (§4). AWGN@10 → RESULT=PASS, CRC-OK,
  420 bps, 0 retx (was: file never delivered).

### 5B. Fade-lost BURST_HEADER → z=27 mis-demod — **PHYSICS, not a bug**
- **Symptom:** `Got 1296 ... need 648 ... 0/6 max_iters=0` on a group RESEND.
- **Mechanism:** a deep fade destroys the group's BURST_HEADER descriptor (the lift
  to z=81 never happens), z stays at the z=27 default, the z=81 data behind it is
  demod'd at 648 → 1296 → fails.
- **Evidence (Good@10):** descriptor RX at 117.885s, none until 160.276s, `Got 1296`
  at 147.060s in the gap — **no lift event near the failure.**
- **Why it is not fixable by drop-back timing:** the descriptor is the **sole**
  z-declaration; without decoding it the RX cannot know the data is z=81. The clean
  invariant (§0) working correctly is *exactly why* a missed descriptor lands on z=27.
- **Why a "retry data at z=81 anyway" fallback is rejected:** (1) it violates the
  invariant (lifts without a descriptor — a guess); (2) the BURST_HEADER is already a
  1-CW DQPSK **R1/4 full-chirp-anchor** frame — about as robust as we have, so a fade
  deep enough to kill it usually kills the data behind it too → low payoff. ARQ resend
  is the correct, half-duplex-honest answer ("zero retx on fading is unphysical").

---

## 6. Latent / open items

- **`have_burst_descriptor_` is never cleared** (`streaming_decoder.hpp:694` set at
  `:765`, no reset). Within one transfer this is fine (it's the persistent "transfer
  declared z=81" signal). **Across transfers / sessions it could go stale** — verify it
  is reset on disconnect / new connection / decoder reset before a *subsequent* z=27-only
  transfer relies on the default. Not yet observed to bite; flagged.
- **`active_ldpc_block_size` is the lone copy NOT driven by `activeBurstLiftingZ()`.**
  Consolidating all `z` reads onto `activeBurstLiftingZ()` (the source of truth) is the
  clean end-state and belongs in the sync/state refactor, not a point patch.

---

## 7. Sync path (context — how a group is acquired; full detail in the sync fix plan)

Two-tier, confirmed on the AWGN@10 + Good@10 traces:
- **Group anchor (BURST_HEADER):** FULL CHIRP — `"Full OFDM anchor sync detected"`,
  corr ~0.84. Radar/matched-filter sync, locks at deeply negative SNR.
- **Contiguous DATA frames:** LIGHT-LTS warm window —
  `"DATA sync detected in warm window"`, corr vs threshold ~0.25 (search) / 0.52 (diff
  acceptance). On AWGN the corr stays healthy (~0.62); on fading it can collapse to noise.
- **WARM position-gating fallback (commit e0ceee4, behind `ULTRA_S16_WARM_HANDOFF`):**
  when light-LTS corr is below the noise floor, locate the frame by **cadence prediction**
  and let **LDPC** validate (no correlation gate). 0 hits on AWGN@10 (corr healthy), **1
  hit on Good@10** (`"light-LTS corr below noise floor; cadence-located, LDPC validates"`)
  — it is the *fading* insurance, dormant on AWGN.

**Tie-in to the refactor:** COLD (full chirp, strict) → WARM (light LTS, position+LDPC)
→ RE_ACQUIRE (forced full chirp). The `z`-lifecycle above is a **parallel** scattered
state machine to the sync state machine; both leaked these bugs because the state is set
from many sites. The SyncController should own BOTH the sync mode and the burst `z`
context as one consolidated, single-source-of-truth object. See
`docs/SYNC_ACQUISITION_FIX_PLAN_2026_05_31.md §7`.

---

## 8. Quick grep map

```
setActiveLDPCLiftingZ        # the 3 lifecycle write sites (lift / drop-back / TX)
active_ldpc_block_size       # the extraction-sizing copy (ofdm_stream_processor.cpp:639 sole set)
activeBurstLiftingZ          # the RX source-of-truth reader (streaming_decoder.hpp:288)
have_burst_descriptor_       # the transfer-level z=81 latch (set :765, never cleared)
applyCarrierLdpc(Forward|Inverse)   # the 648-air-block shuffle (TX :73 / RX :122)
CONTROL_LDPC_BLOCK           # the explicit-648 control decode (streaming_ofdm_decode.cpp:630)
burst_marker                 # data-path gate (streaming_ofdm_decode.cpp:1054)
```
