# IONOS MPG@20 throughput campaign — 2026-08-04 handoff

This is the compact handoff for the current Pi5 -> Winlink IONOS -> Mac campaign.
The goal is a reliable 50 KiB transfer near 3,000 bit/s at operator-set MPG@20.
Reliability is a hard constraint: a faster rung is not an improvement if it loses
the file, wedges ARQ, or fails teardown.

## Outcome

The current 8PSK R2/3 cw4/Z81 BI0 path is byte-exact and teardown-clean, but it has
not reached 3,000 bit/s over a complete fading transfer. Across the eight valid
order-balanced A/B pairs below, mean physical goodput was 1.812693 kbit/s OFF and
1.814288 kbit/s ON. The best earlier independent draw remains 2.494 kbit/s physical;
none of these numbers changes the 2.95--2.96 kbit/s zero-loss full-file ceiling.

The exact-identity descriptor-only partial-repair mechanism works and mechanically
removed 73.2 seconds across the eight enabled arms. It did **not show a statistically
detectable** goodput improvement: paired physical effect +1.47%, 95% CI -14.01% to +16.94%,
`p=0.829`; the log-ratio estimate was -0.12%. It also removes acquisition diversity,
and two enabled runs had a no-callback light repair after a severe partial. The
feature therefore remains default-OFF and requires an acquisition-safe redesign,
not another claim based on the 14.4-second single-run counterfactual.

## Interleaved PSDR A/B — measured wash, keep default-OFF

The batch ran one immutable source/binary/payload and alternated order. Pair 1 is
void as a whole: its OFF arm delivered the exact file but the Pi RX audio consumer had
a 3.145-second service gap coincident with a two-second FIFO overrun before DATA.
Retaining the clean ON arm would destroy pairing. Pairs 2--9 form the eight-pair
analysis set; pair 9 was the complete OFF/ON order-balancing replacement for void pair
1, leaving four ON-first and four OFF-first pairs. Runtime/apparatus, integrity,
arm-provenance, accounting, or teardown failures invalidate paired throughput
measurement. Protocol safety failures remain reported outcomes and block promotion;
craters, selective NACKs, and internal ARQ timeout retransmissions are channel outcomes.

| Pair | Order | Physical OFF | Physical ON | Delta | ON engagements | Craters O/N | NACK O/N | ARQ timeout O/N |
|---:|:---:|---:|---:|---:|---:|:---:|:---:|:---:|
| 2 | ON/OFF | 1.819226 | 2.137783 | +17.511% | 3 | 1/2 | 28/27 | 2/0 |
| 3 | OFF/ON | 1.664136 | 1.497102 | -10.037% | 7 | 1/3 | 47/55 | 0/7 |
| 4 | ON/OFF | 1.890348 | 1.970469 | +4.238% | 8 | 1/1 | 32/39 | 0/0 |
| 5 | OFF/ON | 2.201454 | 1.559907 | -29.142% | 10 | 0/2 | 9/54 | 7/5 |
| 6 | ON/OFF | 1.743745 | 1.925053 | +10.398% | 10 | 0/1 | 44/40 | 0/0 |
| 7 | OFF/ON | 1.562468 | 1.737326 | +11.191% | 5 | 0/2 | 31/50 | 14/0 |
| 8 | ON/OFF | 1.979087 | 1.639366 | -17.166% | 11 | 0/1 | 18/60 | 7/0 |
| 9 | OFF/ON | 1.641078 | 2.047301 | +24.753% | 7 | 1/0 | 52/21 | 0/7 |

Descriptive physical-span inference on the eight percentage deltas:

- mean +1.468%, median +7.318%, SD 18.509 percentage points;
- 95% paired-t CI -14.006% to +16.942%, `t(7)=0.224`, two-sided `p=0.8289`;
- five of eight pairs positive, exact two-sided sign `p=0.7266`;
- log-ratio sensitivity: geometric effect -0.119%, CI -15.062% to +17.453%,
  `p=0.9866`;
- order diagnostic: ON-first mean +3.745%, ON-second mean -0.809%.

