# Test suite for ProjectUltra

# Layer-by-layer OFDM primitive verification.
add_executable(test_layers test_layers.cpp)
target_link_libraries(test_layers PRIVATE ultra_core)
add_test(NAME Layers COMMAND test_layers)

add_executable(test_fft test_fft.cpp)
target_link_libraries(test_fft PRIVATE ultra_core)
add_test(NAME FFT COMMAND test_fft)

add_executable(test_dsp_primitives test_dsp_primitives.cpp)
target_link_libraries(test_dsp_primitives PRIVATE ultra_core)
add_test(NAME DSPPrimitives COMMAND test_dsp_primitives)

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

add_executable(test_tnc_session test_tnc_session.cpp)
target_link_libraries(test_tnc_session PRIVATE ultra_core)
add_test(NAME TNCSession COMMAND test_tnc_session)

# ultra_tnc config parsing — pure unit tests, no SDL/audio.
add_executable(test_ultra_tnc_config
    test_ultra_tnc_config.cpp
    ../tools/ultra_tnc_config.cpp
)
target_include_directories(test_ultra_tnc_config PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../tools
)
target_link_libraries(test_ultra_tnc_config PRIVATE ultra_core)
add_test(NAME UltraTNCConfig COMMAND test_ultra_tnc_config)

add_executable(test_tnc_server test_tnc_server.cpp)
target_link_libraries(test_tnc_server PRIVATE ultra_core)
add_test(NAME TNCServer COMMAND test_tnc_server)

if(SDL2_FOUND)
    add_executable(test_tnc_bridge
        test_tnc_bridge.cpp
        ../src/gui/audio_engine.cpp
    )
    target_include_directories(test_tnc_bridge PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )
    target_compile_definitions(test_tnc_bridge PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(test_tnc_bridge PRIVATE ultra_core)
    add_test(NAME TNCBridge COMMAND test_tnc_bridge)
else()
    message(STATUS "test_tnc_bridge disabled: SDL2 not found")
endif()

add_executable(test_connection_policy test_connection_policy.cpp)
target_link_libraries(test_connection_policy PRIVATE ultra_core)
add_test(NAME ConnectionPolicy COMMAND test_connection_policy)

add_executable(test_connection_adaptive test_connection_adaptive.cpp)
target_link_libraries(test_connection_adaptive PRIVATE ultra_core)
add_test(NAME ConnectionAdaptive COMMAND test_connection_adaptive)

add_executable(test_file_transfer_controller test_file_transfer_controller.cpp)
target_link_libraries(test_file_transfer_controller PRIVATE ultra_core)
add_test(NAME FileTransferController COMMAND test_file_transfer_controller)

add_executable(test_interleaver test_interleaver.cpp)
target_link_libraries(test_interleaver PRIVATE ultra_core)
add_test(NAME Interleaver COMMAND test_interleaver)

add_executable(test_codec_factory test_codec_factory.cpp)
target_link_libraries(test_codec_factory PRIVATE ultra_core)
add_test(NAME CodecFactory COMMAND test_codec_factory)

add_executable(test_soft_combine test_soft_combine.cpp)
target_link_libraries(test_soft_combine PRIVATE ultra_core)
add_test(NAME SoftCombine COMMAND test_soft_combine)

# Stop-and-wait ARQ remains available through the ARQ factory. Keep direct
# coverage so this lower-throughput compatibility path cannot silently regress.
add_executable(test_stop_wait_arq test_stop_wait_arq.cpp)
target_link_libraries(test_stop_wait_arq PRIVATE ultra_core)
add_test(NAME StopWaitARQ COMMAND test_stop_wait_arq)

# Multi-codeword LDPC test (file transfer reliability)
add_executable(test_multiblock_ldpc test_multiblock_ldpc.cpp)
target_link_libraries(test_multiblock_ldpc PRIVATE ultra_core)
add_test(NAME MultiblockLDPC COMMAND test_multiblock_ldpc)

# Selective Repeat ARQ tests
add_executable(test_selective_repeat test_selective_repeat.cpp)
target_link_libraries(test_selective_repeat PRIVATE ultra_core)
add_test(NAME SelectiveRepeatARQ COMMAND test_selective_repeat)

add_executable(test_selective_repeat_policy test_selective_repeat_policy.cpp)
target_link_libraries(test_selective_repeat_policy PRIVATE ultra_core)
add_test(NAME SelectiveRepeatPolicy COMMAND test_selective_repeat_policy)

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

add_executable(test_carrier_ldpc_interleaver test_carrier_ldpc_interleaver.cpp)
target_link_libraries(test_carrier_ldpc_interleaver PRIVATE ultra_core)
add_test(NAME CarrierLDPCInterleaver COMMAND test_carrier_ldpc_interleaver)

add_executable(test_ofdm_carrier_mask_plumbing test_ofdm_carrier_mask_plumbing.cpp)
target_link_libraries(test_ofdm_carrier_mask_plumbing PRIVATE ultra_core)
add_test(NAME OFDMCarrierMaskPlumbing COMMAND test_ofdm_carrier_mask_plumbing)

# Current waveform/rate policy and factory coverage
add_executable(test_waveform_policy test_waveform_policy.cpp)
target_link_libraries(test_waveform_policy PRIVATE ultra_core)
add_test(NAME WaveformPolicy COMMAND test_waveform_policy)

add_executable(test_ofdm_link_adaptation test_ofdm_link_adaptation.cpp)
target_link_libraries(test_ofdm_link_adaptation PRIVATE ultra_core)
add_test(NAME OFDMLinkAdaptation COMMAND test_ofdm_link_adaptation)

# Clean baseband loopback for maintained OFDM waveform wrappers. This verifies
# sync detection, training handoff, demodulation, and soft-bit extraction without
# involving ARQ or hardware audio.
add_executable(test_waveform_loopback test_waveform_loopback.cpp)
target_link_libraries(test_waveform_loopback PRIVATE ultra_core)
add_test(NAME WaveformLoopback COMMAND test_waveform_loopback)

add_executable(test_streaming_config
    test_streaming_config.cpp
    ../src/gui/modem/streaming_decoder.cpp
    ../src/gui/modem/streaming_encoder.cpp
)
target_include_directories(test_streaming_config PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(test_streaming_config PRIVATE ultra_core)
add_test(NAME StreamingConfig COMMAND test_streaming_config)

add_executable(test_streaming_buffer_policy test_streaming_buffer_policy.cpp)
target_include_directories(test_streaming_buffer_policy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(test_streaming_buffer_policy PRIVATE ultra_core)
add_test(NAME StreamingBufferPolicy COMMAND test_streaming_buffer_policy)

add_executable(test_streaming_decode_policy test_streaming_decode_policy.cpp)
target_include_directories(test_streaming_decode_policy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(test_streaming_decode_policy PRIVATE ultra_core)
add_test(NAME StreamingDecodePolicy COMMAND test_streaming_decode_policy)

add_executable(test_streaming_frame_policy test_streaming_frame_policy.cpp)
target_include_directories(test_streaming_frame_policy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(test_streaming_frame_policy PRIVATE ultra_core)
add_test(NAME StreamingFramePolicy COMMAND test_streaming_frame_policy)

add_executable(test_streaming_signal_policy test_streaming_signal_policy.cpp)
target_include_directories(test_streaming_signal_policy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(test_streaming_signal_policy PRIVATE ultra_core)
add_test(NAME StreamingSignalPolicy COMMAND test_streaming_signal_policy)

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
add_test(NAME Throughput COMMAND test_throughput)
