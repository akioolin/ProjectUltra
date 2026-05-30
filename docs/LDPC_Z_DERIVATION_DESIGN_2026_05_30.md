# LDPC lifting-Z: derive in code via the BURST_HEADER descriptor (2026-05-30)

**Goal:** kill the 16 scattered `getenv("ULTRA_LDPC_Z")` reads. Replace them with ONE
code policy → the BURST_HEADER descriptor (the wire contract) → every consumer reads the
descriptor (RX) or the policy-set member (TX). Z stops being a process-global static flag
and becomes *derived in code* per burst.

## Why this is needed (the smell)
`ULTRA_LDPC_Z` is read at 16 sites across 6 files. Each consumer independently re-derives
Z (`atoi(env)==81 ? 81 : 27`) because the per-burst value isn't plumbed to all of them. The
TX chunker (connection) and the encoder (modem_engine) must agree on Z or you get the
truncation / 70%-zero-pad bug (`frame_v2.cpp:2563`, `connection.cpp:4355` comments). A
process-global env can't express "long LDPC for *this* file burst, short for *that* ACK" —
which is exactly what we want.

## The policy (user decision 2026-05-30): traffic-class-derived
```
selectBurstLiftingZ(ctx):
  if not OFDM            -> 27   // MC-DPSK always short
  if control frame      -> 27   // fast ACK turnaround
  if bulk / file burst  -> 81   // long LDPC (n=1944) — fade diversity where latency is free
  else (interactive)    -> 27   // short message, fast turnaround
```
- **Behavior-neutral for both validated paths:** `cli_simulator` short messages (no file) →
  27 (unchanged); GUI file transfer (`gui_qso_scenario.sh` ran `ULTRA_LDPC_Z:=81`) → 81
  (matches the validated harness). So this *codifies the de-facto state*, it does not flip an
  unvalidated default.
- Long codeword for bulk = principled: a 1944-bit block spanning ~1.8× the coherence interval
  at 0.1 Hz Doppler buys fade diversity; latency doesn't matter for a file. A short ACK wants
  Z=27 for turnaround. (See modem_engine.cpp:560 comment — Z=81 ⟹ cw_per_frame=2.)
- One discovery override (`ULTRA_LDPC_Z`) survives INSIDE `selectBurstLiftingZ()` only — a
  single labeled escape hatch for sweeps, not 16 scattered flags.

## Single source of truth
- **Wire contract (already exists):** BURST_HEADER descriptor `payload[5]` carries `lifting_z`
  (`makeBurstHeader` `frame_v2.cpp:468`; decode `frame_v2.hpp:600`). RX already calls
  `setActiveLDPCLiftingZ(bi.lifting_z)` (`streaming_ofdm_decode.cpp:771`) and caches
  `last_burst_descriptor_.lifting_z`.
- **TX authority:** the **connection** owns traffic class (`file_transfer_`, `use_burst_transport_`),
  so the policy lives there: `Connection::selectBurstLiftingZ()`. It sizes its own chunker and
  is pushed to the PHY encoder via the app (same path as `cw_count`, `app.cpp:949`).

## Data-flow after the change
```
TX:  Connection::selectBurstLiftingZ()  ── policy (traffic class) ───┐
        │ sizes chunker (currentDataPayloadCapacity, makeFixedDataFrame)
        └─ app pushes Z to modem  ─► StreamingEncoder::ldpc_lifting_z_
                                        │ encoder consumers read the member
                                        └─ makeBurstHeader(lifting_z) ─► descriptor payload[5]
WIRE:  BURST_HEADER.payload[5] = lifting_z
RX:  descriptor decode ─► last_burst_descriptor_.lifting_z (cached)
        └─ every RX consumer reads the cached descriptor value (NO env)
```

## Site classification (16 → 0 env reads on the data path)
| Site | Role | After |
|------|------|-------|
| `modem_engine.cpp:573` | TX decision | use app-pushed member (default 27); cw=2 coupling stays tied to it |
| `streaming_encoder.cpp:524,936,1154` | TX consumers | read `ldpc_lifting_z_` |
| `connection.cpp:1951,2214,2687,4355` | chunker sizing | `selectBurstLiftingZ()` |
| `frame_v2.cpp:2563` (`makeFixedDataFrame`) | frame builder | Z passed in as param (free fn) |
| `streaming_ofdm_decode.cpp:2734,2828,2898` | RX deinterleave | `last_burst_descriptor_.lifting_z` |
| `streaming_decoder.cpp:641` | RX decode | `last_burst_descriptor_.lifting_z` |
| `streaming_burst_interleave.cpp:463` | RX deinterleave | `last_burst_descriptor_.lifting_z` |
| `streaming_encoder.cpp:584` | TX descriptor | already uses member ✅ |

## Build order (separately provable)
- **Step A — RX trust-the-wire (lowest risk).** RX consumers read the cached descriptor Z
  instead of env. Behavior-neutral in the harness (descriptor==env==81); strictly *more*
  correct (the `:2828` site is env-only today with no descriptor fallback — a latent bug).
  Proof: GUI file transfer Z=81 decodes byte-exact; `cli_simulator` Z=27 short-msg PASSES.
- **Step B — TX consumers read the member.** `streaming_encoder.cpp` consumers read
  `ldpc_lifting_z_` (modem_engine still sets it from env at this step). Behavior-neutral.
- **Step C — the policy authority (behavior-defining).** `Connection::selectBurstLiftingZ()`
  (traffic class, one env override inside) → chunker sites + `makeFixedDataFrame(z)` param +
  app pushes Z to `modem_engine` (replaces the `:573` env read). Remove the last env sites.
  Proof: cli short-msg → 27 byte-exact; GUI file → 81 byte-exact; an interactive OFDM msg →
  27. Multi-seed GUI, no regression.
- **Gate:** Tier-0 → Codex counter-check the full diff under the four-tier stack before merge.

## Invariants (must not break)
1. TX chunker Z **must equal** the encoder Z for every burst, or frames truncate / zero-pad.
2. RX deinterleaver block size **must equal** the encoder's, sourced from the descriptor.
3. Z ∈ {27, 81} only (defensive clamps already in `setLDPCLiftingZ`). Z=81 ⟹ cw_per_frame=2.
4. A frame decoded *before* its group's descriptor arrives falls back to Z=27 (legacy), not 81.
