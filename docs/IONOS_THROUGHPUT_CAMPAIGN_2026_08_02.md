# IONOS MPG@20 transfer campaign — 2026-08-02

## Executive result

This campaign exercised the real Pi 5 audio -> IONOS -> Mac audio path with a
51,200-byte file, then used both endpoint logs to find and repair protocol losses.
Eight completed transfers were byte-identical. Four matched source snapshots produced
the six named final diagnostics below:

- `final_default_05`: **2.32 kbps delivered**, 17/17 physical bursts decoded, no
  partial groups, craters, missing callbacks, collisions, or rate-change requeues.
- `final_default_06`: **1.62 kbps delivered**, 27/27 physical bursts decoded, four
  selectively repaired partial groups, and no craters, missing callbacks, collisions,
  or rate-change requeues.
- `final_psk8_02`: **1.51 kbps delivered** with 8PSK R2/3 proved on both endpoints
  before payload egress. It completed 27/27 bursts but needed 15 selective repairs
  and suffered four whole-group craters.
- `final_default_07`: **1.50 kbps delivered** under automatic policy, 27/27 bursts,
  six selective repairs, one crater, and two rate-change requeues. The second requeue
  exposed a deterministic short-tail climb defect described below.
- `final_default_08`: **2.26 kbps over the 181.1 s application receive timer** on the
  fixed9 tail-guard build; the distinct first-key-down -> CRC-completion span was
  188.884 s, or **2.169 kbps**. It completed 18/18 bursts with one selectively repaired
  partial group and no crater, missing callback, collision, or rate-change requeue.
- `final_default_09`: **1.54 kbps over the 265.6 s application receive timer** on the
  fixed10 teardown build; its first-key-down -> CRC-completion span was 273.358 s, or
  **1.498 kbps**. It completed 28/28 bursts with eight selectively repaired partial
  groups and no crater, missing callback, data collision, or rate-change requeue. Only
  the Pi caller initiated teardown; it sent one scheduled retry before an ACK returned,
  and both applications disconnected and quit cleanly without a crossed close.

A later matched-build, order-balanced two-pair follow-up isolated the experimental
8PSK R2/3 long-code profile: logical cw12/Z27 versus physical cw4/Z81 with identical
629-byte payload capacity and 7,776 coded bits per frame. **Pooled-duration sender
completion goodput**—two 51,200-byte transfers divided by the sum of their durations—was
**1.517 kbps for baseline and 1.920 kbps for Z81 (+26.6%)**. The individual paired results
were 1.50 -> 2.24 kbps and 1.53 -> 1.68 kbps. All four transfers were byte-exact after one
runner filename false negative was rechecked with `cmp` and SHA-256. Both paired directions
are positive, but **n=2 is not a graduation result on this rig**;
`ULTRA_8PSK_LONG_LDPC` remains default-OFF pending a properly powered campaign and
Moderate-channel coverage. The detailed follow-up, including why the earlier negative
two-pair run is invalid as a code-profile comparison, is recorded below.

A still-later immutable snapshot exercised both long profiles directly. Forced QPSK R3/4
cw3/Z81 (`qpsk_r34_z81_01`) delivered the same 51,200-byte payload byte-exact at
1.85 kbps on the application timer / 1.64 kbps over the physical transfer span. Forced
8PSK R2/3 cw4/Z81 (`8psk_z81_01`) was also byte-exact at 1.83 / 1.77 kbps. These are
single sequential realizations, not an A/B: QPSK observed nearby RX SNR around 11 dB and
8PSK around 8 dB. The QPSK trace exposed a software acquisition error on its first
descriptor; the 8PSK trace had nine partial groups and two craters. The current acquisition,
current-group-anchor, and per-request-anchor repairs were made **after** that frozen
snapshot and have no fresh IONOS validation yet. Both profile knobs remain default-OFF.

For benchmark scale, the Z81 arm's **pooled receiver-application goodput** was 2.106 kbps,
or 68.3% of the **project-recorded** 3,085.6 bps VARA MPG@20 benchmark. The 2.36 kbps
pair-1 result is retained below as a single-run observation, not the campaign headline.
That benchmark is not a measurement of this implementation, and its timer contract is not
established as identical to this receiver callback timer. The old
"~2,450 bps ceiling" was a stale `0.593` scheduling model that also counted all 59
occupied carriers as payload. Current geometry has 51 data carriers, and the measured
8PSK R2/3 raw PHY rate is about 4,250 bps. A 3,000 bps delivered result is therefore
mathematically possible at that rung only if total end-to-end efficiency exceeds 70.6%;
the live forced run shows that reliable operation in correlated fading, not a nominal
raw-rate shortage, is the present obstacle.

The original named diagnostics do **not** establish a causal whole-transfer throughput
claim. Those runs are sequential, unpaired fading realizations. In particular, `_05` and `_06` used
identical binaries yet differed by 0.70 kbps because one realization reached and held
8PSK while the other remained at QPSK R2/3; `_02` and `_07` also used identical binaries
but exercised forced and automatic policies in different realizations. The later Z81
follow-up is order-balanced and paired, but two pairs remain far below the declared sample
size needed for an expected-throughput claim.

The campaign did establish concrete correctness improvements: the fixed6 matched runs
removed the observed early-feedback collision, unintended long rate-change requeue stalls,
missing physical group completions, and the false `4/6` group caused by decoding trailing
silence after a lost descriptor. The evidence for those statements is the mechanism trace
and the absence of the exact failure signatures in the validating runs, not the headline
bps delta. Fixed8 then exposed a separate, narrower short-tail economic defect.
The force-handshake repair was additionally proved by `_02`. The short-tail climb repair
is deterministic-boundary validated, and the fixed9 matched build completed `_08` without
regression. The exact late-tail climb opportunity did not recur in `_08`, so the boundary
test—not that live realization—remains the direct proof of the hold mechanism. Fixed10
then repaired the simultaneous completion-driven close exposed after `_08`; `_09` is the
matched live proof of one-owner teardown through a real scheduled retry/ACK exchange.

## Scope and measurement contract

- Channel: the operator-configured Winlink-team IONOS audio simulator at MPG@20.
  The profile is external GUI state and is not serialized in the modem logs, so this
  report records the operator setting rather than claiming it was machine-verified.
- Sender: Pi 5 station, `/home/math/testfile_50k.bin`.
- Receiver and rate authority: Mac station.
- Payload: 51,200 bytes.
- Reference MD5: `c48be6e5657be076dd55f49388522a9d`.
- Reference/received SHA-256:
  `01a5cb8e7cf049bc74cc3518d7fc63038c5ef0c643e310f6547474ea8e427539`.
- Primary application rate: the receiver's CRC-clean duration from its delivered
  `FILE_START` callback to completion. Delivery-as-unit means the first physical burst can
  already have occupied the channel before this timer begins; the logged application rate
  remains a valid outcome but is not used as a physical-airtime denominator.
- Physical accounting: first sender DATA key-down to receiver CRC completion. Key-down
  intervals are clipped at completion for delivery-span duty and headroom; any final TX
  tail beyond that callback is reported separately. The sender's own completion timer is
  retained below as a separate endpoint outcome, not mixed into either receiver clock.
