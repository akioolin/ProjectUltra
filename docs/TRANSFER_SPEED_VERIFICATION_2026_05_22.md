# Transfer Speed Verification - 2026-05-22

Branch: `feat/16qam-promotion-2026-05-21`

Scope: independent verification of data-phase file-transfer speed in
`cli_simulator`/OTASim. No PHY or ARQ behavior was changed for the reported
runs. I used a temporary local measurement probe to count transfer-loop pump
iterations and snapshot `getSimTime()` around `sendFile()`; that probe was
removed after measurement.

## Verdict

I agree with the substance of Claude's claim: the old "On-air goodput" number is
not the delivered data-phase throughput. It is delivered file bits divided by
keyed TX waveform samples only. It excludes real elapsed half-duplex gaps, so it
overstates operator-visible throughput by about 1.65x to 2.35x on the QAM16
R1/2 Good/SNR20 20 KB cells measured here.

The honest handshake-excluded data-phase speed for QAM16 R1/2 Good/SNR20 20 KB
is:

| Seed | On-air bps | True data-phase bps | On-air/true | Dead-air shown by CLI | MPG/20 leader fraction |
|---:|---:|---:|---:|---:|---:|
| 42 | 2330 | 1411 | 1.65x | 39% | 45.7% |
| 43 | 1911 | 853 | 2.24x | 55% | 27.6% |
| 44 | 1609 | 686 | 2.35x | 57% | 22.2% |
| Mean | 1950 | 983 | 1.98x | 50% | 31.9% |

The industry-leader MPG/20 target in `docs/COMPETITIVE_BENCHMARK_TARGET.md` is
23,142 bytes/min, approximately 3086 bps. The honest QAM16 20 KB Good/SNR20
result is therefore not 64-75% of the leader. It is 22-46% by seed, with a
three-seed mean of about 32%.

## Perspective Checks

- PHY theorist: keyed waveform airtime is useful for PHY efficiency, but a
  delivered-rate denominator must include intervals where the half-duplex channel
  carries no payload bits.
- Real-time DSP systems engineer: OTASim wall-clock is CPU-paced and unusable
  for throughput; the 48 kHz sample clock is the stable timing reference.
- Veteran HF operator: after the link is up, the operator-visible file speed is
  the elapsed time until the file is received, including turnaround, ACK waits,
  retransmission waits, and silent gaps.
- First-principles check: throughput is delivered bits divided by elapsed time.
  Any 10 ms sample-clock callback where neither side emits useful waveform has a
  payload rate of zero for that interval.

## Clock Semantics

Confirmed in code:

- `tools/sim/simulated_station.hpp:744-770`: `pullTxSamples()` always adds the
  requested `count` to `tx_sample_clock_`, but adds only copied queued waveform
  samples to `tx_emitted_sample_clock_`.
- `tools/sim/simulated_station.hpp:1054-1065`: `txSampleClock()` is documented
  as the continuous callback cursor; `txEmittedSampleClock()` excludes idle
  pulls.
- `tools/sim/simulated_station.hpp:911-924`: `sampleClockPullTx()` calls
  `pullTxSamples()` when the audio port is paced in the station loop. The
  default `OtaAudioPort` path is paced this way; `HardwareAudioPort` is not.
- `tools/sim/simulated_station.hpp:944-957`: `getSimTime()` advances from
  `total_samples_`, which is incremented by pushed RX callback samples.
- `tools/cli_simulator.cpp:1758-1778`: the sample counters are snapshotted
  after CONNECT and mode negotiation, immediately before `sendFile()`, so the
  robust handshake is excluded from the metric.
- `tools/cli_simulator.cpp:1786-1803`: the file-transfer loop calls
  `pumpOtaSampleClockOnce()` exactly once per loop iteration until
  `file_received_` becomes true.
- `tools/cli_simulator.cpp:2053-2072`: one pump pulls 480 TX samples from ALPHA
  and BRAVO, queues them, pulls 480 RX samples, pushes them, then advances both
  protocol timers by 10 ms.

Important guard: this interpretation is valid for `--role both` OTASim
sample-clock mode. It is not a general hardware-mode statement, because hardware
TX is paced by SDL rather than by `sampleClockPullTx()`.

## Independent Measurements

The independent clocks were:

1. Transfer-loop pump count times `SimulatedStation::CALLBACK_INTERVAL_MS`
   (10 ms).
2. `alpha_->getSimTime()` and `bravo_->getSimTime()` deltas around
   `sendFile()`/`file_received_`.

