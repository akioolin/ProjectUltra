// OFDM sync adapter and standalone channel estimator

#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>
#include <memory>
#include "ultra/ofdm.hpp"
#include "ultra/dsp.hpp"
#include "ultra/logging.hpp"
#include "demodulator_impl.hpp"
#include "demodulator_constants.hpp"

namespace ultra {

using namespace demod_constants;

bool OFDMDemodulator::searchForSync(SampleSpan samples, size_t& out_position, float& out_cfo_hz, float threshold) {
    // Search for Schmidl-Cox sync in samples WITHOUT changing internal state
    // This is used by IWaveform::detectSync() to find preamble position

    if (samples.size() < MIN_SEARCH_SAMPLES) {
        return false;
    }

    // Temporarily store samples for correlation (don't modify rx_buffer)
    std::vector<float> search_buffer(samples.begin(), samples.end());

    // Preamble size constants
    size_t preamble_symbol_len = impl_->config.fft_size + impl_->config.getCyclicPrefix();
    size_t preamble_total_len = preamble_symbol_len * 6;
    size_t correlation_window = preamble_symbol_len * 2;

    if (search_buffer.size() < preamble_total_len + correlation_window) {
        return false;
    }

    // Save original rx_buffer and restore after search
    std::vector<float> saved_buffer = std::move(impl_->rx_buffer);
    impl_->rx_buffer = std::move(search_buffer);

    // Search for preamble
    bool found_sync = false;
    size_t sync_offset = 0;
    size_t refined_lts_offset = 0;
    float sync_cfo = 0.0f;

    size_t search_end = impl_->rx_buffer.size() - preamble_total_len - correlation_window;

    // Use larger step for faster search (64 samples = ~1.3ms at 48kHz)
    // This is a quick search to find candidates, not fine timing
    constexpr size_t QUICK_SEARCH_STEP = 64;

    for (size_t i = 0; i < search_end; i += QUICK_SEARCH_STEP) {
        if (!impl_->hasMinimumEnergy(i, correlation_window)) {
            i += correlation_window / 2 - QUICK_SEARCH_STEP;
            continue;
        }

        float corr = impl_->measureCorrelation(i);

        if (corr > threshold) {
            // Search for plateau
            size_t plateau_count = 0;
            float peak_corr = corr;
            size_t peak_pos = i;

            for (size_t j = 0; j <= PLATEAU_SEARCH_WINDOW && i + j + preamble_total_len < impl_->rx_buffer.size(); j += 8) {
                float ref_corr = impl_->measureCorrelation(i + j);
                if (ref_corr >= PLATEAU_THRESHOLD) {
                    plateau_count++;
                }
                if (ref_corr > peak_corr) {
                    peak_corr = ref_corr;
                    peak_pos = i + j;
                }
            }

            if (plateau_count >= MIN_PLATEAU_SAMPLES) {
                // Verify with LTS
                size_t refined_lts = impl_->refineLTSTiming(peak_pos);
                if (refined_lts != SIZE_MAX) {
                    found_sync = true;
                    sync_offset = peak_pos;
                    refined_lts_offset = refined_lts;
                    sync_cfo = impl_->estimateCoarseCFO(peak_pos);

                    LOG_SYNC(INFO, "searchForSync: found at %zu (LTS=%zu), corr=%.3f, CFO=%.1f Hz",
                             sync_offset, refined_lts_offset, peak_corr, sync_cfo);
                    break;
                }
            }
        }
    }

    if (found_sync) {
        out_cfo_hz = sync_cfo;

        // processPresynced expects samples starting at the FIRST LTS. The LTS
        // template search can peak on either repeated LTS symbol; choose the
        // earlier position only when the preceding two symbols are themselves
        // an LTS pair. Otherwise, subtracting one symbol lands on the final STS
        // and shifts all payload bits by one OFDM symbol.
        out_position = refined_lts_offset;
        if (refined_lts_offset >= preamble_symbol_len) {
            const size_t previous_symbol = refined_lts_offset - preamble_symbol_len;
            const float pair_corr = impl_->measureAnalyticCorrelation(previous_symbol);
            if (pair_corr > 0.85f) {
                out_position = previous_symbol;
            }
        }
    }

    // Restore original buffer (do NOT modify state)
    impl_->rx_buffer = std::move(saved_buffer);

    return found_sync;
}

// =============================================================================
// CHANNEL ESTIMATOR (standalone class)
// =============================================================================

struct ChannelEstimator::Impl {
    ModemConfig config;
    std::vector<Complex> h_estimate;
    ChannelQuality quality;

    Impl(const ModemConfig& cfg)
        : config(cfg)
        , h_estimate(cfg.fft_size, Complex(1, 0))
    {}
};

ChannelEstimator::ChannelEstimator(const ModemConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

ChannelEstimator::~ChannelEstimator() = default;

void ChannelEstimator::updateFromPilots(const Symbol& received, const Symbol& expected) {
    for (size_t i = 0; i < received.size() && i < expected.size(); ++i) {
        if (std::norm(expected[i]) > 1e-10f) {
            Complex h = received[i] / expected[i];
            impl_->h_estimate[i] = 0.5f * h + 0.5f * impl_->h_estimate[i];
        }
    }
}

Symbol ChannelEstimator::equalize(const Symbol& received) {
    Symbol output(received.size());
    for (size_t i = 0; i < received.size(); ++i) {
        if (std::norm(impl_->h_estimate[i]) > 1e-10f) {
            output[i] = received[i] / impl_->h_estimate[i];
        } else {
            output[i] = received[i];
        }
    }
    return output;
}

ChannelQuality ChannelEstimator::getQuality() const {
    return impl_->quality;
}

void ChannelEstimator::interpolate() {
    // Simple linear interpolation
}


} // namespace ultra