- Burst timing: sender and receiver clocks are aligned by multi-burst consensus in
  `tools/analyze_transfer.py`; physical TX sample counts determine key-down duration.
- ACK timing: current reports use exact application audio-queue commits. A commit is
  **not** a DAC/on-air timestamp; the analyzer labels the later stages separately.

All completed runs received exactly 51,200 bytes with CRC, MD5, SHA-256, and `cmp`
integrity checks passing.

## Run results

| run | integrity | receiver application rate | sender completion rate | bursts sent / decoded / physically matched | partial | crater | no callback | collisions |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `fixed_default_03` | PASS | 1.02 kbps / 399.8 s | 1.00 kbps / 409.3 s | 33 / 30 / 30 | 3 | 1 | 3 | 1 |
| `fixed_default_04` | PASS | 1.53 kbps / 268.3 s | 1.41 kbps / 289.7 s | 30 / 29 / 28 | 1 | 0 | 2 | 0 |
| `final_default_05` | PASS | **2.32 kbps / 176.6 s** | 2.20 kbps / 186.2 s | **17 / 17 / 17** | **0** | **0** | **0** | **0** |
| `final_default_06` | PASS | **1.62 kbps / 252.6 s** | 1.56 kbps / 262.0 s | **27 / 27 / 27** | 4 | **0** | **0** | **0** |
| `final_psk8_01` | intentionally aborted null diagnostic; not a throughput sample | n/a | n/a | 8 / 7 / 7 before stop | 0 | 0 | 1 | 0 |
| `final_psk8_02` | PASS | **1.51 kbps / 271.6 s** | 1.46 kbps / 281.0 s | **27 / 27 / 27** | 15 | 4 | **0** | **0** |
| `final_default_07` | PASS | **1.50 kbps / 272.4 s** | unavailable; final receiver ACK was rendered but not queue-committed before app disconnect | **27 / 27 / 27** | 6 | 1 | **0** | **0** |
| `final_default_08` | PASS | **2.26 kbps / 181.1 s** | 2.15 kbps / 190.5 s | **18 / 18 / 18** | 1 | **0** | **0** | **0** |
| `final_default_09` | PASS | **1.54 kbps / 265.6 s** | 1.49 kbps / 275.1 s | **28 / 28 / 28** | 8 | **0** | **0** | **0** |

`fixed_default_04`'s reported `4/6` partial was not six frames on the wire. The opening
five-frame burst lost its descriptor/head but salvaged the four surviving DATA frames from
the physical-tail marker. The following repair/new-data burst carried four physical frames;
its descriptor was also lost, and the receiver used stale fallback `N=6`, demodulating two
silence slots before issuing the callback. That is why its decoded and physically matched
counts differ. The validated builds recognize a CRC-valid physical-tail marker inside
accumulation and finalize the exact wire span; all six completed final diagnostics have
one callback per physical burst.

### Airtime and timer-basis accounting

| run | mean cycle | mean key-down | mean turnaround | delivered payload / DATA key-down within physical span | sender DATA key-down / physical span |
|---|---:|---:|---:|---:|---:|
| `fixed_default_03` | 12.54 s | 8.02 s | 4.52 s | 1.56 kbps | 64.52% |
| `fixed_default_04` | 9.52 s | 7.83 s | 1.69 s | 1.74 kbps | 81.58% |
| `final_default_05` | 10.82 s | 9.10 s | 1.72 s | 2.61 kbps | 85.11% |
| `final_default_06` | 9.76 s | 8.02 s | 1.74 s | 1.90 kbps | 82.65% |
| `final_psk8_02` | 10.40 s | 8.72 s | 1.68 s | 1.74 kbps | 84.34% |
| `final_default_07` | 10.57 s | 8.35 s | 2.22 s | 1.84 kbps | 79.42% |
| `final_default_08` | 10.44 s | 8.88 s | 1.56 s | 2.524 kbps | **85.92%** |
| `final_default_09` | 9.88 s | 8.15 s | 1.74 s | 1.808 kbps | 82.86% |

Ordinary clean cycles turn around in about 1.7 s; `_07`'s mean is higher because its
crater and two synchronized rate-change recoveries add non-keyed gaps. The former
"keyed-to-wall" calculation was invalid: it divided application-timer goodput by key-down
that included airtime before that timer began. The table now uses one physical interval
for numerator and denominator. For `_08`, the application outcome remains 2.26 kbps over
181.1 s, while first key-down -> completion is 188.884 s and 2.169 kbps. Its 85.92%
keyed-to-physical-span ratio leaves 0.355 kbps of non-keyed-only headroom; this does not
erase retransmitted key-down. Another 0.138 s of final sender key-down landed after the RX
completion callback and is explicitly excluded from delivery-span duty and headroom.
This is an accounting correction, not evidence that policy caused `_08`'s rate.

For `_09`, the corresponding physical span is 273.358 s, including 226.506 s of sender
DATA key-down (82.86%). Its non-keyed-only headroom is 0.310 kbps; eliminating every gap
would still leave selectively retransmitted key-down charged. Reverse ACK TX occupied
19.234 s across 28 committed group ACKs, all 28 were accepted, and only 0.034 s of sender
key-down extended beyond the receiver completion callback.

Even the measured payload/key-down rates of 2.61 and 1.90 kbps for `_05` and `_06` remain
below the project-recorded VARA result. The remaining large lever is reliable spectral
efficiency under fading, not another small ACK scheduling trim.

### Rung dwell

| run | rung dwell over the analyzed transfer |
|---|---|
| `fixed_default_03` | QPSK R1/2 105.27 s (25.8%); QPSK R2/3 291.37 s (71.5%); QPSK R3/4 11.06 s (2.7%); seven state announcements / six transitions |
| `fixed_default_04` | QPSK R1/2 164.29 s (57.0%); QPSK R2/3 123.74 s (43.0%); two state announcements / one transition |
| `final_default_05` | QPSK R1/2 11.18 s (6.1%); QPSK R2/3 96.77 s (52.5%); 8PSK R2/3 76.49 s (41.5%); three state announcements / two transitions |
| `final_default_06` | QPSK R1/2 11.18 s (4.3%); QPSK R2/3 249.19 s (95.7%); two state announcements / one transition |
| `final_psk8_02` | forced 8PSK R2/3 279.33 s (100%); one state announcement / no transition |
| `final_default_07` | QPSK R1/2 11.37 s (4.1%); QPSK R2/3 188.55 s (67.3%); 8PSK R2/3 80.23 s (28.6%); six state announcements / five transitions |
| `final_default_08` | QPSK R1/2 **11.03 s (5.8%)**; QPSK R2/3 95.07 s (50.3%); 8PSK R2/3 82.79 s (43.8%); **three state announcements / two transitions** |
| `final_default_09` | QPSK R1/2 11.36 s (4.2%); QPSK R2/3 262.00 s (95.8%); two state announcements / one transition |

The final entry policy therefore did not strand the transfer at R1/2: it used one robust
coherent measurement group and moved to R2/3 at the next decision. Whether it later reached
8PSK depended on the observed outcomes in that fading realization.

## Why automatic entry starts at QPSK R1/2

This is a deliberate **one-group measured-entry probe**, not a claim that R1/2 is the
right sustained rate at MPG@20.

