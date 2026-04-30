# Test suite for ProjectUltra

# Layer-by-layer OFDM primitive verification.
add_executable(test_layers test_layers.cpp)
target_link_libraries(test_layers PRIVATE ultra_core)
add_test(NAME Layers COMMAND test_layers)

add_executable(test_fft test_fft.cpp)
target_link_libraries(test_fft PRIVATE ultra_core)
add_test(NAME FFT COMMAND test_fft)

add_executable(test_ofdm test_ofdm.cpp)
target_link_libraries(test_ofdm PRIVATE ultra_core)
add_test(NAME OFDM COMMAND test_ofdm)

# Basic audio path test (no OFDM/LDPC - just raw samples)
# Use this to verify audio path between laptops
add_executable(test_audio_basic test_audio_basic.cpp)
add_test(NAME AudioBasic COMMAND test_audio_basic)

# WAV file loopback test (generate/decode WAV files for cross-wire testing)
add_executable(test_wav_loopback test_wav_loopback.cpp)
target_link_libraries(test_wav_loopback PRIVATE ultra_core)
add_test(NAME WavLoopback COMMAND test_wav_loopback --loopback)

add_executable(test_protocol test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE ultra_core)
add_test(NAME Protocol COMMAND test_protocol)

add_executable(test_interleaver test_interleaver.cpp)
target_link_libraries(test_interleaver PRIVATE ultra_core)
add_test(NAME Interleaver COMMAND test_interleaver)

# Multi-codeword LDPC test (file transfer reliability)
add_executable(test_multiblock_ldpc test_multiblock_ldpc.cpp)
target_link_libraries(test_multiblock_ldpc PRIVATE ultra_core)
add_test(NAME MultiblockLDPC COMMAND test_multiblock_ldpc)

# Selective Repeat ARQ tests
add_executable(test_selective_repeat test_selective_repeat.cpp)
target_link_libraries(test_selective_repeat PRIVATE ultra_core)
add_test(NAME SelectiveRepeatARQ COMMAND test_selective_repeat)

# Sync Detection Accuracy Test
# Tests fine-grained sync peak refinement (fixes ~118 sample offset issue)
add_executable(test_sync_detection test_sync_detection.cpp)
target_link_libraries(test_sync_detection PRIVATE ultra_core)
add_test(NAME SyncDetection COMMAND test_sync_detection)

# Comprehensive modem unit tests for LDPC, LLR, full-chain noise, and CFO.
add_executable(test_comprehensive_modem test_comprehensive_modem.cpp)
target_link_libraries(test_comprehensive_modem PRIVATE ultra_core)
add_test(NAME ComprehensiveModem COMMAND test_comprehensive_modem)

# RNG consistency test for cross-platform LDPC debugging
add_executable(test_rng test_rng.cpp)
add_test(NAME RNG COMMAND test_rng)

# Protocol v2 frame format tests
add_executable(test_frame_v2 test_frame_v2.cpp)
target_link_libraries(test_frame_v2 PRIVATE ultra_core)
add_test(NAME FrameV2 COMMAND test_frame_v2)

# Current waveform/rate policy and factory coverage
add_executable(test_waveform_policy test_waveform_policy.cpp)
target_link_libraries(test_waveform_policy PRIVATE ultra_core)
add_test(NAME WaveformPolicy COMMAND test_waveform_policy)

# Current streaming MC-DPSK encode/decode loopback.
add_executable(test_streaming_mc_dpsk
    test_streaming_mc_dpsk.cpp
    ../src/gui/modem/streaming_decoder.cpp
    ../src/gui/modem/streaming_encoder.cpp
)
target_include_directories(test_streaming_mc_dpsk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(test_streaming_mc_dpsk PRIVATE ultra_core)
add_test(NAME StreamingMCDPSK COMMAND test_streaming_mc_dpsk)

# Throughput measurement tool
add_executable(test_throughput ../tools/test_throughput.cpp)
target_link_libraries(test_throughput PRIVATE ultra_core)
