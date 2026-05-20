# Zero-Retransmission AWGN Design Review

Date: 2026-05-20

Branch: `fix/zero-retx-awgn-2026-05-20`

Scope: Phase 0 investigation and design verdict only. No protocol fix is implemented in this
commit.

## Verdict

Claude's narrow hypothesis is refuted by the focused SNR=10 AWGN trace.

The retransmissions are timeout-triggered, but the timeout is not firing before a
consolidated SACK for frames that BRAVO already decoded. The first 8-frame burst
is decoded cleanly, BRAVO sends `SACK base=7`, and ALPHA receives that SACK
before it sends the remaining three DATA frames. The three retransmissions are
for seq=8..10, which BRAVO never decoded on their original transmission.

The load-bearing root cause is a physical-turn boundary bug: after BRAVO sends
the ACK/SACK turn, ALPHA immediately opens a new OFDM DATA burst using only a
light LTS data preamble. BRAVO is still in connected light-search behavior, but
its warm-sync prediction has been disturbed by the half-duplex ACK turn. On
clean AWGN this should not be treated as a channel FER failure; it is an
acquisition/re-anchoring failure at the protocol/DSP boundary. The ARQ timeout
then does what it is designed to do: it retransmits the unacknowledged tail
frames.

The best Phase 1 fix is therefore:

1. Re-anchor the first frame of every multi-frame OFDM burst with a full OFDM
   preamble, not only the first burst after negotiation.
2. Enable the existing short SACK timer for wide-OFDM stream-tail frames so a
   decoded final partial batch is acknowledged after carrier-sense coalescing,
   not after a full physical-window hold.
3. Do not increase the ARQ timeout. The current timeout already includes the
   configured physical SACK hold; increasing it would only hide missed tail
   bursts and add operator-visible dead air.

## Evidence From The Reproduction

Command:

```sh
./build/cli_simulator --ota-host 127.0.0.1:50151 --snr 10 --channel awgn --rate r1_4 --seed 42 --test --log-level info --log-category all --log-file /tmp/zero_retx_phase0_snr10_info.log
```

Result: delivery passed, but ALPHA reported `retransmissions=3` and
`timeouts=3`. The preserved sweep logs show the same timeout-only shape:
`RETX: timeout=3 fast_hole=0 hole_probe=0 nack=0` for SNR=10 seed 42.

Key timeline from `/tmp/zero_retx_phase0_snr10_info.log`:

| Time | Event | Meaning |
| --- | --- | --- |
| 13.235 / 20.089 | `ARQ window=8, timeout=12.45s ... physical_sack_hold=5214ms` | The sender timeout is already derived from the long wide-OFDM SACK hold. |
| 20.360 | `Full preamble forced for first burst frame OFDM timing anchor` | The first DATA burst has a full anchor. |
| 20.362 | `TX burst: 8 frames -> 299520 samples` | ALPHA sends the first full ARQ window. |
| 22.577 .. 26.620 | BRAVO receives DATA seq=0..7 | All eight original first-window DATA frames decode. |
| 26.620 | `SR-ARQ: Sent SACK base=7 bitmap=0x00000000` | BRAVO acknowledges the full first window. |
| 29.093 | ALPHA receives `ACK seq=7` | The first SACK arrives before the tail burst is transmitted. |
| 29.093 | `Connection: Flushing burst of 3 frames` | ALPHA advances the window and sends the tail. |
| 29.134 | `TX burst: 3 frames -> 90720 samples` | This burst has no full-preamble log entry. |
| 29.439 .. 31.405 | BRAVO repeatedly logs `DATA sync rejected`, corr 0.15..0.29 | BRAVO does not acquire the original tail burst. |
| 44.090 | ALPHA retransmits seq=8, seq=9, seq=10 with cause=timeout | ARQ timeout recovers frames BRAVO never decoded. |
| 44.853 .. 46.412 | BRAVO receives DATA seq=8..10 | The timeout retransmissions decode. |
| 51.021 | BRAVO sends `SACK base=10` | Final SACK happens only after the timeout retransmissions. |

This timing rules out the specific "BRAVO received all frames and delayed the
consolidated SACK past ALPHA's timeout" diagnosis for this trace. There was no
pre-timeout SACK to send for seq=8..10 because BRAVO did not decode those
original frames.

## Code Evidence

Sender-side timeout accounting starts when ARQ frames enter the transmit window,
before they are handed to the modem queue:

- `src/protocol/selective_repeat_arq.cpp:217-247` stores fixed DATA frames,
  sets `timeout_ms = currentAckTimeoutMs()`, records `first_tx_ms`, then calls
  `transmitData()`.
- `src/protocol/selective_repeat_arq.cpp:841-850` retransmits active,
  unacknowledged slots when the timeout expires.

Receiver-side SACK behavior already has the right policy shape, but wide OFDM
does not currently use the tail-short path:

