#pragma once

// ModemEngine RX Constants
// Centralized constants for readability and maintainability

#include <cstddef>

namespace ultra {
namespace gui {
namespace rx_constants {

// Buffer thresholds
constexpr size_t MIN_SAMPLES_FOR_OFDM_SYNC = 8000;   // ~167ms @ 48kHz
constexpr size_t MIN_SAMPLES_FOR_DPSK = 4000;        // ~83ms @ 48kHz
// MIN_SAMPLES_FOR_ACQUISITION must account for:
// - Full dual chirp: 57600 samples
// - Small margin for post-chirp energy check: ~5000 samples
// Total: ~63000 minimum, use 65000 for safety
// Note: For DPSK frames, we check chirp at buffer start, so no position margin needed
constexpr size_t MIN_SAMPLES_FOR_ACQUISITION = 65000; // ~1.35s @ 48kHz
constexpr size_t MAX_BUFFER_BEFORE_TRIM = 48000;     // 1 second @ 48kHz
constexpr size_t BUFFER_TRIM_TARGET = 24000;         // Keep 500ms after trim

// Thread timing
constexpr int ACQUISITION_POLL_MS = 50;
constexpr int DECODE_POLL_MS = 10;
constexpr int WAIT_FOR_SAMPLES_MS = 10;

// Audio injection
constexpr size_t INJECT_CHUNK_SIZE = 4800;           // 100ms chunks
constexpr int INJECT_CHUNK_DELAY_MS = 10;
constexpr int INJECT_FINAL_DELAY_MS = 500;

// Carrier sense
constexpr size_t ENERGY_WINDOW_SAMPLES = 480;        // 10ms @ 48kHz

} // namespace rx_constants
} // namespace gui
} // namespace ultra
