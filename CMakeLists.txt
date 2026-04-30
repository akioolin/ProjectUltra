cmake_minimum_required(VERSION 3.16)
project(ProjectUltra VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
option(ULTRA_USE_FFTW "Use FFTW3 for FFT (faster)" ON)

# Find dependencies
find_package(Threads REQUIRED)

if(ULTRA_USE_FFTW)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        # Use fftw3f (single-precision float) since code uses fftwf_* functions
        pkg_check_modules(FFTW3 IMPORTED_TARGET fftw3f)
        if(FFTW3_FOUND)
            add_compile_definitions(ULTRA_HAS_FFTW)
            message(STATUS "Found FFTW3 (float): ${FFTW3_LIBRARIES}")
        else()
            message(STATUS "FFTW3 float library not found - using built-in FFT")
        endif()
    else()
        message(STATUS "PkgConfig not found - FFTW3 detection skipped (will use built-in FFT)")
    endif()
endif()

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
    src/ofdm/demodulator.cpp
    src/ofdm/ofdm_sync.cpp
    src/ofdm/channel_equalizer.cpp
    src/fec/ldpc_encoder.cpp
    src/fec/ldpc_decoder.cpp
    src/fec/ldpc_codec.cpp
    src/fec/codec_factory.cpp
    src/fec/frame_interleaver.cpp
    src/fec/burst_interleaver.cpp
    src/framing/frame_builder.cpp
    src/arq/arq_controller.cpp
    src/dsp/fft.cpp
    src/dsp/filters.cpp
    src/dsp/resampler.cpp
    src/modem/modem.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src  # For protocol headers
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty  # For miniz
)

target_link_libraries(ultra_core PUBLIC
    Threads::Threads
)

if(FFTW3_FOUND)
    target_link_libraries(ultra_core PUBLIC PkgConfig::FFTW3)
endif()

# Command-line modem tool
if(ULTRA_BUILD_TOOLS)
    add_executable(ultra
        src/main.cpp
        src/gui/modem/modem_engine.cpp
        src/gui/modem/modem_rx.cpp
        src/gui/modem/modem_rx_decode.cpp
        src/gui/modem/modem_mode.cpp
        src/gui/modem/modem_carrier_sense.cpp
        src/gui/modem/streaming_decoder.cpp
        src/gui/modem/streaming_encoder.cpp
        src/gui/adaptive_mode.cpp
    )
    target_include_directories(ultra PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(ultra PRIVATE ultra_core)

    # CLI Simulator - Uses IWaveform + StreamingDecoder/Encoder directly (NOT ModemEngine)
    add_executable(cli_simulator
        tools/cli_simulator.cpp
        src/gui/modem/streaming_decoder.cpp
        src/gui/modem/streaming_encoder.cpp
    )
    target_include_directories(cli_simulator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(cli_simulator PRIVATE ultra_core)

    # Optional hardware-audio support (--role A|B). Reuses the GUI's
    # AudioEngine class; SDL2 is detected at top level above.
    if(SDL2_FOUND)
        target_sources(cli_simulator PRIVATE
            src/gui/audio_engine.cpp
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

    # Threaded simulator - proper real-time audio simulation
    add_executable(threaded_simulator
        tools/threaded_simulator.cpp
        src/gui/modem/modem_engine.cpp
        src/gui/modem/modem_rx.cpp
        src/gui/modem/modem_rx_decode.cpp
        src/gui/modem/modem_mode.cpp
        src/gui/modem/modem_carrier_sense.cpp
        src/gui/modem/streaming_decoder.cpp
        src/gui/modem/streaming_encoder.cpp
        src/gui/adaptive_mode.cpp
    )
    target_include_directories(threaded_simulator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(threaded_simulator PRIVATE ultra_core)

    # Acquisition profiling
    add_executable(profile_acquisition
        tools/profile_acquisition.cpp
        src/gui/modem/modem_engine.cpp
        src/gui/modem/modem_rx.cpp
        src/gui/modem/modem_rx_decode.cpp
        src/gui/modem/modem_mode.cpp
        src/gui/modem/modem_carrier_sense.cpp
        src/gui/modem/streaming_decoder.cpp
        src/gui/modem/streaming_encoder.cpp
        src/gui/adaptive_mode.cpp
    )
    target_include_directories(profile_acquisition PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(profile_acquisition PRIVATE ultra_core)

    # Simple waveform test (clean TX/RX with consistent config)
    add_executable(test_waveform_simple
        tools/test_waveform_simple.cpp
        src/gui/modem/streaming_decoder.cpp
    )
    target_include_directories(test_waveform_simple PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
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
            src/gui/audio_engine.cpp
            src/gui/serial_ptt.cpp
            src/gui/modem/modem_engine.cpp
            src/gui/modem/modem_rx.cpp
            src/gui/modem/modem_rx_decode.cpp
            src/gui/modem/modem_mode.cpp
            src/gui/modem/modem_carrier_sense.cpp
        src/gui/modem/streaming_decoder.cpp
            src/gui/modem/streaming_encoder.cpp
            src/gui/adaptive_mode.cpp
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
