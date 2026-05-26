# Good@20 Coherent QPSK Phase-1 Diagnosis

Scope: `feat/good-fading-qam16-ladder-2026-05-24`, GUI auto-path only, no expert forcing. Scenario command shape:

```bash
./tools/qam16_ladder_scenario.sh --channel good --snr-db 20 --seed N --expect-mod QPSK --expect-rate R2/3 --out /tmp/phase1_good20_qpsk_r23_seedN
```

The harness was first corrected to match `QAM16_LADDER_TEST_SCENARIO.md`: three scripted messages in each direction before the 10 KB file, and pass/fail now requires both ALPHA and BRAVO to receive the messages.

## Perspective Stack

1. PHY theorist: the link must survive Good fading with coherent pilots and soft metrics before throughput tuning matters.
2. Real-time DSP systems engineer: end-to-end GUI timing and OTASim transport are part of the measurement; ARQ timeout behavior is treated as a symptom unless the audio transport drops samples.
3. Veteran HF operator: Good fading still has deep fades; a usable link must degrade coherently and finish cleanly without manual forcing.
4. First-principles escape hatch: if the current PHY raw ceiling is below the leader, no timer or ACK tweak can close the full gap.

## Seed Results

All runs used full debug GUI logging. Diagnostic reruns are not counted in the baseline pass rate.

| Seed | Auto-selected data mode | Result | Goodput bps | Retx | ALPHA CWFAIL | BRAVO CWFAIL | Notes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| 1 | QPSK R2/3 | PASS | 1360 | 0 | 0 | 0 | clean coherent run |
| 2 | QPSK R2/3 | FAIL | 0 | 17 | 0 | 190 | BRAVO hard-fade decode failures; file incomplete |
| 3 | QPSK R2/3 | PASS | 1350 | 2 | 16 | 0 | recovered, but PHY-induced retransmits |
| 4 | DQPSK R1/2 | FAIL | 400 | 6 | 0 | 8 | ladder missed coherent rung; later DQPSK R1/4 |
| 5 | QPSK R2/3 | PASS | 1350 | 0 | 0 | 0 | clean coherent run |
| 6 | DQPSK R1/2 | FAIL | 1380 | 1 | 0 | 8 | ladder missed coherent rung at peer SNR 19.8, fading 0.70 |

Baseline canonical pass rate: 3/6. Coherent-QPSK selection rate: 4/6. Coherent-QPSK pass rate when selected: 3/4. Clean coherent-QPSK rate, with zero CWFAIL and zero retransmits: 2/4.

Transport was clean in the E2E logs for all six seeds: `enqueued=0` occurrences, nonzero `rx_pending_blocks` occurrences, underrun, overrun, client fail, and server fail counters were all zero. ARQ retransmissions correlate with PHY CW failures or ladder/mode events, not with OTASim transport drops.

## Reliability Finding

The residual reliability problem is still PHY/equalizer/soft-metric limited on top of the Wiener estimator baseline. Seed 2 failed in the real GUI path with QPSK R2/3 selected, `BRAVO_CWFAIL_COUNT=190`, 17 ALPHA retransmissions, and no file completion.

A separate diagnostic rerun of seed 2 with `ULTRA_FAILURE_ATTRIBUTION=1` was not counted in the pass rate, but it exposed QPSK equalizer diagnostics:

- Failed frame at 66.003 s: QPSK R2/3, `cw_ok=0`, `cw_fail=8`, SNR 20.11 dB, corr 0.663, `inside_noise_frac=0.891`, `llr_to_empirical_sigma2=1.947`, mean carrier SNR 4.037 dB.
- Failed frame at 100.869 s: QPSK R2/3, `cw_ok=1`, `cw_fail=7`, SNR 19.91 dB, corr 0.915, `inside_noise_frac=0.495`, `norm_evm_p95=15.427`, `llr_to_empirical_sigma2=0.506`, CPE mean 13.352 deg.

That second frame is the key symptom: sync correlation and nominal broadband SNR are good, but only about half of equalized samples fit the assumed post-equalizer noise model and the LLR sigma is about half the empirical residual variance. This is consistent with stale or incomplete channel tracking and overconfident soft metrics, not a transport problem and not an ARQ-timer root cause.