They agreed exactly to the printed 0.01 s precision on every run.

| Cell | Seed | Negotiated data mode | File | On-air time / bps | Pump-clock time / bps | `getSimTime()` delta / bps | Dead-air |
|---|---:|---|---:|---:|---:|---:|---:|
| QAM16 R1/2 Good/SNR20 | 42 | OFDM-CHIRP 16QAM R1/2 cw=8 | 20480 B | 70.32 s / 2330 | 116.15 s / 1411 | 116.15 s / 1411 | 39% |
| QAM16 R1/2 Good/SNR20 | 43 | OFDM-CHIRP 16QAM R1/2 cw=8 | 20480 B | 85.72 s / 1911 | 192.04 s / 853 | 192.04 s / 853 | 55% |
| QAM16 R1/2 Good/SNR20 | 44 | OFDM-CHIRP 16QAM R1/2 cw=8 | 20480 B | 101.83 s / 1609 | 238.87 s / 686 | 238.87 s / 686 | 57% |
| 8PSK R1/2 Good/SNR20 | 42 | OFDM-CHIRP 8PSK R1/2 cw=8 | 5120 B | 25.59 s / 1601 | 42.46 s / 965 | 42.46 s / 965 | 40% |
| 8PSK R1/2 Good/SNR20 | 42 | OFDM-CHIRP 8PSK R1/2 cw=8 | 20480 B | 111.34 s / 1472 | 243.40 s / 673 | 243.40 s / 673 | 54% |
| DQPSK auto Good/SNR12 guard | 42 | OFDM-CHIRP DQPSK R1/4 cw=4 | 1024 B | 20.97 s / 391 | 37.03 s / 221 | 37.03 s / 221 | 43% |

## Reproducer Commands

Run from the repository root; each command invokes `./build/cli_simulator` as
required and self-spawns OTASim.

```bash
./build/cli_simulator --channel good --snr 20 --seed 42 --file 20480 --waveform ofdm_chirp --expert --mod qam16 --rate r1_2 --log-level error --phy-diag-log /tmp/transfer_verify_qam16_r12_good20_20kb_s42.diag
./build/cli_simulator --channel good --snr 20 --seed 43 --file 20480 --waveform ofdm_chirp --expert --mod qam16 --rate r1_2 --log-level error --phy-diag-log /tmp/transfer_verify_qam16_r12_good20_20kb_s43.diag
./build/cli_simulator --channel good --snr 20 --seed 44 --file 20480 --waveform ofdm_chirp --expert --mod qam16 --rate r1_2 --log-level error --phy-diag-log /tmp/transfer_verify_qam16_r12_good20_20kb_s44.diag
./build/cli_simulator --channel good --snr 20 --seed 42 --file 5120 --waveform ofdm_chirp --expert --mod 8psk --rate r1_2 --log-level error --phy-diag-log /tmp/transfer_verify_8psk_r12_good20_5kb_s42.diag
./build/cli_simulator --channel good --snr 20 --seed 42 --file 20480 --waveform ofdm_chirp --expert --mod 8psk --rate r1_2 --log-level error --phy-diag-log /tmp/transfer_verify_8psk_r12_good20_20kb_s42.diag
./build/cli_simulator --channel good --snr 12 --seed 42 --file 1024 --mod dqpsk --rate auto --log-level error --phy-diag-log /tmp/transfer_verify_dqpsk_auto_good12_1kb_s42.diag
```

The temporary independent-clock probe printed these matching lines during the
measurement pass:

| Cell | Probe output |
|---|---|
| QAM16 s42 | `pump_ticks=11615 pump_sec=116.15 pump_bps=1411 alpha_sim_delta=116.15 bravo_sim_delta=116.15 sim_bps=1411` |
| QAM16 s43 | `pump_ticks=19204 pump_sec=192.04 pump_bps=853 alpha_sim_delta=192.04 bravo_sim_delta=192.04 sim_bps=853` |
| QAM16 s44 | `pump_ticks=23887 pump_sec=238.87 pump_bps=686 alpha_sim_delta=238.87 bravo_sim_delta=238.87 sim_bps=686` |
| 8PSK 5 KB s42 | `pump_ticks=4246 pump_sec=42.46 pump_bps=965 alpha_sim_delta=42.46 bravo_sim_delta=42.46 sim_bps=965` |
| 8PSK 20 KB s42 | `pump_ticks=24340 pump_sec=243.40 pump_bps=673 alpha_sim_delta=243.40 bravo_sim_delta=243.40 sim_bps=673` |
| DQPSK 1 KB s42 | `pump_ticks=3703 pump_sec=37.03 pump_bps=221 alpha_sim_delta=37.03 bravo_sim_delta=37.03 sim_bps=221` |