The CONNECT measurement is acquired in the MC-DPSK domain. It is useful for acquisition
and mode entry, but it is not a trustworthy estimate of the first coherent-OFDM payload's
margin on a frequency-selective fading channel. The source records the measurement that
motivated the policy: across five real MPG@20 transfers, the former QPSK R3/4 first group
was incomplete every time (11/25 frames total), whereas QPSK R1/2 subsequently ran 24/24
groups clean.

The corrected sequence is:

1. Preserve the automatically selected rung as the latent controller's prior.
2. Cap only the first automatic coherent group at no higher than QPSK R1/2.
3. Feed that group's actual `k/M` result into the rate controller.
4. Let the first coherent group own the next decision.
5. Apply an operator-forced mode after the automatic cap so a valid force remains an
   exact override.

This fixes the opposite failures seen in earlier revisions: blind R3/4 entry lost the
first payload group, while pinning the weak CONNECT snapshot held `fixed_default_04` at
R1/2 for most of the transfer. In all five final default runs the first group was 5/5, and
the receiver commanded R2/3 immediately afterward. On the corrected physical timer,
first-key-down R1/2 dwell was about 11.0-11.4 seconds; the former ~21-second values began
at a pre-data mode announcement and were not transfer-only dwell.

## Defects repaired during the campaign

### 1. Feedback could start before the physical burst ended

`fixed_default_03` contains one exact half-duplex collision: a standalone ACK began 4.52 s
into physical burst 24, 2.03 s before its end. Logical `FINAL`, an inferred backstop, or a
lost descriptor could be mistaken for proof that the peer had stopped transmitting.

The current transport distinguishes logical completion from physical completion. Eligible
multi-frame, non-interleaved OFDM wire copies carry `PHYSICAL_BURST_END` only on the exact
last serialized DATA frame. A provisional air gate remains closed until descriptor geometry,
the CRC-valid physical-tail marker, or a valid standalone boundary proves the end. The ARQ's
logical frame is not mutated by wire-only stamping.

### 2. Missing descriptors produced phantom frames and premature callbacks

The descriptorless fallback initially salvaged useful frames but did not know the exact
tail while group accumulation was active. In `fixed_default_04`, it collected four real
frames, then treated two silence slots as members of a stale six-frame group and reported
`4/6`.

The decoder now recognizes a CRC-valid physical-tail marker on each successfully demodulated
member and immediately finalizes that exact collected span. A descriptor-declared group
remains authoritative if it arrives late because its `N` is still needed to represent an
unknown missing head. The final logs show `CRC physical tail decoded ... finalizing exact
wire span`, and all six completed final diagnostics have zero missing callbacks.

### 3. A tail repair inherited the wrong RTO and duplicate feedback planes

The original tail incident emitted both a legacy frame NACK and a tone SACK. The sender
heard the shorter tone response, retried while the receiver was still keyed on the legacy
waveform, and then gave a one-frame repair the configured full-group deadline.

The corrected path emits one physical feedback plane, computes deadlines from the exact
serialized physical turn (actual frame count, CW count, anchor, padding, and queued audio),
suspends holes not sent in that turn, and explicitly refills file/message tails after tone
SACKs. A singleton repair no longer inherits a whole-window RTO.

### 4. Mode changes and completion could invalidate live ARQ state

Mode-change retries are now based on one-control physical geometry, hold while local TX or
the channel is busy, and consume retry budget only after actual clear-channel egress. The
receiver emits one immediate MODE_CHANGE ACK rather than same-fade copies. In-flight
descriptor geometry and the connected decoder cursor survive the reconfiguration.

Rate-change requeues fell from three events / 15 chunks in `fixed_default_03` to zero in
`fixed_default_04`, `final_default_05`, and `final_default_06`. Terminal ARQ failure and
completion also snapshot and clear the old lifecycle before allowing reentrant work, so a
SACKed suffix cannot be published as a false success.

### 5. Selection and diagnostics used the wrong evidence

The selector now consumes actual `k/M` group outcomes and compares candidate useful bytes
per physical cycle using candidate-specific CW/window geometry. The automatic cold-start
prior is preserved across the R1/2 measurement probe. Tone-ACK detection can reject a
CRC-valid but protocol-impossible candidate and continue its timing search instead of
committing it as an ACK.

The transfer analyzer now counts only committed ACK vectors, excludes rendered-but-dropped
audio, separates semantic rejection from PHY failure, and labels audio-queue commit honestly.
This matters because earlier wording called queue acceptance "ACK on air," which it is not.

### 6. Environment force was local configuration, not a negotiated QSO contract

`final_psk8_01` proved that setting `ULTRA_FORCE_DATA_MOD=8PSK` and
`ULTRA_FORCE_DATA_RATE=R2_3` on both processes did not prove that the connected data PHY
used that profile. The environment values bypassed the automatic recommendation at some
sites, but they were not captured as outbound CONNECT intent, were not acknowledged with
force provenance, and did not freeze every automatic actuator for the QSO. The valid
diagnostic therefore remained at QPSK R1/2 even though both provenance files recorded the
force variables.

The repaired contract is end-to-end:

1. Parse the modulation/rate pair atomically. A malformed present field rejects the
   environment profile as one unit; a valid single-field force still resolves its
   complement automatically.
2. Capture the resolved force in CONNECT and preserve it on every CONNECT retry.
3. Make the responder apply that profile before serializing CONNECT_ACK and mark forced
   provenance in the ACK, including a responder-only local force.
4. Make an initiator reject a conflicting CONNECT_ACK instead of creating a split PHY.
5. Pin all automatic rate actuators for the QSO. An explicit force outranks the automatic
   `ULTRA_MAX_OFDM_RATE` ceiling; the ceiling still bounds every automatic climb.

`final_psk8_02` met the predeclared proof gate: the Pi CONNECT logged
`forced_mod=5, forced_rate=3`, the Mac logged `Using FORCED modulation 8PSK` and
`Using FORCED code rate R2/3`, and both endpoints logged 8PSK R2/3 before the first file
burst. The byte-identical 51,200-byte completion makes it a valid forced-rung diagnostic.

### 7. An automatic climb could price a full burst and execute on a short file tail

`final_default_07` contains one justified recovery and one avoidable tail move. Its first
8PSK climb had a full candidate group available; a later `0/5` crater correctly demoted
and requeued nine chunks. Near completion, however, the selector again priced an 8PSK
candidate with `N=8` while only three target-rung frames remained. It switched, decoded
`1/3`, demoted, and requeued two chunks. That last transition paid a synchronized mode
change and recovery for a counterfactual different from the full-cycle candidate the
selector had scored.

The sender now permits an **automatic faster** authority command during an active file
only when the exact remaining bytes can fill at least one complete physical burst under
the target profile. The calculation reuses the target codeword count, lifting `Z`, payload
capacity, tone-window cap, airtime ceiling, re-anchor cost, and burst-frame budget. A
short-tail hold emits neither descriptor data nor legacy MODE_CHANGE and does not arm the
authority dedup latch, so it remains re-evaluable. DOWN/recovery moves remain unconditional:
reliability is more important than avoiding a tail transition.