True GUI-path genie channel MSE was not available in this run. The repo has QAM16-specific diagnostic approximations, but the live GUI QPSK path did not expose a simulator true-H tap. The diagnostic evidence above is therefore post-equalizer residual/LLR evidence, not a perfect-H MSE measurement.

## Dead-Air Finding

Clean seed 1 file phase:

- ALPHA file send: 74.728 s.
- BRAVO CRC-ok receive: 137.903 s, 60.3 s receiver-reported file time, 1.36 kbps.
- ALPHA transfer complete: 139.194 s.
- TX samples from ALPHA send through ALPHA completion: 2,374,560 samples = 49.47 s keyed airtime.
- Sender-side file span: 64.466 s.
- Dead-air fraction: 23.3%.
- Payload over keyed airtime: about 1,656 bps.

Dead-air is real, but it is not the dominant clean-seed gap. Removing all clean-seed dead-air would raise 1.36 kbps only to about 1.66 kbps. In failing seed 2, dead-air rose to 64.3%, but that is ARQ recovery after PHY loss, not a primary timer issue.

## Rung-Structure Finding

The in-use coherent Good@20 QPSK rung is already close to the leader's carrier geometry:

- FFT: 1024 at 48 kHz.
- Carrier spacing: 46.875 Hz.
- Carriers: 59 over about 2766 Hz.
- CP: 128 samples from LONG CP mode, so symbol samples are 1152.
- Symbol rate: 48000 / 1152 = 41.667 baud.
- Pilot profile for coherent R2/3: spacing 5, 12 pilots, 47 data carriers.
- Modulation/code: QPSK R2/3.
- Raw information ceiling: 47 data carriers * 2 bits * 41.667 baud * 2/3 = 2611 bps.

The market-leader target is about 3086 bps at Good@20. Therefore the current QPSK R2/3 rung cannot mathematically match the leader even with perfect reliability and zero dead-air; its raw ceiling is only about 84.6% of the target.

Using seed 1 as the clean decomposition:

- Current receiver goodput: 1360 bps.
- Keyed-air payload rate: about 1656 bps.
- Current raw ceiling: about 2611 bps.
- Market leader target: about 3086 bps.

Gap from 1360 to 3086: 1726 bps. Clean dead-air accounts for about 296 bps of that gap. Raw-to-keyed implementation/protocol/burst overhead accounts for about 955 bps. The current raw ceiling is still 475 bps below the leader.

## Ladder Finding

Seeds 4 and 6 did not select coherent QPSK at all. They selected DQPSK R1/2 because the measured negotiation point landed just below or outside the QPSK gate:

- Seed 4: ALPHA connected at DQPSK R1/2, peer SNR 19.8 dB, peer fading 0.50; later downgraded to DQPSK R1/4.
- Seed 6: ALPHA connected at DQPSK R1/2, peer SNR 19.8 dB, peer fading 0.70.

Per the canonical scope, this is not a differential regression to chase. It is an auto-ladder/coherent-endgame finding: Good@20 does not reliably enter the coherent QPSK rung without forcing.

## Dominant Lever

Dominant immediate lever: coherent PHY reliability and soft channel tracking. The next fix should build on the existing scattered-pilot/Wiener/MMSE baseline with the allowed live levers: DPSS/Slepian time-basis tracking and/or SOFT-CD3 post-FEC virtual pilots. Delay-domain sparsity should not be retried.

Why not dead-air first: clean-seed dead-air can recover only about 296 bps, and failing-seed dead-air is caused by PHY loss recovery.

Why not rung structure first: the raw ceiling must be raised before we can match or exceed 3086 bps, but the multi-seed gate is already failing. A faster rung without fixing the QPSK hard-fade CWFAIL tail would increase throughput only on lucky seeds.

After coherent reliability is 3/3 and then 20-seed stable, the speed campaign must address rung structure/finer rates because the current QPSK R2/3 raw ceiling is below the leader.