Keyed-throughput inference was also a wash: mean paired delta +0.548%, CI -15.433%
to +16.530%, `p=0.9376`; geometric effect -1.112%. Across valid arms, OFF/ON
outcomes were 4/12 craters, 261/346 NACK retransmission entries, 30/19 ARQ-timeout
entries, and one/one no-callback burst. All sixteen files were byte-exact and all
sixteen teardowns passed.

The enabled arms recorded 143 provenance events, 96 decisions, and 61 engagements.
At exactly 1.2 seconds per engagement, they avoided 73.2 seconds: mean 3.975% of the
enabled physical span and 4.672% of enabled DATA key-down. Do not add that mechanical
counterfactual to the observed A/B effect; changing airtime moves every later burst
to a different fade.

The acquisition warning is concrete. Pair 9 ON's missing callback was burst 5, a
descriptor-only/light-DATA repair engaged after only 1/8 members decoded; it forced
seven timeout retransmissions. The otherwise void pair 1 ON had the same shape after
a 3/6 partial. A valid OFF arm also lost one full-anchored callback, so this is not a
causal rate estimate, but it proves that a robust descriptor plus light DATA is not
an unconditional acquisition substitute in a deep fade. Any successor must preserve
acquisition diversity or fail closed on severe partials before it is re-measured.

The observed log-ratio SD is 0.194. At this variance, eight pairs can resolve only
roughly a 21--24% multiplicative effect with conventional power, not the expected
4% mechanical gain. This campaign is a valid safety/effect-size study and a clear
non-promotion result; it is not statistically capable of proving such a small gain.
The reproducible standard-library analysis is `/tmp/projectultra_psdr_stats.py`.

## Immutable run provenance

Run label:
`mpg20_8psk_cw4_z81_bi0_psdr_identityfix_01`

- Campaign: `/tmp/projectultra-ionos-campaign.20260804T115940Z`
- Freeze: `/tmp/projectultra-ionos-freeze.20260804T115940Z`
- Pi freeze: `/home/math/projectultra-ionos-freeze.20260804T115940Z`
- Git HEAD: `03c6cbde2f2a6ba742a60c65fc7eba0b013025ff`
- Worktree patch SHA-256: `5cc1530d2fecd0ba2ecd6c5bb2a2583c226ea47505d08e6fe6bec57405cd4e04`
- Source archive SHA-256: `60c8502852867d38bb487137d94f048f8b9b9d0bdaa99eda5e8ace2fb4aad579`
- Mac binary SHA-256: `4810781ed7c8e4e4007449017a0d2d5664a740a9ec05f9f488c1fec7c37ee9df`
- Pi binary SHA-256: `a7f54c6de00c191ebce0c3c46f27dcd8f0e8d48bbd13d95741516cf675895db4`
- Payload/received SHA-256:
  `958ab5edfc50e42e64173412ef335e3f3e84debd8698e7e345df94e4e692496f`

Endpoint arm: 8PSK R2/3, physical cw4/Z81, BI0, 11,500 ms burst ceiling,
8192-sample Mac/Pi audio periods, production soft combining on, partial descriptor
repair on, software ALC off, delayed ACK repeat off. Endpoint ULTRA environments
matched exactly. The channel setting is operator-attested external state:
IONOS MPG@20.

## Corrected single-run proof gates

- Result/integrity/teardown: PASS / PASS / PASS
- Physical groups: 17 sent, 17 decoded, 17 matched
- Missing or unmatched callbacks: 0 / 0
- Craters / timeouts / half-duplex collisions: 0 / 0 / 0
- Partial groups / NACK retransmission entries: 12 / 43
- ACKs: 17 committed, 17 accepted, zero semantic rejection
- Physical transfer span: 212.850333 s
- Sender DATA key-down in span: 185.663333 s
- Physical / keyed payload rate: 1.924357 / 2.206144 kbit/s
- Median turnaround: 1.69 s; 98.2% was ACK commit to sender acceptance

The final DATA ACK was low-margin but valid: normalized peak 0.066, confidence
1.401, one Hamming-corrected block. The existing 25 ms robust staircase delivered
it without a timeout. This is a collection warning, not evidence for enabling the
unvalidated delayed-repeat experiment.