Receiver pricing includes the just-decoded clean group before transmitting the command;
sender round bookkeeping commits that same evidence later in the ACK callback. The guard
therefore receives an explicit accepted-clean-round credit so its post-ACK target `N`
matches the deferred physical refill. The special startup R1/2 -> R2/3 experiment retains
its separate trusted `M>=4` contract rather than being silently narrowed to normal `N`.

The boundary test retires FILE_START, stages production-like deferred ACK ordering, and
observes actual burst egress: `N-1=7` holds and refills at the current rung, while `N=8`
climbs and emits exactly one physical eight-frame 8PSK burst. Descriptor-disabled `N-1`
still cannot fall through to legacy MODE_CHANGE, and a one-frame DOWN tail still demotes.
The fixed9 matched-build `_08` transfer then completed byte-exact at 2.26 kbps on the
application timer (2.169 kbps on the physical delivery span), with one selective repair
and no crater, callback loss, collision, or rate-change requeue. It did not present another
near-EOF UP opportunity, so the deterministic boundary remains the direct proof of the
hold; `_08` is live integration/no-regression evidence.

### 8. Both scripted peers could close immediately after the final file ACK

After `_08` delivered the file, `--disconnect-on-file-done` was active at both endpoints.
The receiver armed teardown from its successful receive callback even though that callback
only proved the final ACK had been queued. The sender independently armed teardown when it
consumed that ACK. Both peers consequently transmitted full-anchor DISCONNECT requests at
nearly the same time, collided, and entered avoidable retry churn after an otherwise clean
transfer.

Completion-driven teardown now has one deterministic owner. Only a **successful outbound**
`FileSent` callback arms the request; receive completion and failed sends do not. The GUI
tick then permits only the QSO caller/initiator to issue it. The issued flag and timestamp
are published before `disconnect()` so synchronous state callbacks or duplicate completion
events cannot observe an unissued close. The protocol also handles a genuine operator-level
crossed close: decoding the peer's DISCONNECT while locally `DISCONNECTING` suppresses local
retries, keeps the peer ACK alive for the responder grace window, and converges without
double-counting the session.

The protocol regression makes both stations disconnect simultaneously and drops every
DISCONNECT ACK. Each station must put exactly one DISCONNECT request on the simulated wire,
send repeated peer ACKs during grace, count one disconnect, and converge to `DISCONNECTED`.
This protects the mutual-close fallback independently of the GUI's single-owner policy.

The fixed10 matched-build `_09` transfer exercised the normal one-owner path. Only the Pi
caller logged `Sending DISCONNECT`; the Mac emitted no DISCONNECT request. The Pi sent one
scheduled retry before an ACK returned. The Mac decoded one of those requests and ACKed it;
the endpoint clocks do not identify which copy it decoded. The Pi logged `Disconnect
acknowledged`, and both endpoints reached a clean disconnected state and quit. There was no
crossed close.

## Forced-8PSK diagnostics: null run and valid rerun

`final_psk8_01` was launched with the same fixed6 source and binary hashes as
`final_default_05` and `_06`, and with these variables recorded on **both** endpoints:

```text
ULTRA_FORCE_DATA_MOD=8PSK
ULTRA_FORCE_DATA_RATE=R2_3
ULTRA_LOCK_RATE=1
```

The modem nevertheless stayed at QPSK R1/2 for all seven completed groups. The run was
intentionally aborted after that null-control proved force precedence was broken. Its
`RESULT=MAC_EXIT`, `INTEGRITY=FAIL`, and incomplete artifact are expected consequences of
the stop; they are **not** an 8PSK failure and must never be included in a throughput mean.

`final_psk8_02` is the repaired matched-build rerun. It met that criterion and delivered
51,200 byte-identical bytes at 1.51 kbps receiver / 1.46 kbps sender. It also recorded
15 partial groups and four complete `0/N` craters across 27/27 physical bursts. Per-frame
decode success was broadly flat across positions one through five, so the failures do not
have the increasing-with-frame-position signature expected from stale within-burst CSI.

This does not prove automatic policy is faster: `_02` and `_07` are sequential unpaired
fades. It does prove that forcing the nominally faster rung is not a reliability solution
under this observed MPG@20 realization. The selector should price those outcomes honestly;
it should not be tuned to remain on 8PSK through correlated outages.

## Experimental 8PSK long-code follow-up

This follow-up held the negotiated data rung at 8PSK R2/3 on both endpoints and changed
only the physical LDPC geometry selected for connected OFDM file DATA:

- baseline: logical and physical cw12/Z27;
- test: logical cw12/Z27 represented on air as physical cw4/Z81;
- invariant: 629 useful payload bytes and 7,776 coded bits per frame in both arms;
- descriptor: every multi-frame Z81 group announced cw4 and Z81 in its `BURST_HEADER`;
  the singleton file tail was padded to two addressed physical frames so it could not
  escape without that descriptor.

The first two-pair attempt was negative on the sender timer: 1.43 versus 1.22 kbps and
1.61 versus 1.50 kbps (baseline versus Z81), or about -10.5% by arm means. That attempt is
retained as useful defect evidence, but **not as evidence against the code profile**. The
two arms did not receive equivalent implementations:

1. Z81 rebuilt a substantially larger LDPC decoder graph for every logical frame, while
   the established Z27 fixed-frame decoder was cached. Fixed-frame encoding also rebuilt
   the rate/Z matrix for every DATA frame; that common bug cost Z81 much more.
2. The fresh Z81 decoder used its constructor-default 50-iteration ceiling instead of the
   R2/3 production policy's 70 iterations.
3. Fixed-CW discovery and the CW0/HARQ header peek still assumed Z27 bit geometry. Both
   pre-fix Z81 runs ended with `HARQ keys real=0`, versus 95 in each baseline run.

The timing signature was decisive: median sender `Flushing burst` -> descriptor-ready time
was 0.530/0.518 s for the two Z81 runs versus 0.065/0.056 s for baseline, and receiver
sync load-shed counters reached x51/x41 versus x1/x1. The comparison therefore mixed a
coding-profile change with a deterministic real-time CPU/backlog penalty and a disabled
retransmission-combining identity path.

The repaired snapshot caches fixed-frame encoders and decoders by the complete `(rate, Z)`
identity, restores the configured iteration/factor state on every decoder checkout, carries
the descriptor's active Z through CW discovery/deinterleaving/header peek/final decode, and
includes Z in the HARQ key equality and hash. The post-fix median preparation times were
0.029/0.039 s for Z81 and 0.039/0.029 s for baseline; every run ended at only the startup
load-shed x1. The two Z81 runs built 86 and 89 real HARQ keys rather than zero. These are
mechanism-level validation of the implementation repairs; they do not attribute the whole
goodput difference to any one repair.

The post-fix launch order was BASE -> Z81, then Z81 -> BASE, using one frozen source and
matched platform builds:

