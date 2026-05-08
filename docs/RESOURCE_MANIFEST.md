# Resource Manifest

Last updated: 2026-05-08

Scope: binary resources under `recordings/`, `fixtures/`, and `tests/data/`.
Reference checks used basenames against source/header files, CMake files,
README/docs/agent markdown, and repository fixture/capture notes. No artifacts
were deleted in this pass.

## Live Fixtures

| File | Size | Where referenced |
|------|-----:|------------------|
| `fixtures/ofdm_chirp_r14_dqpsk_clean.wav` | 576044 B | `fixtures/README.md`, `tests/test_decode_bench_replay.cpp`, `docs/KNOWN_BUGS.md` |
| `fixtures/ofdm_chirp_r14_dqpsk_snr15_awgn.wav` | 576044 B | `fixtures/README.md`, `tests/test_decode_bench_replay.cpp` |
| `fixtures/ofdm_chirp_r12_dqpsk_snr15_awgn.wav` | 144044 B | `fixtures/README.md`, `tests/test_decode_bench_replay.cpp` |
| `fixtures/ofdm_chirp_r34_dqpsk_snr15_awgn.wav` | 144044 B | `fixtures/README.md`, `tests/test_decode_bench_replay.cpp` |
| `fixtures/ofdm_chirp_r14_dqpsk_snr15_good.wav` | 144044 B | `fixtures/README.md`, `tests/test_decode_bench_replay.cpp` |
| `fixtures/ofdm_chirp_r12_dqpsk_snr15_good.wav` | 144044 B | `fixtures/README.md`, `tests/test_decode_bench_replay.cpp` |
| `fixtures/ota_test_r14_15s.wav` | 2837804 B | `fixtures/README.md` |
| `recordings/ota_capture_2026-05-07_k1vl/ota_r1_2_kc3vpb_to_k1vl.wav` | 1.2M | `recordings/ota_capture_2026-05-07_k1vl/RESULTS.md:33`, folder referenced by `README.md:463` |
| `recordings/ota_capture_2026-05-07_k1vl/ota_r1_4_kc3vpb_to_k1vl.wav` | 964K | `recordings/ota_capture_2026-05-07_k1vl/RESULTS.md:34`, folder referenced by `README.md:463` |
| `recordings/ota_capture_2026-05-07_k1vl/ota_r3_4_kc3vpb_to_k1vl.wav` | 836K | `recordings/ota_capture_2026-05-07_k1vl/RESULTS.md:35`, folder referenced by `README.md:463` |
| `recordings/ota_full_session_2026-05-07/full_session_r1_2.wav` | 4.6M | `docs/CHANGELOG.md:72`, folder referenced by `docs/CHANGELOG.md:34` and `recordings/ota_capture_2026-05-07_k1vl/RESULTS.md:6` |
| `recordings/ota_full_session_2026-05-07/full_session_r1_4.wav` | 4.5M | Folder referenced by `docs/CHANGELOG.md:34` and `recordings/ota_capture_2026-05-07_k1vl/RESULTS.md:6` |
| `recordings/ota_full_session_2026-05-07/full_session_r3_4.wav` | 4.6M | Folder referenced by `docs/CHANGELOG.md:34` and `recordings/ota_capture_2026-05-07_k1vl/RESULTS.md:6` |
| `tests/data/test_connect_data_sequence.f32` | 2.5M | `src/gui/app.cpp:1722` |

## Historical Artifacts

| File | Size | Last-known context | Proposed disposition |
|------|-----:|--------------------|----------------------|
| `tests/data/archive/f3_deadbeef_3bursts.raw` | 4.1M | `2156053 Add hardware audio test recordings for OFDM verification` | Archived; keep until a replay harness uses or explicitly rejects it. |
| `tests/data/archive/f3_fresh_recording.raw` | 1.8M | `2156053 Add hardware audio test recordings for OFDM verification` | Archived; keep until a replay harness uses or explicitly rejects it. |
| `tests/data/archive/f6_hardware_recording.f32` | 940K | `2156053 Add hardware audio test recordings for OFDM verification` | Archived; keep until a replay harness uses or explicitly rejects it. |
| `tests/data/archive/f6_hardware_recording.raw` | 472K | `2156053 Add hardware audio test recordings for OFDM verification` | Archived; keep until a replay harness uses or explicitly rejects it. |
| `tests/data/archive/ofdm_deadbeef_hardware_100pct.f32` | 236K | `2156053 Add hardware audio test recordings for OFDM verification` | Archived; keep until a replay harness uses or explicitly rejects it. |
| `tests/data/archive/probe_hardware_recording.f32` | 1.1M | `2156053 Add hardware audio test recordings for OFDM verification` | Archived; keep until a replay harness uses or explicitly rejects it. |
| `tests/data/archive/probe_hardware_recording.raw` | 564K | `2156053 Add hardware audio test recordings for OFDM verification` | Archived; keep until a replay harness uses or explicitly rejects it. |
| `tests/data/archive/probe_ldpc_fixed_20260118.f32` | 1.1M | `9b4357c Add test recording: PROBE with fixed LDPC encoding (2026-01-18)` | Archived; keep until a replay harness uses or explicitly rejects it. |
| `tests/data/archive/probe_ldpc_mac_2026-01-18.f32` | 1.5M | `8bd064d Add LDPC probe test recording from Mac` | Archived; keep until a replay harness uses or explicitly rejects it. |
| `tests/data/archive/v2_connect_3cw_hardware.f32` | 1.8M | `a1294f4 Add timing offset support to prx command for hardware recordings` | Archived; keep until a replay harness uses or explicitly rejects it. |
| `tests/data/archive/v2_connect_marker_index_verified.f32` | 1.8M | `ac3bf63 Add verified hardware recording with marker+index codeword format` | Archived; keep until a replay harness uses or explicitly rejects it. |