## Dead-Air Decomposition

The dead air is physically real as elapsed time in the half-duplex schedule:
when neither station is emitting useful waveform, delivered bits per second is
zero. A real HF operator would experience that silence. The uncomfortable part
is that not all of it is unavoidable physics.

Static timing that is plausibly radio-real:

- PTT model: 20 ms TX/RX switch plus 100 ms cooldown in
  `RadioPttStateMachine`.
- Carrier-sense guard: 50 ms default in `SimulatedStation::DEFAULT_TR_GUARD_MS`.
- ACK airtime itself: counted in on-air time, not dead air.
- Some decode/search latency and channel-settling time is real in an SDR/audio
  chain, but the measured CPU decode totals are far smaller than the multi-second
  gaps.

Protocol/tuning components that look recoverable:

- OFDM does not use the ARQ default 2000 ms SACK delay in these cells. The OFDM
  path configures a physical SACK hold of roughly one sender window plus 30 ms.
  For QAM16 R1/2 cw=8/window=8, the observed 8-frame burst is 6.80 s, so the
  hold is about 6.83 s. That is deliberate half-duplex coalescing, but it is a
  large contributor.
- Repeated ACK copies create visible ACK-to-ACK idle spacing. In QAM16 s42, the
  ACK-repeat gap bucket was about 9.1 s; in s43 it was about 27.8 s; in s44 it
  was about 12.4 s.
- Retransmission and timeout waits dominate the bad seeds. QAM16 s42 had 4
  retransmissions and 1 timeout; QAM16 s43 had 23 retransmissions and 19
  timeouts; QAM16 s44 had 41 retransmissions and 36 timeouts.

PHY-diagnostic interval clipping, limited to the handshake-excluded data phase,
gave this no-TX idle decomposition for QAM16 R1/2 20 KB:

| Seed | No-TX idle | DATA->ACK gaps | ACK->DATA gaps | DATA->DATA timeout/hole gaps | ACK->ACK repeat gaps | Guard floor estimate |
|---:|---:|---:|---:|---:|---:|---:|
| 42 | 46.6 s | 25.4 s | 12.1 s | 0.0 s | 9.1 s | 4.5-5.4 s |
| 43 | 108.0 s | 20.4 s | 25.8 s | 32.1 s | 27.8 s | 8.0-9.7 s |
| 44 | 138.8 s | 36.5 s | 53.8 s | 34.1 s | 12.4 s | 12.2-14.8 s |

The guard floor estimate uses 140-170 ms per observed turn gap, matching the
normal short gaps in the trace and the 20 ms + 100 ms + 50 ms timing model. That
means most of the measured dead air is not irreducible RF turnaround. It is
mostly SACK/window scheduling, ACK repetition, and retransmission/timeout
behavior. Those are real elapsed time today, but they are optimization targets.

There is one subtle simulator artifact/risk: a few traces show small overlaps
between active DATA and ACK intervals when reconstructed from per-station
`station_tx_active` diagnostics. The overlap was small relative to the finding
(about 0.8 s in QAM16 s42, 1.6 s in s43, 1.8 s in s44, and 4.0 s in 8PSK 20 KB).
It does not rescue the headline throughput; if anything, it means the keyed
sample sum is not a perfect "channel occupied" metric either. The elapsed
sample-clock span remains the correct delivered-rate denominator.

## Comparison Judgment

Use end-to-end data-phase goodput, not keyed on-air goodput, for the leader
comparison. The benchmark document says the leader table is published
bytes/min and that bytes/min includes ARQ and protocol overhead. That is a
delivered-payload rate, not raw PHY airtime. The best apples-to-apples ProjectUltra
number is therefore:

```text
delivered file bits / elapsed data-phase sample-clock time
```

The user requirement is handshake-excluded, and the snapshot point satisfies
that. If the leader's published bytes/min includes connection setup, this
comparison is generous to us. If it excludes setup but includes ACK/ARQ, this is
the right comparison.

Bottom line: on Good/SNR20, the honest ProjectUltra data-phase result for the
QAM16 R1/2 20 KB headline cell is 686-1411 bps by seed, with a three-seed mean
near 983 bps. Against 3086 bps for the leader's MPG/20 table, that is 22-46% by
seed, about 32% on average.