## Exact partial-repair proof

`arq_progress` is cumulative transport progress and is not physical k/N. The new
gate snapshots the exact serialized current-round identities before and after the
accepted ACK. Every decision below began with all N exact identities live, retired
exactly k, retransmitted exactly N-k identities, and produced a synchronous callback.

| Source burst | Cumulative vs physical | Exact unacked | Repair burst/result |
|---:|---:|---:|---:|
| 1 | 2 vs 2/8 | 8 -> 6 | 2: 8/8 |
| 4 | 7 vs 7/8 | 8 -> 1 | 5: 5/8 |
| 5 | 5 vs 5/8 | 8 -> 3 | 6: 8/8 |
| 7 | 2 vs 2/8 | 8 -> 6 | 8: 6/8 |
| 8 | 6 vs 6/8 | 8 -> 2 | 9: 4/8 |
| 9 | **9 vs 4/8** | 8 -> 4 | 10: 2/8 |
| 10 | 2 vs 2/8 | 8 -> 6 | 11: 3/8 |
| 11 | 3 vs 3/8 | 8 -> 5 | 12: 8/8 |
| 13 | 7 vs 7/8 | 8 -> 1 | 14: 2/8 |
| 14 | 2 vs 2/8 | 8 -> 6 | 15: 6/8 |
| 15 | 6 vs 6/8 | 8 -> 2 | 16: 5/6 |
| 16 | **11 vs 5/6** | 6 -> 1 | 17: 2/2 |

The last repair had one real ARQ frame plus one ULPAD member. Identity accounting
correctly excluded ULPAD while the physical callback still reported 2/2.

Twelve light repairs avoided twelve 1.2-second full DATA anchors. Holding the same
decode outcomes only for mechanical accounting, feature-off would have taken about
227.25 seconds / 1.802 kbit/s physical instead of 212.85 / 1.924, a 6.77% gain.
The keyed counterfactual is 200.06 seconds / 2.047 kbit/s instead of 185.66 / 2.206,
a 7.76% gain. This is not a channel-causal A/B because changing airtime shifts every
later burst within the fading process.

## Reference transfers

These are separate fading realizations and must not be treated as pairs.

| Profile | Physical kbit/s | Keyed kbit/s | NACK entries | Partials | Result |
|---|---:|---:|---:|---:|---|
| QPSK R3/4 cw3/Z81 BI0 | 1.890 | 2.143 | 11 | 7 | exact, teardown PASS |
| 8PSK R2/3 cw4/Z81 BI0, repair off | 1.979 | 2.260 | 32 | 10 | exact, teardown PASS |
| 8PSK R2/3 cw4/Z81 BI0, corrected repair on | 1.924 | 2.206 | 43 | 12 | exact, teardown PASS |
| Favorable 8PSK draw observed earlier | 2.494 | 2.862 | 9 | 6 | exact; not a default claim |

QPSK R3/4 is the steadier fallback. 8PSK R2/3 has the better upside but remains
more variable under MPG@20. These three neighboring references remain useful forensic
anchors, but the order-balanced result above supersedes them for PSDR efficacy.

## Why 3,000 bit/s is not reached yet

For cw4/Z81 8PSK R2/3, one file frame carries 624 file bytes and occupies 59,360
samples (1.236667 s). A descriptor costs 67,680 samples (1.410 s), and the TX wrapper
adds 0.200 s per burst. A clean N8 steady cycle with the measured turnaround is about
3.02 kbit/s.

A complete 50 KiB transfer needs 84 unique frames: FILE_START plus 83 FILE_DATA
frames. Even with zero loss it needs eleven groups and a short tail:

```text
84 frames      x 1.236667 s = 103.880 s
11 descriptors x 1.410000 s =  15.510 s
11 TX guards   x 0.200000 s =   2.200 s
10 turnarounds x 1.69-1.71 s =  16.9-17.1 s
                                 -----------
physical span                    138.49-138.69 s
payload goodput                    2.95-2.96 kbit/s
```

