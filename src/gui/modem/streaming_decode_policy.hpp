#pragma once

#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/types.hpp"
#include <algorithm>
#include <cstddef>

namespace ultra {
namespace gui {
namespace streaming_decode_policy {

inline size_t estimateRobustOFDMControlSamples(size_t default_control_samples,
                                               Modulation data_mod,
                                               CodeRate data_rate,
                                               int carriers,
                                               int samples_per_symbol) {
    if (data_mod == Modulation::DQPSK && data_rate == CodeRate::R1_4) {
        return default_control_samples;
    }

    if (carriers <= 0 || samples_per_symbol <= 0) {
        return default_control_samples;
    }

    constexpr int kControlPilotSpacing = 10;
    constexpr int kLDPCCodewordBits = 648;
    const int bits_per_symbol = ofdm_link_adaptation::bitsPerOFDMSymbol(
        carriers, true, kControlPilotSpacing, Modulation::DQPSK);
    if (bits_per_symbol <= 0) {
        return default_control_samples;
    }

    const int data_symbols = (kLDPCCodewordBits + bits_per_symbol - 1) / bits_per_symbol;
    const size_t robust_samples = static_cast<size_t>(2 + data_symbols) *
                                  static_cast<size_t>(samples_per_symbol);
    return std::max(default_control_samples, robust_samples);
}

enum class DecodeSampleMode {
    PendingCodewords,
    ConnectedOFDMPeek,
    ConnectedOFDMBurst,
    ControlPeek,
};

struct DecodeSampleRequirement {
    size_t samples = 0;
    DecodeSampleMode mode = DecodeSampleMode::ControlPeek;
};

inline DecodeSampleRequirement selectDecodeSampleRequirement(int pending_total_cw,
                                                             bool is_ofdm,
                                                             bool connected,
                                                             bool burst_interleave_enabled,
                                                             bool burst_latched,
                                                             size_t pending_cw_samples,
                                                             size_t ofdm_control_samples,
                                                             size_t full_frame_samples,
                                                             size_t control_frame_samples) {
    if (pending_total_cw > 0) {
        return {pending_cw_samples, DecodeSampleMode::PendingCodewords};
    }

    if (is_ofdm && connected) {
        if (burst_interleave_enabled && burst_latched) {
            return {full_frame_samples, DecodeSampleMode::ConnectedOFDMBurst};
        }
        return {ofdm_control_samples, DecodeSampleMode::ConnectedOFDMPeek};
    }

    return {control_frame_samples, DecodeSampleMode::ControlPeek};
}

}  // namespace streaming_decode_policy
}  // namespace gui
}  // namespace ultra