- `src/protocol/selective_repeat_arq.cpp:425-466` sends SACK immediately for
  out-of-order or eligible batch-threshold events, otherwise arms
  `sack_timer_ms_`.
- `src/protocol/selective_repeat_arq_policy.hpp:100-112` supports a shorter
  timer when `sack_delay_short_ms != 0 && !frame_more_frag`.
- `src/protocol/connection.cpp:1658-1663` configures wide OFDM with
  `wideOFDMSackDelayMs(...)` but then sets `setSackDelayShort(0)`, preserving
  the long delay for tail frames.

The wide-OFDM timeout is not a random constant:

- `src/protocol/connection_policy.hpp:312-320` computes the physical SACK hold
  as `window_size * data_ms + kCarrierSenseSackCoalesceMs`.
- `src/protocol/connection_policy.hpp:368-395` computes the ACK timeout from
  the full TX burst, repeated ACK path, configured SACK delay, and decode/audio
  margin.

The burst path sends the tail as a new multi-frame physical DATA turn:

- `src/protocol/connection.cpp:771-824` pipelines OFDM fragments until the ARQ
  window fills or all pending fragments are submitted, then flushes the burst.
- `src/protocol/connection.cpp:1911-1947` sends a buffered OFDM burst through
  `on_transmit_burst_` when the buffer has more than one frame.

The encoder only emits a full preamble for a burst when a one-shot flag is set:

- `src/gui/modem/streaming_encoder.cpp:393-413` sets `force_first_full_preamble`
  from `force_full_preamble_once_`. Without that flag, the first burst frame
  uses `generateDataPreamble()` when the waveform supports light data preambles.
- `tools/sim/simulated_station.hpp:1376-1381` sets the one-shot full-preamble
  flag at connected OFDM negotiation. That explains the first burst's full
  anchor, but it does not re-arm the flag after each later ACK/SACK turnaround.

The decoder side then behaves consistently with that transmit choice:

- `src/gui/modem/streaming_sync_acquisition.cpp:465-529` uses connected
  light-sync acquisition after the connected OFDM anchor and explicitly has no
  chirp fallback in that path.
- `src/gui/modem/streaming_signal_policy.hpp:119-170` rejects weak connected
  wide-OFDM light-sync candidates below the configured confidence thresholds.
  The trace's original-tail correlations of 0.15..0.29 are correctly rejected.
- `src/gui/modem/streaming_decoder.cpp:266-288` degrades and eventually clears
  warm-sync prediction after misses.
- `src/gui/modem/streaming_frame_arrival_policy.hpp:151-214` only narrows the
  warm search when warm-sync is active, predicted, and sufficiently confident.

## Alternative Diagnoses Checked

BRAVO's first SACK was not lost. BRAVO sends `SACK base=7` at 26.620, and ALPHA
receives ACK seq=7 at 29.093 and again at 29.179. ALPHA advances the base to 8
and suppresses the duplicate.

ALPHA's timeout state is not the primary bug in this trace. The timeout fires
only for active, unacknowledged seq=8..10 slots after BRAVO failed to decode
their original transmissions. The retransmissions are redundant from an AWGN
operator perspective, but they are correct ARQ recovery for frames that never
entered BRAVO's ARQ receive window.

The deferred-SACK coalescing fix did not drop a SACK for seq=8..10. No SACK for
those frames could be generated before timeout because no original seq=8..10
DATA frames were decoded.

A burst-interleaver group boundary is not the root cause for this acceptance
case. The trace is R1/4 with no burst-interleave group marker on the failed tail
burst, and the first eight frames decode without LDPC failures.

## Four-Perspective Review

### 1. PHY Theorist

The raw channel is not failing the first-window frames. On AWGN SNR=10, the
trace cleanly decodes all eight initial DATA frames and the timeout
retransmissions decode cleanly too. The failed original tail shows acquisition
rejections, not LDPC/frame-decode failures.

The deterministic 3-retransmission shape across seeds is therefore a PHY access
state problem, not a random FER problem. The receiver is being asked to acquire
a new physical DATA turn using only connected light sync after its own ACK turn
has interrupted the previous cadence.

Principled PHY fix: put a full chirp+LTS anchor at the start of each multi-frame
OFDM burst. Subsequent frames in that same burst can continue using light data
preambles because the physical cadence has been re-established.

### 2. Real-Time DSP Systems Engineer

The original concern about the SACK timer is valid as a class of failure, but it
is not the load-bearing failure in this trace. The sender receives the first
SACK before transmitting the 3-frame tail, so the race is not "timeout beats
SACK" for already decoded data.

The actual race is at the half-duplex physical-turn boundary. BRAVO finishes an
8-frame receive burst, transmits repeated ACK/SACK control frames, and then must
return to receive mode. ALPHA immediately sends a new 3-frame DATA burst. The
decoder remains in connected light-search behavior, but its warm prediction has
lost a reliable continuous DATA cadence. With no full anchor on the new DATA
turn, the original tail is missed.