Therefore the current full-file format is slightly below 3,000 bit/s even at zero
frame loss. The corrected 1.924-kbit/s proof run transmitted 128 physical frame slots
for 84 unique frames: 43 retransmissions plus one ULPAD member. That failure burden,
not ACK-decision CPU time, is the dominant present gap. Deleting every non-keyed
interval would still leave that run at only 2.206 kbit/s.

## Fixes landed in this investigation

1. Disconnect sentinel ACKs now use a full anchor on every initial, proactive, and
   reactive copy, and the receive expectation survives `DISCONNECTING`. Fresh QPSK
   and 8PSK transfers proved byte-exact teardown.
2. Descriptor-only repair now uses exact before/after ARQ frame identities rather
   than cumulative progress. Stale/prior ACKs, crater, timeout, BI1, singleton,
   transition, geometry mismatch, and unproven provenance still fail closed.
3. Connected-session teardown clears learned Z81 geometry so it cannot leak into a
   later peer/session.
4. FILE_START now has an explicit eleven-byte minimum. Forced QPSK R1/3 cw1
   (8-byte payload) fails before wire TX instead of wrapping metadata length and
   emitting a truncated start; a later 8-byte FILE_DATA profile remains supported
   after valid metadata.
5. Learned Z81 geometry now uses a dedicated mutex and session epoch. A disconnect
   cannot race a decode-thread resolver, and a descriptor decode begun in the old
   session cannot repopulate the table after teardown.
6. `sendFile()` validates FILE_START capacity before accepting a deferred request.
   If geometry shrinks after queueing, the request produces exactly one terminal
   failure callback instead of being cleared and silently lost.

## Verification

- Frozen pre-transfer Mac suite: 101/101 enabled tests passed; `TNCSession` is
  intentionally disabled in that configuration.
- Frozen Pi focused suite: Protocol, ConnectionPolicy, ConnectionAdaptive,
  StreamingConfig, and StreamingSignalPolicy passed 5/5.
- Post-FILE_START normal and ASan+UBSan focused suites:
  `FileTransferController` 129/129 and `ConnectionAdaptive` 696/696.
- Final post-fix full build passed. The complete loopback-enabled CTest run passed
  101/101 enabled tests in 174.42 seconds, including the 77.50-second byte-exact
  `UltraTncSimAudio`; `TNCSession` remains intentionally disabled by configuration.
- After the final geometry-race and queued-file fixes, a complete normal rebuild
  passed. Normal and ASan+UBSan focused runs passed `ConnectionAdaptive`,
  `FileTransferController`, and `BurstStaleGeometry`; the latter exercises a real
  reader thread across 256 session transitions plus stale-epoch rejection.
- The final single-invocation, loopback-enabled CTest passed 101/101 enabled tests
  in 174.24 seconds, including byte-exact `UltraTncSimAudio` in 78.03 seconds;
  `TNCSession` is the one intentionally disabled test.
- `git diff --check` is clean. No commit was created by this investigation.

## Recommended next work, in order

1. Do not enable the current descriptor-only repair. Redesign it so a missed robust
   descriptor cannot strand the following light group, or fail closed after a severe
   partial. Size any new trial from the measured 0.194 log-ratio SD and predeclare it.
2. Attack the physical-frame failure burden with post-pilot channel/SINR and
   decoder/HARQ evidence. Do not tune the selector from LTS NMSE or GUI SNR labels;
   selection must use acquisition probability plus post-equalizer effective SINR/PER.
3. Once repairs are near zero, reclaim structural overhead. The safest candidates are
   versioned FILE_START+first-data packing (about one frame per file) and separately
   reducing audio callback latency without reducing the total jitter cushion. The raw
   8192 -> 4096 flip already caused missed ACKs/timeouts and is not safe as a default.
4. Move raw-audio draining off the GUI/render thread into a dedicated bounded pump
   (preferably SPSC), and log FIFO high-water plus per-stall dropped samples. Enlarging
   the FIFO only hides the pair-1 apparatus failure and adds latency.
5. Add a hardware duty governor before treating the current N8 geometry as a 100 W
   radio default. This simulator run spent 87.2% of its transfer span in sender DATA
   key-down; a real final cannot be assumed to tolerate that continuously.
