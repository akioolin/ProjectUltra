# Throughput Recovery Attempt - 2026-05-22

## Status

No transport-timing change from this round is kept or committed.

The primary QAM16 R1/2 Good/SNR20 20 KB target did not improve under any
principled Phase-1 prototype tested here. Each attempted change either regressed
seed 42, regressed the mean, or increased timeout/retransmission pressure. Code
was restored to branch baseline after each rejected prototype.

Baseline from `docs/TRANSFER_SPEED_VERIFICATION_2026_05_22.md`:

| Cell | Seed | On-air bps | End-to-end bps | Dead air | ALPHA retx/timeouts/failed |
| --- | ---: | ---: | ---: | ---: | --- |
| QAM16 R1/2 Good/SNR20 20 KB | 42 | 2330 | 1411 | 39% | 4/1/0 |
| QAM16 R1/2 Good/SNR20 20 KB | 43 | 1911 | 853 | 55% | 23/19/0 |
| QAM16 R1/2 Good/SNR20 20 KB | 44 | 1609 | 686 | 57% | 41/36/0 |

## Rejected Prototypes

All commands used the trusted end-to-end line from `cli_simulator`.

### RTO Physical Ceiling

Command:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 20480 --seed 44 --log-level error --phy-diag-log /tmp/recovery_rto_qam16_r12_good20_20kb_s44.diag
```

Result: 345 bps end-to-end, 1356 bps on-air, 75% dead air. ALPHA
retx/timeouts/failed = 66/61/0. BRAVO decoded/failed/success = 104/15/87.4%.

Rejected: raising the effective RTO floor delayed real loss recovery and made
the bad seed worse.

### Queue-Aware RTO

Command:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 20480 --seed 44 --log-level error --phy-diag-log /tmp/recovery_rto2_qam16_r12_good20_20kb_s44.diag
```

Result: 671 bps end-to-end, 1840 bps on-air, 64% dead air. ALPHA
retx/timeouts/failed = 26/23/0. BRAVO decoded/failed/success = 84/11/88.4%.

Rejected: fewer timeouts did not translate to trusted goodput; seed 44 still
regressed from 686 to 671 bps.

### ACK Repeat Good=2

Commands:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 20480 --seed 44 --log-level error --phy-diag-log /tmp/recovery_ack2_only_qam16_r12_good20_20kb_s44.diag
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 20480 --seed 43 --log-level error --phy-diag-log /tmp/recovery_ack2_only_qam16_r12_good20_20kb_s43.diag
```

Results:

| Seed | End-to-end bps | On-air bps | Dead air | ALPHA retx/timeouts/failed | BRAVO decoded/failed/success |
| ---: | ---: | ---: | ---: | --- | --- |
| 44 | 707 | 1843 | 62% | 27/25/0 | 82/12/87.2% |
| 43 | 486 | 1197 | 59% | 94/94/0 | 120/25/82.8% |

Seed 42 did not complete cleanly and hit max-retry behavior during the prototype.

Rejected: Good fading still needs full ACK diversity; reducing copies creates
timeout trains on primary cells.

### Shorter SACK Holds

Commands:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 20480 --seed 42 --log-level error --phy-diag-log /tmp/recovery_sack30_qam16_r12_good20_20kb_s42.diag
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 20480 --seed 43 --log-level error --phy-diag-log /tmp/recovery_sack30_qam16_r12_good20_20kb_s43.diag
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 20480 --seed 44 --log-level error --phy-diag-log /tmp/recovery_sack30_qam16_r12_good20_20kb_s44.diag
```

30 ms SACK hold results:

| Seed | End-to-end bps | On-air bps | Dead air | ALPHA retx/timeouts/failed | BRAVO decoded/failed/success |
| ---: | ---: | ---: | ---: | --- | --- |
| 42 | 595 | 1388 | 57% | 60/58/0 | 91/18/83.5% |
| 43 | 719 | 1444 | 50% | 49/44/0 | 91/17/84.3% |
| 44 | 810 | 1600 | 49% | 39/32/0 | 79/18/81.4% |