| pair / launch order | arm | integrity | receiver application | sender completion | first-key-down -> completion | bursts sent / decoded / matched | partial | crater | no callback |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| 1 / first | cw12/Z27 baseline | PASS after explicit recheck | 1.56 kbps / 263.3 s | **1.50 kbps / 272.6 s** | 1.51 kbps / 270.97 s | 25 / 25 / 25 | 11 | 5 | 0 |
| 1 / second | cw4/Z81 | PASS | 2.36 kbps / 173.4 s | **2.24 kbps / 183.0 s** | 2.26 kbps / 181.14 s | 17 / 17 / 17 | 5 | 0 | 0 |
| 2 / first | cw4/Z81 | PASS | 1.90 kbps / 215.5 s | **1.68 kbps / 243.6 s** | 1.69 kbps / 241.91 s | 22 / 21 / 21 | 6 | 3 | 1 |
| 2 / second | cw12/Z27 baseline | PASS | 1.71 kbps / 239.1 s | **1.53 kbps / 267.3 s** | 1.54 kbps / 265.47 s | 24 / 23 / 23 | 10 | 3 | 1 |

The sender-completion paired deltas are +49.3% and +9.8%. Because all payloads are the same
size, the campaign summary pools the durations rather than averaging the rounded per-run
rates: sender completion is 1.517 -> 1.920 kbps (+26.6%), receiver application is
1.631 -> 2.106 kbps (+29.2%), and physical first-key-down -> completion is
1.527 -> 1.936 kbps (+26.8%). Selective-repair counts also fell in both pairs and crater
count was better/tied, but these remain descriptive outcomes from only two sequential
pairs. The first baseline's original `summary.env` said `INTEGRITY=FAIL` because the
temporary runner looked only for `testfile_50k.bin` while that transfer preserved the
source name `source-testfile_50k.bin`. `integrity_recheck.env` records the actual receive
path, byte count, matching SHA-256, and `cmp` result; this was a reporting false negative,
not a modem integrity failure.

**Decision: keep `ULTRA_8PSK_LONG_LDPC` default-OFF.** The fixed implementation has strong
positive directional and mechanism evidence, but two pairs are below this rig's predeclared
minimum of eight and cover only the operator-reported MPG@20 state. Graduation requires a
larger order-balanced matched-build sample, explicit completion/failure accounting, and a
Moderate-channel campaign before broad enablement. The result supports continuing the
experiment; it does not yet support changing the shipping default.

### Post-campaign audit: abnormal RX Z reset (repaired)

The four-run snapshot handled normal group completion and transfer teardown correctly, but a
subsequent lifecycle audit found a separate fail-closed gap. Consuming a Z81 `BURST_HEADER`
sets both `sync_controller_.have_burst_descriptor_` and the live
`OFDMChirpWaveform::setActiveLDPCLiftingZ(81)` geometry. Several abnormal group exits cleared
the descriptor latch/buffers without resetting that live waveform geometry: timeout/hard
failure paths in `StreamingDecoder::accumulateBurstFrames()`, exception exits from
`finalizeBurstGroup()`, and the HEADNULL backstop in `decodeCurrentFrame()`. A following
descriptor-less path could therefore inherit stale Z81 sample/codeword sizing.

The repair centralizes decode-thread cleanup in `clearActiveBurstDescriptorGeometry()` so
those exits return the waveform and descriptor state to Z27 as one operation. The connected
mode-hop rebuild path in `applyPendingConnectedOFDMMode()` must do the complementary thing:
when a valid descriptor lock is intentionally preserved, it re-applies that descriptor's
wire Z81 after rebuilding the waveform instead of silently reverting mid-group.

That lifecycle repair is now complete. Decode-thread exits use the centralized operation;
cross-thread `feedAudio()` overflow, `reset()`, and `setMode()` defer live waveform/
interleaver mutation to the safe top-of-`processBuffer()` boundary. Regressions cover normal
completion, timeout, hard failure, exception, HEADNULL, mode rebuild, lifecycle reset, and
the requirement that Z27 be restored before synchronous callbacks observe state. This
repair predates the two current-profile live runs below, but the still-newer acquisition and
anchor-contract repairs do not. The profiles therefore remain default-OFF.

## Current Z81 profile transfers and acquisition follow-up

The immutable current-profile snapshot used one 51,200-byte master payload for two forced
MPG@20 transfers. Both endpoints passed the runner's environment/profile gate and both
received files matched the source by byte count, SHA-256, CRC, and `cmp`:

| run | physical profile | application goodput | physical-span goodput | physical outcomes |
|---|---|---:|---:|---|
| `qpsk_r34_z81_01` | QPSK R3/4 cw3/Z81 | 1.85 kbps / 221.6 s | 1.64 kbps / 250.04 s | 23 sent, 22 decoded/matched; 3 partial groups, 1 crater, 1 first-burst no-callback; 13 missing chunks repaired |
| `8psk_z81_01` | 8PSK R2/3 cw4/Z81 | 1.83 kbps / 223.7 s | 1.77 kbps / 231.45 s | 22/22/22 sent/decoded/matched; 9 partial groups, 2 craters |

Every run accepted all 22 committed group ACKs, had zero rate-change requeue, and completed
byte-exact. The QPSK receiver missed the very first descriptor even though its chirp
correlation was about 0.90. The trace establishes the software cause: detector training was
absolute sample 673085 while the ring had only received through 671744. Clamping that
legitimate future position to the write head decoded 1341 samples early (221 modulo the
1120-sample OFDM symbol), matching the observed roughly 225-sample LTS timing error. The
later QPSK `0/5` is separate: its actual DATA LTS was near 0 dB in-band with EVM around
6.8-7.8, so it remains a genuine fading outage.

The current code now defers an exact immutable detector window until a future training
position is live; it never clamps backward. A separate full-first/resend problem was also
reproduced: the same search window can contain an earlier complete descriptor chirp and a
stronger, later DATA up-chirp whose pair/training is incomplete. The detector now recovers
an earlier complete pair only when the strongest candidate is unusable. This is deliberately
recorded as a second mechanism rather than retroactively attributing every live loss to the
first clamp defect.

The descriptor now also carries backward-compatible
`BURST_FLAG_CURRENT_GROUP_FULL_ANCHOR` (`0x08`). It tells the receiver that the DATA group
following this descriptor starts with a full chirp+LTS, so first/resend/mode-switch turns do
not waste a light-marker probe. The sender's full-anchor reason is bound to the exact
physical request through `DeferredTx`; a CCA-delayed or purged request cannot donate its
one-shot to another burst. Finally, distinct completed-group `WAITING-REBASE` voices now
use a QSO-local mod-64 event identity rather than the descriptor's constant group zero.

### Repaired Good@20 burst screens

`measure_ack_fer --config burst_chunk` now pumps one 4800-sample idle interval through the
same channel before each descriptor, compares against the encoder's actual physical-tail
stamping, and passes the FULL group-start decision per request. With MEDIUM CP, group size
five, and seeds `{7,11,23,42}`, it produced:

| profile/regime | BI0 | BI1 | invariant |
|---|---:|---:|---|
| QPSK R3/4 cw3/Z81, descriptor FULL + DATA LIGHT, `n=80/seed` | 1567/1600 frames; 298/320 groups | 1595/1600; 319/320 | 403680 samples / 8.410 s; 527 B/frame |
| QPSK R3/4 cw3/Z81, descriptor FULL + DATA FULL, `n=20/seed` | 390/400; 73/80 | 390/400; 78/80 | 160/160 descriptors, 320 syncs, 74 earlier recoveries |
| 8PSK R2/3 cw4/Z81, descriptor FULL + DATA FULL, `n=20/seed` | 353/400; 60/80 | 360/400; 72/80 | 160/160 descriptors, 320 syncs, 88 earlier recoveries |

