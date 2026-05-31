#pragma once

#include "streaming_control_profile.hpp"
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
    const auto control_profile =
        streaming_control_profile::profileForDataMode(data_mod);
    if (control_profile.modulation == data_mod && data_rate == control_profile.rate) {
        return default_control_samples;
    }

    if (carriers <= 0 || samples_per_symbol <= 0) {
        return default_control_samples;
    }

    constexpr int kLDPCCodewordBits = 648;
    const int control_pilot_spacing =
        streaming_control_profile::pilotSpacingForProfile(control_profile);
    const int bits_per_symbol = ofdm_link_adaptation::bitsPerOFDMSymbol(
        carriers, true, control_pilot_spacing, control_profile.modulation);
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

inline bool hasSubFixedFrameSoftBits(size_t soft_bits,
                                     int fixed_frame_codewords,
                                     size_t ldpc_block_bits) {
    if (fixed_frame_codewords <= 0 || ldpc_block_bits == 0) {
        return false;
    }

    const size_t fixed_frame_bits =
        static_cast<size_t>(fixed_frame_codewords) * ldpc_block_bits;
    return soft_bits >= ldpc_block_bits && soft_bits < fixed_frame_bits;
}

// 2026-05-29 channel-adaptive interleaver: burst_regime_active means "in a burst
// file-transfer" (interleave-INDEPENDENT — use_burst_interleave_ || burst_transport_rx_),
// NOT "byte interleave enabled". burst_latched means "the negated-LTS group-start
// marker was detected". A group-start data frame is sized as a FULL data frame (not a
// control peek) whenever a marker is latched in the burst regime — this holds whether
// or not the group's bytes are interleaved, because the encoder emits the marker for
// every group regardless. (Pre-decouple this branch was gated on the interleave flag,
// which silently mis-sized interleave-off groups as control peeks → they never decoded.)
inline DecodeSampleRequirement selectDecodeSampleRequirement(int pending_total_cw,
                                                             bool is_ofdm,
                                                             bool connected,
                                                             bool burst_regime_active,
                                                             bool burst_latched,
                                                             size_t pending_cw_samples,
                                                             size_t ofdm_control_samples,
                                                             size_t full_frame_samples,
                                                             size_t control_frame_samples) {
    if (pending_total_cw > 0) {
        return {pending_cw_samples, DecodeSampleMode::PendingCodewords};
    }

    if (is_ofdm && connected) {
        if (burst_regime_active && burst_latched) {
            return {full_frame_samples, DecodeSampleMode::ConnectedOFDMBurst};
        }
        return {ofdm_control_samples, DecodeSampleMode::ConnectedOFDMPeek};
    }

    return {control_frame_samples, DecodeSampleMode::ControlPeek};
}

}  // namespace streaming_decode_policy
}  // namespace gui
}  // namespace ultra
