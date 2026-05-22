# HF Link-Layer Turnaround / ARQ Redesign Review (2026-05-22)

Branch reviewed: `feat/16qam-promotion-2026-05-21`

Scope: design review only. No PHY/protocol source was changed. I did run a
read-only `cli_simulator` probe to reproduce the motivating cell.

## Executive Verdict

I agree with the diagnosis that the current link is wasting too much wall-clock
time waiting for feedback on a half-duplex channel. I do not agree that a
derived RTO alone should be expected to recover most of the 65 percent dead air.
The current evidence says the dead air is coupled to real fade loss, feedback
loss, receive/transmit queue timing, and conservative SACK scheduling. Existing
Phase-1 timing prototypes already showed that plausible shorter/longer timing
changes can reduce one symptom while regressing trusted end-to-end goodput.

I partially agree with the proposed architecture. Scheduled turnaround is the
right direction if it means an explicit half-duplex transaction cycle anchored to
actual burst/sample timing, with guarded feedback windows and listen-before-talk
behavior. It is not a replacement for loss recovery: if the receiver misses the
burst, it also misses the schedule, so the sender still needs a no-feedback
recovery path.

I agree that full Raptor/RaptorQ should not be the default for this small,
point-to-point HF use case. I would keep a smaller systematic block-erasure code
option on the backlog for severe feedback blackouts or broadcast/multicast, but
the primary path should be scheduled coded ARQ/HARQ with hardened feedback.

My ranked architecture is:

1. Explicit transaction cycles with announced/derived reply windows, cycle IDs,
   listen-before-talk gates, and sender no-feedback fallback after the scheduled
   reply window.
2. Hardened feedback as a repeated/coded feedback window, not a single fragile
   ACK packet.
3. Time-separated code-combining repair: start from the current selective
   per-codeword repair and Chase-combine mechanics, then add true incremental
   redundancy only after a rate-compatible LDPC redundancy-version design exists.
4. Deterministic frequency and time diversity interleaving sized against measured
   coherence time, with latency caps for chat/forms.
5. Optional small-block erasure coding as a mode, not the core ARQ replacement.

## Evidence From This Branch