The broader steady 8PSK geometry screen confirms that shortening the equal-capacity long
profile is the wrong direction:

| geometry / BI | decoded frames | complete groups | modeled goodput with 1.790 s turnaround |
|---|---:|---:|---:|
| cw12/Z27 / BI0 | 2113/2400 (88.04%) | 323/480 (67.29%) | 2342 bps |
| cw4/Z81 / BI0 | 2174/2400 (90.58%) | 348/480 (72.50%) | 2410 bps |
| cw3/Z81 / BI0 | 673/800 (84.13%) | 102/160 (63.75%) | 1947 bps |
| cw2/Z81 / BI0 | 615/800 (76.88%) | 86/160 (53.75%) | 1427 bps |
| cw4/Z81 / BI1 | 753/800 (94.13%) | 147/160 (91.88%) | 2504 bps |

cw12/Z27 and cw4/Z81 both carry 624 useful file bytes per frame and emit 364480 samples
(7.593 s) per steady five-frame group. cw4/Z81 beat cw12/Z27 on every seed. The QPSK
steady BI1 screen reduced incomplete groups from 22 to one; its frame-weighted /
complete-group-only capacity proxies were 2454.8 / 2334.2 bps BI0 and 2498.7 /
2498.7 bps BI1.

These results justify a fresh real BI0/BI1 experiment for each exact profile, not a shipping
change. Historical real MPG@20 forced-16QAM evidence favored BI0 by 37%, delivered 5/5
versus 4/5, and had 12 rather than 62 craters. Interleave remains default-OFF, as do both
long-code profile knobs. No transfer in this document yet validates the future-training,
earlier-candidate, current-anchor-bit, or request-binding fixes over the real IONOS path.

## Project-recorded benchmark and remaining gap

Project documentation records a VARA result of 23,142 bytes/min at Multipath-Good
SNR 20:

```text
23142 bytes/min * 8 / 60 = 3085.6 bps
```

Sources:

- [TRANSFER_SPEED_VERIFICATION_2026_05_22.md](TRANSFER_SPEED_VERIFICATION_2026_05_22.md)
- [COMPETITIVE_BENCHMARK_TARGET.md](COMPETITIVE_BENCHMARK_TARGET.md)

The number includes the recorded VARA transfer protocol behavior, but the source does not
pin setup-timer boundaries tightly enough to call it a locally reproduced end-to-end
measurement. It is therefore a **project-recorded competitive target**, not ground truth
for this IONOS campaign.

The fading-anchor study has been corrected to separate raw PHY capacity from the historical
fixed scheduling model:

- [FADING_ANCHOR_MEASUREMENT_2026_07_26.md](FADING_ANCHOR_MEASUREMENT_2026_07_26.md)

The old `~2,450 bps` claim was not a hard ceiling. It used a stale `0.593` multiplier and
incorrectly treated all 59 occupied carriers as payload; spacing-8 OFDM uses 51 data
carriers and eight pilots. With current geometry, 8PSK R2/3 is about 4,250 raw bps. The
raw-to-delivered efficiency required for 3,000 bps is:

```text
3000 / 4250 = 70.6%
```

That is feasible in principle. The forced `_02` run nevertheless delivered only 1.51 kbps
because fading losses consumed repeated physical bursts. The gap is reliability plus all
end-to-end overhead, not an immutable 8PSK capacity limit.

At the observed rates, the transfer rows use the receiver application timer; the final row
is raw PHY capacity and is intentionally not a delivered-goodput observation:

| reference | rate (bps) | fraction of 3,085.6 benchmark | remaining gap |
|---|---:|---:|---:|
| `final_default_05` | ~2,320 | 75.2% | ~766 bps |
| `final_default_06` | ~1,620 | 52.5% | ~1,466 bps |
| `final_psk8_02` | ~1,510 | 48.9% | ~1,576 bps |
| `final_default_07` | ~1,500 | 48.6% | ~1,586 bps |
| `final_default_08` | ~2,260 | 73.2% | ~826 bps |
| `final_default_09` | ~1,540 | 49.9% | ~1,546 bps |
| Z81 post-fix arm, pooled receiver-application duration (n=2) | ~2,106 | 68.3% | ~980 bps |
| current 8PSK R2/3 raw PHY capacity | ~4,250 | not delivered goodput | n/a |

QPSK R3/4 is about 3,188 raw bps, which would require roughly 96.8% total efficiency to
match 3,085.6 delivered bps and leaves essentially no room for half-duplex feedback or
recovery. Closing the gap therefore requires a reliably usable higher-spectral-efficiency
rung and receiver improvement, not forcing a fragile rung or tuning the selector to ignore
its observed error rate.

## Statistical and operational caveats

1. **No whole-transfer causal A/B claim.** The original diagnostics are sequential fading
   realizations and six source snapshots, not randomized interleaved pairs. The long-code
   follow-up is order-balanced, matched-build, and paired, but has only two pairs. The prior
   rig record observed paired standard deviation of 14-36%; neither part of this report is
   powered for an expected mean or shipping-policy effect.
2. **Same build, materially different result.** `final_default_05` and `_06` share exact
   source and binary hashes but delivered 2.32 and 1.62 kbps. This is direct evidence that
   rate dwell and realization variance dominate a one-run comparison.
3. **Mechanism evidence is narrower but stronger.** It is valid to say the validated
   fixed6/fixed8 builds removed the observed collision, phantom tail slots, and missing
   callbacks, while fixed10 removed the observed completion-driven crossed close. `_05`,
   `_06`, `_08`, and `_09` had no rate-change requeues; `_07` had one necessary
   synchronized recovery and one avoidable short-tail move. It is not valid
   to assign the entire bps increase to any one repair.
4. **MPG@20 is operator provenance.** The external IONOS GUI state is not embedded in the
   endpoint logs. Future automation should capture a simulator configuration screenshot or
   machine-readable profile before each batch.
5. **High PA duty is not hardware-safe by implication.** The six original final completed
   transfers spent 79.42-85.92% of their physical delivery spans in sender DATA key-down;
   the four long-code follow-up runs were likewise 80.9-84.6%. A real 100 W final may
   derate near the continuous-digital duty region. A hardware deployment needs a duty
   governor and the specific transmitter manufacturer's power/duty limits; IONOS does not
   exercise thermal protection.
6. **`/tmp` is ephemeral.** Preserve the campaign directory before reboot or cleanup if
   these raw artifacts are needed for later review.

## Exact provenance

All nine named runs use Git HEAD `eac3c9f37e88d3ceb216fb9ee66b8ca52ee1e734`; each
run's provenance also records a SHA-256 of the dirty binary diff used to build it.