Half-window SACK hold results:

| Seed | End-to-end bps | On-air bps | Dead air | ALPHA retx/timeouts/failed | BRAVO decoded/failed/success |
| ---: | ---: | ---: | ---: | --- | --- |
| 42 | 1070 | 2168 | 51% | 8/7/0 | 73/5/93.6% |
| 43 | 729 | 1867 | 61% | 21/18/0 | 78/9/89.7% |
| 44 | 806 | 1613 | 50% | 40/39/0 | 83/20/80.6% |

One-frame-shorter remaining-window SACK hold:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 20480 --seed 42 --log-level error --phy-diag-log /tmp/recovery_sack_remaining_qam16_r12_good20_20kb_s42.diag
```

Result: 1334 bps end-to-end, 2293 bps on-air, 42% dead air. ALPHA
retx/timeouts/failed = 5/1/0. BRAVO decoded/failed/success = 73/3/96.1%.

Rejected: every shorter hold regressed seed 42 or the three-seed mean. The
full SACK hold is protecting real half-duplex scheduling, not only idle time.

### Full-Window SACK Through MORE_FRAG

Immediate threshold command:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 20480 --seed 42 --log-level error --phy-diag-log /tmp/recovery_window_sack_qam16_r12_good20_20kb_s42.diag
```

Result: 813 bps end-to-end, 1946 bps on-air, 58% dead air. ALPHA
retx/timeouts/failed = 22/19/0. BRAVO decoded/failed/success = 80/9/89.9%.

Guarded 170 ms threshold command:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 20480 --seed 42 --log-level error --phy-diag-log /tmp/recovery_guarded_sack_qam16_r12_good20_20kb_s42.diag
```

Result: 813 bps end-to-end, 1946 bps on-air, 58% dead air. ALPHA
retx/timeouts/failed = 22/19/0. BRAVO decoded/failed/success = 80/9/89.9%.

Rejected: even a physically guarded full-window threshold ACK destabilized the
primary clean seed.

### ACK Repeat Cancellation After Peer Advance

Command:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 20480 --seed 42 --log-level error --phy-diag-log /tmp/recovery_ack_cancel_qam16_r12_good20_20kb_s42.diag
```

Result: 749 bps end-to-end, 1845 bps on-air, 59% dead air. ALPHA
retx/timeouts/failed = 28/25/0. BRAVO decoded/failed/success = 80/14/85.1%.

Rejected: delayed clean ACK copies still carry useful diversity in Good fading;
causal-looking cancellation was not safe.

### Timeout Backoff

Command:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 20480 --seed 44 --log-level error --phy-diag-log /tmp/recovery_backoff_qam16_r12_good20_20kb_s44.diag
```

Result: 601 bps end-to-end, 1644 bps on-air, 63% dead air. ALPHA
retx/timeouts/failed = 42/41/0. BRAVO decoded/failed/success = 87/22/79.8%.

Rejected: backing off timeout retransmissions delayed recovery and increased
loss exposure on the bad seed.

## Multi-Perspective Conclusion

PHY theorist: the attempted dead-air cuts were not independent of RF/decoder
state. Earlier ACKs changed the interference/timing pattern and increased
decode failures or timeout trains.

Real-time DSP systems engineer: OTASim protocol timers and transmit queues make
apparently idle wall time part of the scheduling contract. Shortening SACK or
ACK-repeat behavior creates queue churn and control/data overlap that the on-air
metric hides but the trusted end-to-end metric exposes.

Veteran HF operator: Good fading is still a fading channel. The repeated ACKs
and conservative SACK hold look wasteful when the first copy succeeds, but they
protect real transfers when control copies fade or when the far side has not
settled into receive.

First-principles escape hatch: the current "dead air" decomposition includes
recoverable-looking gaps, but these prototypes show those gaps are coupled to
half-duplex channel occupancy and decode latency. A later win probably needs a
more explicit TX/RX state machine or receiver-ready signal, not just shorter
constants in ARQ timing.