Reproduced command:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 5120 --seed 44 --log-level error
```

Result matched the motivating finding: file delivered and verified, end-to-end
goodput 360 bps, 65 percent dead air, ALPHA `retransmissions=27`,
`timeouts=26`, `failed=0`, and BRAVO data `frame_success=82.6%`.

Relevant code facts:

- SR-ARQ timestamps and arms each TX slot before `transmitData()`: fixed OFDM
  send assigns `timeout_ms=currentAckTimeoutMs()` and `first_tx_ms=arq_time_ms_`
  before the callback in `src/protocol/selective_repeat_arq.cpp:239-285`.
- The timeout path is silent until the per-slot timer expires, then increments
  `stats_.timeouts` and calls `retransmitFrame()` in
  `src/protocol/selective_repeat_arq.cpp:984-1005`.
- The adaptive estimator is Karn-safe and RFC-6298-like, but its floor is forced
  up to `configured_ack_timeout_ms` and its ceiling is `max(12000,
  configured_ack_timeout_ms)` in `src/protocol/selective_repeat_arq_policy.hpp:193-217`.
- `currentAckTimeoutMs()` simply returns the adaptive value if present, else the
  configured value, in `src/protocol/selective_repeat_arq.cpp:1531-1535`.
  Therefore a later physical floor must either be applied on every read or the
  estimator must be reset/renormalized on mode/window/CW changes.
- OFDM file sends fill the ARQ window and flush the physical burst afterward in
  `src/protocol/connection.cpp:758-797`, so timer start is not the same as
  actual last keyed sample.
- Wide OFDM sets SACK delay to a full window of data airtime plus the carrier
  sense coalescing margin in `src/protocol/connection_policy.hpp:320-327`, then
  computes timeout from TX burst, ACK copies, SACK delay, and decode/jitter
  margin in `src/protocol/connection_policy.hpp:380-408`.
- The current v2 data/control frame formats have no explicit turn-boundary or
  reply-window fields in `src/protocol/frame_v2.hpp:421-548`.
- The current "repair" path sends selected original information codewords in a
  `DATA_REPAIR` frame, not fresh parity, in
  `src/protocol/selective_repeat_arq.cpp:1179-1255`.

Existing branch documents are also material:

- `docs/FAILURE_ATTRIBUTION_2026_05_22.md:3-8` says the QAM16 R1/2 Good
  failures are fade/SNR-limited, not primarily channel-estimation-limited.
- `docs/THROUGHPUT_RECOVERY_2026_05_22.md:24-50` records rejected physical and
  queue-aware RTO prototypes on the 20 KB bad seed.
- `docs/THROUGHPUT_RECOVERY_2026_05_22.md:73-132` records rejected shorter SACK
  and threshold-SACK prototypes.
- `docs/HARQ_AUDIT_2026_05_19.md:71-75` notes that slow Good-channel fading
  makes closely spaced Chase retransmissions less independent than ideal HARQ
  theory assumes.

## Prior Art Anchors

- RFC 6298 is the named SRTT/RTTVAR RTO prior art, but TCP's assumptions are not
  half-duplex HF. Its own rationale is to avoid spurious retransmissions while
  still recovering loss: https://www.rfc-editor.org/rfc/rfc6298.html
- STANAG 5066 uses selective-repeat ARQ for HF data transfer; this validates the
  current high-level ARQ family, not the current timing constants:
  https://ham.zmailer.org/oh2mqk/HF-data/stanag5066.pdf
- STANAG 4538 xDL is the closer prior art for this redesign: point-to-point HF
  ARQ, robust ACK waveforms, and code-combining ARQ. FFI's public report
  describes HDL/LDL packet frames, ACK/selective ACK, code combining, and robust
  ACK waveforms: https://kudos.dfo.no/documents/51047/files/33495.pdf
- PACTOR is a practical half-duplex HF ARQ precedent with a fixed synchronous
  data/control-signal cycle and explicit idle time for turnaround:
  https://www.arrl.org/pactor
- MIL-STD-188-110D uses defined interleaver matrices and multiple interleaver
  depths for HF modem robustness; this supports principled interleaver design
  rather than ad hoc burst spreading:
  https://quicksearch.dla.mil/WMX/Default.aspx?token=5792474
- Raptor/RaptorQ are named fountain-code prior art. RFC 5053 describes Raptor as
  a fountain code that can generate as many encoding symbols as needed, and RFC
  6330 is the RaptorQ FEC scheme:
  https://www.rfc-editor.org/rfc/rfc5053 and
  https://datatracker.ietf.org/doc/html/rfc6330
- For US amateur operation, 47 CFR 97.101 requires good engineering/good amateur
  practice and avoiding willful interference; 47 CFR 97.221 constrains
  automatically controlled HF digital operation. A scheduled ACK window is not a
  license to ignore channel occupancy:
  https://www.ecfr.gov/current/title-47/part-97/section-97.101 and
  https://www.ecfr.gov/current/title-47/part-97/section-97.221

## 1. Scheduled Turnaround

Verdict: sound in principle, but only if it is a scheduled transaction cycle,
not just a header hint. I agree with Claude's direction and disagree with any
claim that it fully removes dead air or collision risk.

PHY theorist: scheduled feedback is not extra channel capacity. If the forward
burst is erased, the receiver has no schedule and cannot reply. If the feedback
burst is erased, the sender still needs a timeout. What scheduling buys is
removal of self-inflicted uncertainty: the sender can wait until the end of a
known reply window instead of guessing whether a late ACK is still in flight.

Real-time DSP systems engineer: anchoring to the burst, not wall clock, is the
right call. With independent 48 kHz soundcards at 50 ppm each, worst-case
relative drift is about 100 ppm. Over a 20 s over that is 2 ms, or 96 samples.
That is small compared with VOX/CAT/PTT and audio-buffer margins, but only if
the schedule is relative to observed burst/sample timing and has explicit guard.
The current code starts ARQ timers at frame submission, before the burst flush
and before actual keyed samples, so a future schedule must expose "actual
on-air start/end" or a conservative queue budget. Otherwise the schedule will
inherit the same error in a cleaner-looking form.

Veteran HF operator: a real radio is not a deterministic full-duplex wire. VOX
hang, relay timing, CAT latency, ALC settling, and external QRM all matter. The
receiver must still listen before transmitting in the scheduled feedback slot.
If the slot is busy, it should skip or defer with a bounded backoff instead of
blindly keying. The sender then treats the missed slot as no feedback and
continues with the recovery cycle.

First-principles escape hatch: scheduling reduces uncertainty and collision
probability; it cannot recover information that was not received. A design proof
must separate "no schedule because burst missed", "schedule known but CCA busy",
"feedback sent but erased", and "feedback received late".

Required semantics:

- Every forward burst carries a cycle ID, sequence range, final-sample marker or
  equivalent burst-length descriptor, reply-window offset, reply-window duration,
  and guard class.
- Put the schedule in a very robust preamble/header and repeat it in a trailer
  if possible. A receiver that decodes only the trailer can still answer.
- Sender remains receive-only through the reply window. Its no-feedback timer
  starts after the reply window closes, not when the data frame is queued.
- Receiver transmits feedback only inside the reply window and only after CCA.
- Missed forward burst means no receiver reply. Sender falls back to a robust
  poll/repeat or lower-rate repair cycle.

Named prior art: PACTOR fixed half-duplex data/control cycles, STANAG 4538 xDL
forward frame plus ACK/selective ACK cycles, TDMA guard-time practice, and
slotted-ALOHA/TDMA collision analysis for guard bands.

## 2. Fountain/Raptor Rejection

Verdict: rejecting full fountain/Raptor as the default is correct. I would keep a
small-block systematic erasure-code option on the backlog, because it is useful
when feedback is the bottleneck, but I would not make RaptorQ the core link
layer.

At the reproduced 5 KB QAM16 R1/2 cell, the sender used 19 original ARQ frames
for 5120 bytes, about 269 payload bytes per source symbol. Treating each ARQ
frame as a fountain source symbol gives K around 19 for the whole object.
RaptorQ's asymptotic story is excellent, but at K=19 one additional symbol is
about 5.3 percent overhead and two symbols are about 10.5 percent overhead
before any robust framing, source-block IDs, ESI fields, and object metadata.
At the measured 1020 bps on-air goodput, one 269-byte extra symbol is roughly
2.1 s of keyed airtime; at 360 bps end-to-end it is roughly 6 s of transfer
time. That can beat a long ACK blackout, but it is wasteful when only one
codeword or one frame needs targeted repair.

PHY theorist: fountain codes operate naturally on erasures after inner decoding.
They do not exploit soft LLR evidence from failed LDPC codewords unless you add a
separate soft outer decoder, which is not the normal RaptorQ model. IR-HARQ/code
combining uses the receiver's analog evidence more directly.

Real-time DSP systems engineer: full RaptorQ adds source-block construction,
symbol IDs, object-level buffering, decode matrices, and new failure modes. For
KB-scale messages it also delays delivery until enough block symbols arrive.

Veteran HF operator: for a 1-5 KB form/chat/file, waiting for the whole object
to decode feels worse than progressive ARQ unless the channel is in a feedback
blackout. Operators tolerate a few scheduled repair cycles more readily than a
large opaque block transfer that either pops complete or stalls.

Middle ground: add a systematic Reed-Solomon/RLNC/RaptorQ-style small-block mode
only after scheduled feedback is measured. Use it when feedback FER is high,
when transmitting to multiple receivers, or when a "send N data + M repair
symbols then listen" mode beats interactive repair. That is not Step 1.

## 3. IR-HARQ And Time-Diversity Interleaving

Verdict: the right direction is coded ARQ with time diversity, but the current
implementation is not yet IR-HARQ. Claude is correct about the target class of
solution and too optimistic about how directly it maps to this codebase.

What exists now:

- Chase combining retains and sums same-codeword LLRs when the frame/CW can be
  identified. The design and limitations are documented in
  `docs/HARQ_SOFT_COMBINE_2026_05_19.md:5-21` and
  `docs/HARQ_SOFT_COMBINE_2026_05_19.md:72-85`.
- `DATA_REPAIR` sends selected original information codewords, not new parity,
  in `src/protocol/selective_repeat_arq.cpp:1179-1255`.

True IR-HARQ requires redundancy-version identity, rate-compatible parity or
puncturing/extension support, decoder support for changing effective code rate,
and an interleaver/mapper that keeps every redundancy version identifiable and
combinable. With 802.11n N=648 LDPC, do not assume the existing R1/2, R2/3, and
R3/4 modes are a drop-in nested mother-code ladder until the matrices and
puncturing/shortening contract prove it.

Time diversity is necessary because Good/Moderate fading coherence is on the
same order as current retransmission cycles. Sending a Chase or IR repair
immediately inside the same fade does not buy independent evidence. A scheduled
repair cycle should deliberately space repair parity/copies across a different
channel realization when the coherence estimator says the fade is slow.

Better-known method family:

- STANAG 4538 xDL code-combining ARQ: established HF prior art for point-to-point
  data links with robust ACKs.
- Type-II/III HARQ with incremental redundancy: correct theory if the LDPC
  redundancy-version machinery exists.
- Chase combining: lower implementation risk and already partly present.
- MIL-STD-188-110 style interleaver-depth selection: correct prior art for
  spreading burst/fade errors, but must be latency-bounded.

The interleaver must spread each codeword across frequency and time without
turning one whole fade into an entire object failure. The current failure
attribution says to inspect whether each CW spans the full carrier set and
multiple symbols; that should happen before adding a longer time interleaver.

## 4. Step 1: Derived RTO

Verdict: yes as a correctness and measurement move; no as a guaranteed
throughput win. I disagree with "recovers most of the 65 percent cheaply" as a
claim until it passes the gates below.

A derived RTO is principled if the computed floor is:

```text
timer_start_to_on_air_end
+ receiver decode latency budget
+ scheduled/allowed SACK coalescing window
+ receiver T/R and PTT/key-up budget
+ ACK airtime * ACK copies
+ sender RX/audio-buffer budget
+ drift/implementation margin
```

The trap is that several of those terms are not currently represented at the
same layer. `ARQConfig::turnaround_ms` exists in `src/protocol/arq_interface.hpp:18-32`,
but SR-ARQ's active timing is mostly configured through ACK timeout and SACK
delay. Wide OFDM SACK delay is already tied to a physical window in
`src/protocol/connection_policy.hpp:320-327`, and ARQ slot timers start before
the physical burst is flushed. A derived RTO that ignores queue-to-air timing can
still be wrong by seconds.

The adaptive estimator interaction is also nontrivial:

- If `adaptive_ack_timeout_ms_` is already set, `setAckTimeout()` updates the
  configured value but does not immediately rewrite the adaptive value.
- `updateRTO()` uses the configured timeout as a future floor only when a valid
  RTT sample arrives.
- Retransmitted frames are Karn-ineligible, so a loss-heavy transfer can run a
  stale adaptive RTO for a long time.

Therefore the safe design is not "replace constants in `updateRTO()` only." It
is:

- Keep SRTT/RTTVAR for observed variation.
- Maintain a physical floor/ceiling in ARQ state and apply it in
  `currentAckTimeoutMs()` on every read.
- Reset or rebase SRTT/RTTVAR when modulation, code rate, CW count, window,
  ACK-repeat count, SACK policy, or waveform changes.
- Start no-feedback recovery from actual burst/reply-window timing, not from
  enqueue time, once scheduled turnaround exists.

## 5. Ranked Optimal Architecture

1. Transaction-cycle scheduler. Add an outer cycle state machine above SR-ARQ:
   `DATA_CYCLE(seq_range, cycle_id, reply_offset, reply_duration, guard_class)`,
   `FEEDBACK_CYCLE(cycle_id, cumulative_ack, sack/need_more, quality)`, and
   `NO_FEEDBACK_RECOVERY`. This can be implemented without changing the inner
   OFDM+LDPC bit-error code. It must be integrated in one common connection
   state machine path before more triplicated plumbing is added.

2. Hardened feedback window. Use the most robust available control waveform or
   a repeated/coded 1-CW feedback message. Feedback should be a compact
   cumulative ACK plus SACK/need-more bitmap, repeated across the feedback
   window with jitter/spacing. A single "need-more/done" packet is too brittle.

3. Code-combining repairs. First make the current per-CW repair and Chase
   combining work under scheduled cycles. Then prototype true IR-HARQ as a
   separate redundancy-version design. Do not call original-codeword repair
   "fresh parity."

4. Diversity mapper/interleaver. Ensure each LDPC CW sees the whole carrier set
   and enough OFDM symbols. Add time-depth only after measuring coherence, with
   presets such as short/medium/long tied to estimated Doppler/fade statistics
   and maximum operator latency.

5. Small-block outer erasure mode. For severe feedback loss or multicast, use a
   systematic block code over ARQ chunks. Start with simple RS/RLNC-style tests
   before importing full RaptorQ complexity.

6. Carrier-sense etiquette and third-party protection. A scheduled two-station
   link still shares HF spectrum. CCA before every scheduled transmit, abort on
   busy channel, and record "busy-skipped feedback" as a first-class metric.

## 6. Prototype And Measure Plan

Prototype A: observability before behavior.

- Add diagnostics only: per-cycle timer start/end, physical burst enqueue time,
  actual keyed sample interval, predicted ACK window, actual ACK keyed interval,
  CCA busy/clear, and collision/overlap markers in OTASim.
- Gate: no protocol behavior change; reproduced QAM16 R1/2 Good/SNR20 5 KB
  seed44 still reports the same pass/fail and similar stats. The new log must
  classify all 26 timeout waits into "real missing feedback", "late feedback",
  "feedback erased", "sender not actually listening", or "timer aged before
  air."

Prototype B: derived physical RTO only.

- Apply a physical floor/ceiling on every `currentAckTimeoutMs()` read and reset
  estimator state on mode/window/CW/SACK-policy changes.
- Do not shorten SACK holds or ACK repeats in this prototype.
- Gate across QAM16 R1/2 Good/SNR20 5 KB seeds 42/43/44 and 20 KB seeds
  42/43/44: delivery remains 100 percent; no max-retry failures; mean
  end-to-end goodput improves at least 15 percent; worst-seed end-to-end goodput
  does not regress more than 5 percent; timeout count drops at least 40 percent;
  BRAVO frame_success does not drop more than 2 percentage points; collision/
  overlap markers remain zero.

Prototype C: scheduled feedback window in simulator.

- Add cycle IDs and reply windows to an experimental frame extension or sideband
  metadata first, then wire format after the simulator proves it.
- Sender is prohibited from transmitting during the feedback window.
- Receiver sends feedback only inside the window and only if CCA is clear.
- Gate: on the same 5 KB and 20 KB QAM16 cells, collision/overlap markers are
  zero, busy-channel skips are explicitly counted, end-to-end goodput improves
  at least 25 percent mean and 15 percent worst-seed, and no seed loses delivery.

Prototype D: scheduled code-combining repair.

- First use existing Chase/per-CW repair with repair cycles spaced by measured
  coherence. Then compare a true IR-HARQ redundancy-version prototype only if
  the LDPC support is real.
- Gate: for frames classified as fade/SNR-limited, retransmissions per delivered
  KB drop at least 30 percent without reducing BRAVO frame_success or increasing
  final-ACK loss. HARQ key-build misses must be separately reported, because
  missed CW0 means no safe combine key today.

Prototype E: interleaver/mapping audit.

- Trace coded-bit placement from LDPC CW to OFDM carrier/symbol for QAM16 R1/2
  Good failures. Confirm whether every CW spans the whole carrier set.
- Gate: if any CW is frequency-localized, a deterministic permutation must raise
  5 KB QAM16 R1/2 Good/SNR20 seed43/44 BRAVO frame_success by at least 5
  percentage points with no 5 KB seed42 regression. If each CW already spans the
  band, stop blaming the mapper and move to time diversity/repair scheduling.

## Final Disagreements With Claude

- Scheduled turnaround is necessary but not sufficient. It removes self-timing
  ambiguity, not fade erasures, missed schedules, third-party occupancy, or lost
  feedback.
- A derived RTO is the right first measurement/correctness move, but the branch
  already has evidence that timing-only prototypes can regress goodput. Treat it
  as a falsifiable hypothesis, not a cheap recovery guarantee.
- Current repair is not IR-HARQ. The code sends selected original codewords and
  optionally Chase-combines; true fresh parity needs new LDPC redundancy-version
  machinery.
- "One feedback event" should mean one protected feedback window, not one fragile
  packet. HF control feedback deserves the robust waveform/repetition treatment
  seen in STANAG 4538-style systems.
- Full RaptorQ is overkill as the default, but a small systematic erasure-code
  mode is a useful escape hatch for feedback-blackout cases.