| run | started UTC | finished UTC | diff SHA-256 | source archive SHA-256 | Mac binary SHA-256 | Pi binary SHA-256 |
|---|---|---|---|---|---|---|
| `fixed_default_03` | 2026-08-02 01:31:19 | 01:39:05 | `387e650c3dbc7ec4b45c162a2a86c84bd21027afc2ce7e67158566537b170871` | `ff60e0513f80d5d6c48a2d3a0f2e911e98ad113892d1668746710ab577fa4e29` | `c37cd45c582d00def2b5b57d971abebb3e8776f94b36d41e22bc7656f36a31b8` | `6dae1baeecf79233223a5f59169f7d96542efd7054cc04a1e3ec65b82a3319c9` |
| `fixed_default_04` | 2026-08-02 02:16:50 | 02:22:37 | `0367b1c3cb72046f45f0e882048136df85c590e523c3551b24e760f5b192c8e0` | `1efcd4b4f2cc7457f4210aa2f49872dde4b8f8ccf487a7ba63880908c08e2516` | `a0e3e31aed130ff5919b265510cabd3bf0de612df058b16388bf8be003604090` | `e187947dfaf4a6406c02338da1cdbf164340be96d1f21e141401a677472bb453` |
| `final_default_05` | 2026-08-02 03:27:11 | 03:31:12 | `fcc1ddf31c65411cb9ad6a85cb96c5e614bf26be6ed59d19d3d8bb8b97220c58` | `9d86b20d971b3d2e83c01ba0d8b1427a4923f1594ce4e1ab9c8c96cf13f61093` | `19ea48f4fc8edfa4089027edeb0b9913356006197a9a06a4e6b3e723a30ebc22` | `7395eccb85dd43d0b245113d963404465dcc400b55341684e65a2e092694a270` |
| `final_default_06` | 2026-08-02 03:31:30 | 03:36:46 | same as `_05` | same as `_05` | same as `_05` | same as `_05` |
| `final_psk8_01` | 2026-08-02 03:37:47 | 03:39:55 | same as `_05` | same as `_05` | same as `_05` | same as `_05` |
| `final_psk8_02` | 2026-08-02 04:19:15 | 04:24:46 | `f9ba0ddb1235ff7383aca3e84afa86a63c70ed188e380bef0f435dc0089e5efd` | `b15f5ad3ed9458fdcb6804e35d7a833cdf8eed2565b6d3a5169d9d083c5e2d6b` | `a5c7eea007be449bfcdab88b53ee8933e81dc154e9b1bbdbb4f8d748b76f2941` | `5f703c6519afaa7ca6ba07eeb597ed9b9933c240150b9dcb0576487cccf60027` |
| `final_default_07` | 2026-08-02 04:25:22 | 04:30:54 | same as `_02` | same as `_02` | same as `_02` | same as `_02` |
| `final_default_08` | 2026-08-02 05:06:55 | 05:10:57 | `f7d0fd28e06b46789f9805cca3acf7e672f31852865fe562b906f81094987cd9` | `5499222dee4436b113e23cb225c52d0229764615fac5f193eae4f5bb073e41a2` | `5d560e5ba19716e1c5408e75d24cb68dba14bc9388415bc7e1c93035bdded658` | `682cf96e2e6f3120f3186f9c38349bd7f505cbeee0d3e45a535291724bfad3ba` |
| `final_default_09` | 2026-08-02 12:45:48 | 12:51:19 | `abfcdec648696b7c300fba5fe222f408cda9cd961ecf7bd14ad4cf040aad1f40` | `f58052bfa2f3383eef16f4274901251c46b416594572843b3e957c1685a42add` | `c0eca62f8ed77078f764d62eebdfa2c8a1a4f1229aa0ab7000a124bbef3e6c2c` | `a5beeebbfa02fa1c9a0fbc5506c352e183ff3c9fbd215841ec547864c987784f` |

The exact fixed8 source freeze used for `_02` and `_07` is retained at
`/tmp/projectultra-throughput-fixed8.5WsqMl`; its archive and endpoint hashes are the
literal values in the table, not a later rebuild from the moving worktree.

The exact fixed9 source freeze used for `_08` is retained locally at
`/tmp/projectultra-throughput-fixed9.Kt7bql/source-v2.tar.gz` and was built on the Pi at
`/tmp/projectultra-throughput-fixed9-Kt7bql`. Its source archive hash matched before the
Pi build. The endpoint executables are platform-specific, so their differing hashes are
expected and both are pinned in the table.

The exact fixed10 source freeze used for `_09` is retained locally at
`/tmp/projectultra-throughput-fixed10.5LDRWF/source.tar.gz` and was extracted and built on
the Pi at `/tmp/projectultra-throughput-fixed10-5LDRWF`. The Mac executable was
`/Users/mathieuvachon/Projects/ProjectUltra/build/ultra_gui`. The archive and both endpoint
binary hashes are pinned in the table; the source archive hash was verified before the Pi
build.

The later long-code follow-up used a separate immutable freeze on both endpoints:

```text
Git HEAD:              eac3c9f37e88d3ceb216fb9ee66b8ca52ee1e734
source archive:        /tmp/projectultra-z81-postfix-freeze.20260802T155554Z/source.tar.gz
source archive SHA256: 4b53a50b0eb264c461fa0b8ad07ab7d2bf0ac503397b3d537306eaa7e3658941
Mac binary SHA256:     9892179fdb83e0bb92d3292684d639b2d3e4d51cf115da56adffd278b509b8c8
Pi binary SHA256:      83baab582b0430d8d4b6a13c24492d456f277c521ba176e4b7bcbea2ee811d38
payload SHA256:        01a5cb8e7cf049bc74cc3518d7fc63038c5ef0c643e310f6547474ea8e427539
Pi source/build root:  /home/math/projectultra-z81-postfix-freeze.20260802T155554Z
```

The snapshot's captured `worktree.patch` hashes to
`3718cd1a7fae1e62dbbdbb091655f3d0d875561b0cff3ae8756e228c428d9d39`.
Each run also records `57328ba029e34cbf896c793ee21936987d16565ea858e7259bfb46d6e65bce77`
for a freshly rendered `git diff --binary` inside the extracted source. Those are different
text renderings of the same frozen dirty tree (full versus abbreviated object-ID headers),
not different build inputs; the archive and endpoint binary hashes above are the executable
provenance.

The later `qpsk_r34_z81_01` and `8psk_z81_01` transfers used a second immutable
current-profile freeze on both endpoints:

```text
Git HEAD:                eac3c9f37e88d3ceb216fb9ee66b8ca52ee1e734
source archive:          /private/tmp/projectultra-z81-current-freeze.20260802T170932Z/source.tar
source archive SHA256:   5946b6db2e69c94d0edb9645cae7b396c03e0ab0e04b06cd70c9fdba59cd98f0
working-tree patch SHA:  2afea4563ae1c6441f2dfa1dd36341cc1f91c07d3922ad84938b351804ab8bb2
Mac binary SHA256:       2caec37b9a306f1bf7c9ce178acca0fcb51311b3b76002e625e89ee48a3f8e50
Pi binary SHA256:        48286b8ecc3bb1b23f69fb8609cc92beae31985afda6f1c5267cae6c3d98593b
master payload SHA256:   6c2dd1807c8b9d7202384a353af4ccec6a50c2dcbc859f9a83bb4269fe439fd8
operator channel:        IONOS MPG@20 (operator reported)
```

