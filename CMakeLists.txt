cmake_minimum_required(VERSION 3.16)
project(ProjectUltra VERSION 0.3.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release if the operator/dev didn't pick a build type. Debug builds
# of this modem run several times slower on the audio thread, which manifests
# as throughput regressions and per-frame retx on hardware. Release is the
# right default for the modem hot path; the CI matrix overrides explicitly
# (Debug + sanitizers) for the test stages that need it.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "CMAKE_BUILD_TYPE not set; defaulting to Release")
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Choose the type of build (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# MSVC-specific settings
if(MSVC)
    # Ensure math constants (M_PI, etc.) are available from <cmath> in all TUs.
    add_compile_definitions(_USE_MATH_DEFINES)
    # Enable parallel builds
    add_compile_options(/MP)
    # Suppress some common warnings
    add_compile_options(/wd4244)  # Conversion warnings (double to float)
    add_compile_options(/wd4267)  # size_t to int conversion
    # Treat source files as UTF-8
    add_compile_options(/utf-8)
endif()

# Build options
option(ULTRA_BUILD_TESTS "Build test suite" ON)
option(ULTRA_BUILD_TOOLS "Build command-line tools" ON)
option(ULTRA_BUILD_GUI "Build GUI application" ON)
option(ULTRA_BUILD_HARDWARE_TESTS "Register opt-in hardware smoke CTest target" OFF)

if(DEFINED ENV{ULTRA_HARDWARE_TESTS} AND "$ENV{ULTRA_HARDWARE_TESTS}" STREQUAL "1")
    set(ULTRA_BUILD_HARDWARE_TESTS ON CACHE BOOL
        "Register opt-in hardware smoke CTest target" FORCE)
endif()

if(MSVC)
    set(ULTRA_TOOL_TEST_WARNINGS /W4)
    set(ULTRA_TOOL_WARNING_SUPPRESSIONS /wd4100)
    set(ULTRA_TEST_ASSERT_OPTION /UNDEBUG)
else()
    set(ULTRA_TOOL_TEST_WARNINGS -Wall -Wextra)
    set(ULTRA_TOOL_WARNING_SUPPRESSIONS -Wno-unused-parameter -Wno-unused-private-field)
    set(ULTRA_TEST_ASSERT_OPTION -UNDEBUG)
endif()

function(ultra_enable_tool_test_warnings)
    foreach(src IN LISTS ARGN)
        set_property(SOURCE "${src}" APPEND PROPERTY COMPILE_OPTIONS
            ${ULTRA_TOOL_TEST_WARNINGS} ${ULTRA_TOOL_WARNING_SUPPRESSIONS})
    endforeach()
endfunction()

function(ultra_enable_test_warnings)
    foreach(src IN LISTS ARGN)
        set_property(SOURCE "${src}" APPEND PROPERTY COMPILE_OPTIONS
            ${ULTRA_TEST_ASSERT_OPTION} ${ULTRA_TOOL_TEST_WARNINGS})
    endforeach()
endfunction()

# Find dependencies
find_package(Threads REQUIRED)

execute_process(
    COMMAND git rev-parse --verify HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE ULTRA_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT ULTRA_GIT_COMMIT)
    set(ULTRA_GIT_COMMIT "unknown")
endif()
string(SUBSTRING "${ULTRA_GIT_COMMIT}" 0 12 ULTRA_GIT_COMMIT_SHORT)

execute_process(
    COMMAND git describe --tags --exact-match HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE ULTRA_RELEASE_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT ULTRA_RELEASE_TAG)
    set(ULTRA_RELEASE_TAG "")
endif()

execute_process(
    COMMAND git status --porcelain
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE ULTRA_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(ULTRA_GIT_STATUS STREQUAL "")
    set(ULTRA_GIT_DIRTY_BOOL false)
else()
    set(ULTRA_GIT_DIRTY_BOOL true)
endif()
set(ULTRA_BUILD_OS "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
string(TIMESTAMP ULTRA_BUILD_TIME_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ultra/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/include/ultra/version.hpp
    @ONLY
)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ultra/build_info.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/include/ultra/build_info.hpp
    @ONLY
)

# SDL2 detection — used by both ultra_gui (always required) and cli_simulator
# (optional, for hardware audio I/O in --role A|B mode). Detect once at the
# top level so both targets share the same libs.
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 sdl2)
    endif()
endif()
if(SDL2_FOUND)
    message(STATUS "Found SDL2: cli_simulator hardware audio enabled")
else()
    message(STATUS "SDL2 not found: cli_simulator --role A|B will be unavailable")
endif()

# Core library
add_library(ultra_core STATIC
    src/ofdm/modulator.cpp
    src/ofdm/ofdm_demodulator_setup.cpp
    src/ofdm/ofdm_symbol_demap.cpp
    src/ofdm/ofdm_stream_processor.cpp
    src/ofdm/ofdm_sync_adapter.cpp
    src/ofdm/ofdm_sync.cpp
    src/ofdm/channel_equalizer_baseband.cpp
    src/ofdm/channel_equalizer_lts.cpp
    src/ofdm/channel_equalizer_pilot.cpp
    src/ofdm/channel_equalizer_equalize.cpp
    src/fec/ldpc_encoder.cpp
    src/fec/ldpc_decoder.cpp
    src/fec/ldpc_codec.cpp
    src/fec/codec_factory.cpp
    src/fec/frame_interleaver.cpp
    src/fec/burst_interleaver.cpp
    src/fec/carrier_ldpc_interleaver.cpp
    src/fec/soft_combine.cpp
    src/dsp/fft.cpp
    src/dsp/filters.cpp
    src/dsp/resampler.cpp
    # Protocol layer (callsign addressing, connection management)
    src/protocol/frame_v2.cpp
    src/protocol/arq_interface.cpp
    src/protocol/arq.cpp
    src/protocol/selective_repeat_arq.cpp
    src/protocol/connection.cpp
    src/protocol/connection_handlers.cpp
    src/protocol/protocol_engine.cpp
    src/protocol/file_transfer.cpp
    src/protocol/compression.cpp
    # Local diagnostics and field-report bundle support
    src/diagnostics/audio_ring.cpp
    src/diagnostics/event_buffer.cpp
    src/diagnostics/report_bundle.cpp
    src/diagnostics/redaction.cpp
    src/diagnostics/session_summary.cpp
    src/diagnostics/diagnostics_recorder.cpp
    # VARA-compatible TNC protocol shell
    # (tnc_bridge.cpp is added conditionally below — depends on SDL2 via
    #  AudioEngine, so it cannot live in the unconditional ultra_core
    #  sources. Builds without SDL2 still get session + server.)
    src/tnc/tnc_session.cpp
    src/tnc/socket_compat.cpp
    src/tnc/tnc_server.cpp
    # Waveform abstraction layer
    src/waveform/mc_dpsk_waveform.cpp
    src/waveform/ofdm_cox_waveform.cpp
    src/waveform/ofdm_chirp_waveform.cpp
    src/waveform/waveform_factory.cpp
    # miniz compression library (public domain)
    thirdparty/miniz/miniz.c
    thirdparty/miniz/miniz_tdef.c
    thirdparty/miniz/miniz_tinfl.c
    thirdparty/miniz/miniz_zip.c
)

target_include_directories(ultra_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src  # For protocol headers
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty  # For miniz
)

target_link_libraries(ultra_core PUBLIC
    Threads::Threads
)

if(WIN32)
    target_link_libraries(ultra_core PRIVATE ws2_32)
    # Ensure <windows.h> never pulls in the legacy winsock1 headers project-
    # wide. tnc_bridge.cpp includes audio_engine.hpp (SDL.h → windows.h),
    # then later tnc_server.hpp (→ winsock2.h). Without this, both winsock
    # versions get included in the same TU and redefine sockaddr/fd_set/etc.
    target_compile_definitions(ultra_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(SDL2_FOUND)
    if(TARGET SDL2::SDL2)
        target_link_libraries(ultra_core PUBLIC SDL2::SDL2)
    else()
        target_include_directories(ultra_core PUBLIC ${SDL2_INCLUDE_DIRS})
        target_link_libraries(ultra_core PUBLIC ${SDL2_LIBRARIES})
    endif()
    # TNCBridge wraps AudioEngine (SDL2-only). Add to ultra_core only when
    # SDL2 is available; otherwise CI's Test/Sanitizer/Coverage steps that
    # build without SDL2 fail to compile the unconditional core.
    target_sources(ultra_core PRIVATE src/tnc/tnc_bridge.cpp)
endif()

# Shared GUI/modem runtime source lists. Keep these centralized so CLI tools,
# TNC, simulator, and GUI builds cannot silently drift apart.
set(ULTRA_STREAMING_DECODER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/modem/streaming_decoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/modem/streaming_sync_acquisition.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/modem/streaming_ofdm_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/modem/streaming_burst_interleave.cpp
)

set(ULTRA_STREAMING_MODEM_SOURCES
    ${ULTRA_STREAMING_DECODER_SOURCES}
    src/gui/modem/streaming_encoder.cpp
)

set(ULTRA_AUDIO_ENGINE_SOURCE
    src/gui/audio_engine.cpp
)

set(ULTRA_MODEM_ENGINE_SOURCES
    src/gui/modem/modem_engine.cpp
    src/gui/modem/modem_rx.cpp
    src/gui/modem/modem_rx_decode.cpp
    src/gui/modem/modem_mode.cpp
    src/gui/modem/modem_carrier_sense.cpp
    ${ULTRA_STREAMING_MODEM_SOURCES}
    src/gui/adaptive_mode.cpp
)

# Command-line modem tool
if(ULTRA_BUILD_TOOLS)
    add_library(ultra_tool_wav_io STATIC
        tools/io/wav_io.cpp
    )
    ultra_enable_tool_test_warnings(tools/io/wav_io.cpp)
    target_include_directories(ultra_tool_wav_io PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    target_link_libraries(ultra_tool_wav_io PUBLIC ultra_core)

    add_library(ultra_sim_station STATIC
        tools/sim/simulated_station.cpp
        ${ULTRA_STREAMING_MODEM_SOURCES}
    )
    ultra_enable_tool_test_warnings(tools/sim/simulated_station.cpp)
    target_include_directories(ultra_sim_station PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    target_link_libraries(ultra_sim_station PUBLIC ultra_core)

    add_executable(ultra
        src/main.cpp
        ${ULTRA_MODEM_ENGINE_SOURCES}
    )
    target_include_directories(ultra PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    target_link_libraries(ultra PRIVATE ultra_core)

    # CLI Simulator - Uses IWaveform + StreamingDecoder/Encoder directly (NOT ModemEngine)
    add_executable(cli_simulator
        tools/cli_simulator.cpp
    )
    ultra_enable_tool_test_warnings(tools/cli_simulator.cpp)
    target_include_directories(cli_simulator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    target_link_libraries(cli_simulator PRIVATE ultra_sim_station)

    add_executable(session_decode
        tools/session_decode.cpp
    )
    ultra_enable_tool_test_warnings(tools/session_decode.cpp)
    target_include_directories(session_decode PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    target_link_libraries(session_decode PRIVATE ultra_sim_station ultra_tool_wav_io)

    add_library(ultra_replay_lib STATIC
        src/replay/bundle_loader.cpp
        src/replay/event_timeline.cpp
        src/replay/replay_runner.cpp
        src/replay/divergence_report.cpp
        ${ULTRA_STREAMING_DECODER_SOURCES}
    )
    ultra_enable_tool_test_warnings(
        src/replay/bundle_loader.cpp
        src/replay/event_timeline.cpp
        src/replay/replay_runner.cpp
        src/replay/divergence_report.cpp
    )
    target_include_directories(ultra_replay_lib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    target_link_libraries(ultra_replay_lib PUBLIC ultra_tool_wav_io)

    add_executable(ultra_replay
        tools/ultra_replay.cpp
    )
    ultra_enable_tool_test_warnings(tools/ultra_replay.cpp)
    target_include_directories(ultra_replay PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    target_link_libraries(ultra_replay PRIVATE ultra_replay_lib)

    add_executable(ultra_report
        tools/ultra_report.cpp
    )
    ultra_enable_tool_test_warnings(tools/ultra_report.cpp)
    target_include_directories(ultra_report PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    target_link_libraries(ultra_report PRIVATE ultra_core)

    # Optional hardware-audio support (--role A|B). Reuses the GUI's
    # AudioEngine class; SDL2 is detected at top level above.
    if(SDL2_FOUND)
        target_sources(cli_simulator PRIVATE
            ${ULTRA_AUDIO_ENGINE_SOURCE}
        )
        # SDL_MAIN_HANDLED tells SDL.h not to rename our int main() to
        # SDL_main. Required for CLI tools on Windows; otherwise the linker
        # can't find main() and reports LNK2019 / LNK1120. ultra_gui keeps
        # SDL's main wrapping because it's a windowed SDL app.
        target_compile_definitions(cli_simulator PRIVATE
            ULTRA_HAVE_SDL2
            SDL_MAIN_HANDLED
        )
        if(TARGET SDL2::SDL2)
            target_link_libraries(cli_simulator PRIVATE SDL2::SDL2)
        else()
            target_include_directories(cli_simulator PRIVATE ${SDL2_INCLUDE_DIRS})
            target_link_libraries(cli_simulator PRIVATE ${SDL2_LIBRARIES})
        endif()
    endif()

    if(SDL2_FOUND)
        add_executable(ultra_tnc
            tools/ultra_tnc.cpp
            tools/ultra_tnc_config.cpp
            ${ULTRA_AUDIO_ENGINE_SOURCE}
            src/gui/serial_ptt.cpp
            ${ULTRA_STREAMING_MODEM_SOURCES}
        )
        target_include_directories(ultra_tnc PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tools
        )
        target_compile_definitions(ultra_tnc PRIVATE
            ULTRA_HAVE_SDL2
            SDL_MAIN_HANDLED
        )
        target_link_libraries(ultra_tnc PRIVATE ultra_core)
    else()
        message(STATUS "ultra_tnc disabled: SDL2 not found")
    endif()

    # Decode bench - generate + decode deterministic WAV fixtures so
    # AI agents and humans can A/B decoder changes against bit-identical
    # input. No SDL, no real-time pacing, runs as fast as possible.
    add_executable(decode_bench
        tools/decode_bench.cpp
        ${ULTRA_STREAMING_MODEM_SOURCES}
    )
    ultra_enable_tool_test_warnings(tools/decode_bench.cpp)
    target_include_directories(decode_bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    target_link_libraries(decode_bench PRIVATE ultra_tool_wav_io)

    # Threaded simulator - proper real-time audio simulation
    add_executable(threaded_simulator
        tools/threaded_simulator.cpp
        ${ULTRA_MODEM_ENGINE_SOURCES}
    )
    ultra_enable_tool_test_warnings(tools/threaded_simulator.cpp)
    target_include_directories(threaded_simulator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(threaded_simulator PRIVATE ultra_core)

    # Simple waveform test (clean TX/RX with consistent config)
    # Decoder-only by design; do not pull in the encoder runtime here.
    add_executable(test_waveform_simple
        tools/test_waveform_simple.cpp
        ${ULTRA_STREAMING_DECODER_SOURCES}
    )
    ultra_enable_tool_test_warnings(tools/test_waveform_simple.cpp)
    target_include_directories(test_waveform_simple PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    target_link_libraries(test_waveform_simple PRIVATE ultra_core)
endif()

# GUI Application
if(ULTRA_BUILD_GUI)
    # SDL2 detection is done at top level (shared with cli_simulator).

    find_package(OpenGL QUIET)

    if(SDL2_FOUND AND OPENGL_FOUND)
        # Dear ImGui sources
        set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui)

        add_executable(ultra_gui
            src/gui/main_gui.cpp
            src/gui/app.cpp
            ${ULTRA_AUDIO_ENGINE_SOURCE}
            src/gui/serial_ptt.cpp
            ${ULTRA_MODEM_ENGINE_SOURCES}
            src/gui/widgets/constellation.cpp
            src/gui/widgets/controls.cpp
            src/gui/widgets/status.cpp
            src/gui/widgets/settings.cpp
            src/gui/widgets/waterfall.cpp
            src/gui/widgets/file_browser.cpp
            # ImGui core
            ${IMGUI_DIR}/imgui.cpp
            ${IMGUI_DIR}/imgui_draw.cpp
            ${IMGUI_DIR}/imgui_tables.cpp
            ${IMGUI_DIR}/imgui_widgets.cpp
            # ImGui backends (OpenGL2 default + SDL renderer software fallback)
            ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
            ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp
            ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
        )

        target_include_directories(ultra_gui PRIVATE
            ${IMGUI_DIR}
            ${IMGUI_DIR}/backends
            ${SDL2_INCLUDE_DIRS}
        )

        target_link_libraries(ultra_gui PRIVATE
            ultra_core
            ${SDL2_LIBRARIES}
            ${OPENGL_LIBRARIES}
        )

        # Platform-specific settings
        if(APPLE)
            target_link_libraries(ultra_gui PRIVATE
                "-framework OpenGL"
                "-framework Cocoa"
                "-framework IOKit"
                "-framework CoreVideo"
            )
        elseif(WIN32)
            target_link_libraries(ultra_gui PRIVATE
                opengl32
                imm32
                dbghelp
            )
        elseif(UNIX)
            target_link_libraries(ultra_gui PRIVATE dl)
        endif()

        message(STATUS "GUI: Enabled (SDL2 + OpenGL 2.1)")
    else()
        message(STATUS "GUI: Disabled (SDL2 or OpenGL not found)")
        message(STATUS "  Install SDL2: brew install sdl2 (macOS) or apt install libsdl2-dev (Linux)")
    endif()
endif()

# Tests
if(ULTRA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