The existing SACK timing contract still needs one correction in Phase 1. Wide
OFDM currently configures a long physical-window SACK delay and disables the
short tail timer. Once the tail is acquired correctly, the final partial batch
should use the existing `MORE_FRAG=0` short-timer path, with carrier sense still
preventing ACK transmission into an active sender burst.

The timeout formula already accounts for the long SACK delay. Increasing the
timeout would be an unprincipled workaround because it would only wait longer
before recovering a missed physical turn.

### 3. Veteran HF Operator

On a clean channel, wasting airtime on three duplicate DATA frames plus the
timeout wait is visible and unacceptable. A full anchor at the start of a new
multi-frame OFDM DATA turn costs less airtime than a 12.45-second timeout and
three duplicate frames.

On fading channels, retransmissions remain expected when frames are actually
lost. This design does not suppress retransmissions, disable SACK
consolidation, or make ACK behavior channel-class conditional. It gives the
receiver a reliable reacquisition point after a TX/RX turn and keeps ARQ's
recovery behavior intact.

### 4. First-Principles Timing

For this mode, the log reports:

- DATA frame time: 648 ms
- ACK frame time: 216 ms
- ACK repeats: 3
- ARQ window: 8
- Physical SACK hold: `8 * 648 ms + 30 ms = 5214 ms`
- Sender timeout: 12.45 s

The first burst starts at 20.362, BRAVO sends SACK base=7 at 26.620, and ALPHA
receives it at 29.093. That is before the tail burst starts at 29.134, so the
first-window timing is already safe.

For the tail, no SACK exists before timeout because BRAVO does not acquire
seq=8..10. If Phase 1 anchors that tail burst, the receiver should decode the
tail within the physical duration of that 3-frame burst. Even with the current
long 5.214-second tail SACK delay, the response should still arrive before the
12.45-second timeout. Enabling the existing short tail SACK timer adds margin
and matches the stream-tail policy without changing channel behavior.

## Phase 1 Change Plan

### Change 1: Full Anchor At Multi-Frame OFDM Burst Start

Modify `StreamingEncoder::encodeBurstLight()` so the first frame of every
multi-frame OFDM burst uses `waveform_->generatePreamble()` when the waveform
supports a lighter data preamble. Subsequent frames continue using
`generateDataPreamble()`.

Implementation notes:

- Preserve `force_full_preamble_once_` for single-frame reconnect/control cases.
- Keep MC-DPSK behavior separate; this change targets OFDM burst encoding.
- Preserve or fix the LTS burst-marker handling so a full preamble and a
  burst-interleave group-start marker do not corrupt the chirp portion.
- Add a focused unit or integration assertion that a 3-frame OFDM burst starts
  with a full anchor and later frames remain light.

### Change 2: Enable Wide-OFDM Tail SACK Short Delay

Modify `Connection::configureArqForCurrentDataMode()` for wide OFDM so
`setSackDelayShort(...)` uses a derived carrier-sense coalescing delay instead
of `0`. The intended value is `connection_policy::kCarrierSenseSackCoalesceMs`
or a named wrapper such as `wideOFDMSackTailDelayMs()` that resolves to the same
principle: once `MORE_FRAG=0` has been decoded, wait only for local
carrier-sense/coalescing, not a full physical window.

Implementation notes:

- Do not make this AWGN-only or simulator-only.
- Leave the long `wideOFDMSackDelayMs(...)` for in-burst frames.
- Keep the sender timeout formula derived from the configured long SACK delay.
  No timeout increase is part of this design.
- Update policy/unit tests to cover wide-OFDM tail short delay if current tests
  do not already assert the connection-level configuration.

## Phase 2 Verification Plan

After Phase 1 implementation:

```sh
cmake -S . -B build -DULTRA_BUILD_GUI=OFF
cmake --build build -j4
```

Run the acceptance sweep:

```sh
for snr in 10 12 14 16 20 24; do
  for seed in 42 100 200; do
    ./build/cli_simulator --ota-host 127.0.0.1:50151 --snr "$snr" --channel awgn --rate r1_4 --seed "$seed" --test
  done
done
```

Required result: all 18 cells pass with delivery 100% and `retransmissions=0`.

Run the ctest suite while excluding only the two documented pre-existing OTA
fixture failures:

```sh
ctest --test-dir build -E "OTASimulatorTwoEndpointNoisy|OTASimulatorTwoEndpointMCDPSKLowSNR"
```

Run Good fading SNR=15 sanity checks across seeds 42, 100, and 200. Expected
result: all pass cleanly. Retransmissions are not required to be zero on fading,
but they must not regress materially from the current behavior.

## Ship/Defer Gate

Proceed to Phase 1 only if both changes stay local to the burst anchor and ARQ
tail-SACK configuration. If implementing the anchor uncovers a deeper coupling
with burst interleaving or warm-sync prediction that cannot be fixed without
retouching warm-sync LTS detection, stop and defer rather than shipping a
timeout-only workaround.