That freeze contains the Z81 lifecycle repair and both long-code profiles, but predates the
future-training defer, earlier-complete-candidate recovery, current-group FULL flag,
request-bound anchor option, and WAITING-REBASE event-identity fixes. It must not be cited as
live validation of those later changes.

`final_default_08` received 51,200 bytes with CRC, `cmp`, MD5, and SHA-256 all exact. Its
18 committed group ACKs were all accepted, with zero semantic rejection, collision, or
rendered-but-uncommitted ACK. The only loss was one `6/8` partial group, repaired
selectively in the next physical turn. There was no `0/N` crater and no rate-change
requeue. The run reached 8PSK with a complete target group available and stayed there;
it did not exercise the new short-tail hold branch.

`final_default_09` likewise received 51,200 bytes with CRC, `cmp`, MD5, and SHA-256 all
exact. Its 28 committed group ACKs were all accepted, with zero semantic rejection,
Hamming correction, collision, or rendered-but-uncommitted ACK. Eight `k/M` partials were
selectively repaired; there was no `0/N` crater, callback loss, or rate-change requeue.
Only the Pi initiated teardown, it sent one scheduled retry before an ACK returned, and
both peers disconnected and quit cleanly without a crossed close.

## Artifacts and reproduction

Campaign root:

```text
/tmp/projectultra-throughput-campaign.jr8b2G
```

Each run directory contains `provenance.env`, `summary.env`, `mac.log`, `pi5.log`,
`analysis.txt`, `analysis.json`, `events.txt`, and source/received payload copies when the
transfer completed. `fixed_default_03`'s authoritative report is
`analysis.corrected.txt` / `analysis.corrected.json`.

Completed-run directories:

```text
/tmp/projectultra-throughput-campaign.jr8b2G/fixed_default_03
/tmp/projectultra-throughput-campaign.jr8b2G/fixed_default_04
/tmp/projectultra-throughput-campaign.jr8b2G/final_default_05
/tmp/projectultra-throughput-campaign.jr8b2G/final_default_06
/tmp/projectultra-throughput-campaign.jr8b2G/final_psk8_02
/tmp/projectultra-throughput-campaign.jr8b2G/final_default_07
/tmp/projectultra-throughput-campaign.jr8b2G/final_default_08
/tmp/projectultra-throughput-campaign.jr8b2G/final_default_09
```

Aborted null-control directory:

```text
/tmp/projectultra-throughput-campaign.jr8b2G/final_psk8_01
```

Post-fix long-code follow-up root and completed-run directories:

```text
/tmp/projectultra-z81-postfix-campaign.20260802T155554Z
/tmp/projectultra-z81-postfix-campaign.20260802T155554Z/z81_postfix_pair01_base
/tmp/projectultra-z81-postfix-campaign.20260802T155554Z/z81_postfix_pair01_long
/tmp/projectultra-z81-postfix-campaign.20260802T155554Z/z81_postfix_pair02_long
/tmp/projectultra-z81-postfix-campaign.20260802T155554Z/z81_postfix_pair02_base
```

Current-profile freeze, campaign root, and completed-run directories:

```text
/private/tmp/projectultra-z81-current-freeze.20260802T170932Z
/private/tmp/projectultra-z81-current-campaign.20260802T170932Z
/private/tmp/projectultra-z81-current-campaign.20260802T170932Z/qpsk_r34_z81_01
/private/tmp/projectultra-z81-current-campaign.20260802T170932Z/8psk_z81_01
```

Each current-profile run contains `provenance.env`, `summary.env`, both endpoint logs,
`analysis.txt`, `analysis.json`, `events.txt`, the exact source payload, and the received
payload. Use those immutable artifacts for the pre-fix postmortem; build a new freeze before
claiming over-the-air behavior for the subsequent sync/anchor repairs.

Do **not** use `/tmp/projectultra-throughput-campaign.jr8b2G/run_one.sh` for a new
comparison. That historical runner was later retargeted: its Pi executable is pinned to a
pre-fix Z81 freeze while its Mac `ROOT` follows the moving workspace. Reusing it now can
silently launch different implementations on the two endpoints and record stale provenance.

With IONOS already running at the intended profile and both audio routes verified, continue
the long-code sample with the matched post-fix runner. Pass the complete force/profile tuple
to **both** endpoints and alternate the within-pair order. For example, pair 3 is BASE ->
Z81:

```bash
/bin/bash /tmp/projectultra-z81-postfix-campaign.20260802T155554Z/run_one.sh \
  z81_postfix_pair03_base --log-level info \
  --mac-env ULTRA_FORCE_DATA_MOD=8PSK \
  --mac-env ULTRA_FORCE_DATA_RATE=R2_3 \
  --mac-env ULTRA_LOCK_RATE=1 \
  --mac-env ULTRA_8PSK_LONG_LDPC=0 \
  --pi-env ULTRA_FORCE_DATA_MOD=8PSK \
  --pi-env ULTRA_FORCE_DATA_RATE=R2_3 \
  --pi-env ULTRA_LOCK_RATE=1 \
  --pi-env ULTRA_8PSK_LONG_LDPC=0

/bin/bash /tmp/projectultra-z81-postfix-campaign.20260802T155554Z/run_one.sh \
  z81_postfix_pair03_long --log-level info \
  --mac-env ULTRA_FORCE_DATA_MOD=8PSK \
  --mac-env ULTRA_FORCE_DATA_RATE=R2_3 \
  --mac-env ULTRA_LOCK_RATE=1 \
  --mac-env ULTRA_8PSK_LONG_LDPC=1 \
  --pi-env ULTRA_FORCE_DATA_MOD=8PSK \
  --pi-env ULTRA_FORCE_DATA_RATE=R2_3 \
  --pi-env ULTRA_LOCK_RATE=1 \
  --pi-env ULTRA_8PSK_LONG_LDPC=1
```

Run pair 4 in the opposite order. The runner refuses to overwrite an existing run
directory, archives a pre-existing download, records hashes before launch, verifies the
returned payload with `cmp`, and then runs the analyzer. Its current filename check expects
`testfile_50k.bin`; if a future sender preserves a different source basename, recheck the
actual received path with `cmp` and SHA-256 instead of treating the runner's filename miss as
a modem failure. To re-analyze an existing run with the current analyzer:

```bash
python3 /Users/mathieuvachon/Projects/ProjectUltra/tools/analyze_transfer.py \
  /tmp/projectultra-throughput-campaign.jr8b2G/final_default_05/pi5.log \
  /tmp/projectultra-throughput-campaign.jr8b2G/final_default_05/mac.log \
  --json /tmp/projectultra-throughput-campaign.jr8b2G/final_default_05/analysis.recheck.json \
  > /tmp/projectultra-throughput-campaign.jr8b2G/final_default_05/analysis.recheck.txt
```

The original `_02` logs prove its fixed8 force engaged. They still do not form a selector
comparison against the sequential automatic `_07` realization. Any continued long-code
comparison must remain order-balanced and powered from a predeclared variance assumption,
must predeclare completion and delivered goodput as primary outcomes, score connected
transfer failures as zero rather than dropping them, and retain mechanism metrics (profile
engagement, preparation latency, load shed, HARQ key provenance, partials, craters,
callbacks, collisions, requeues, and PA duty) for attribution.
