// StreamingDecoder module

#include "streaming_decoder.hpp"
#include "adaptive_reanchor_policy.hpp"
#include "streaming_buffer_policy.hpp"
#include "streaming_decode_policy.hpp"
#include "streaming_decoder_debug.hpp"
#include "sync/frame_arrival_policy.hpp"
#include "streaming_frame_policy.hpp"
#include "sync/signal_policy.hpp"
#include "gui/startup_trace.hpp"
#include "waveform/mc_dpsk_waveform.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
#include "fec/frame_interleaver.hpp"
#include "fec/burst_interleaver.hpp"
#include "ultra/fec.hpp"
#include "fec/ldpc_codec.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/timing_profiler.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <stdexcept>

namespace ultra {
namespace gui {

namespace v2 = protocol::v2;
namespace buffer_policy = streaming_buffer_policy;
namespace decode_policy = streaming_decode_policy;
namespace arrival_policy = ::ultra::sync::frame_arrival_policy;
namespace frame_policy = streaming_frame_policy;
namespace signal_policy = ::ultra::sync::signal_policy;

namespace {

bool qam16GenieTimingCfoEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ULTRA_QAM16_GENIE_TIMING_CFO");
        return value && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
    }();
    return enabled;
}

}  // namespace

float StreamingDecoder::applyCFOPreCorrection(std::vector<float>& samples, float cfo_hz,
                                              size_t absolute_start_sample) {
    // The Hilbert pre-corrector operates on a finite frame whose first samples
    // are the OFDM LTS. At sub-Hz offsets the real CFO over one frame is
    // negligible, while the finite analytic-filter edge transient can imprint a
    // false per-carrier magnitude ripple on the LTS/pilots. Leave those small
    // offsets for the demodulator's complex baseband correction path.
    constexpr float kMinCFOForHilbertPreCorrectionHz = 0.75f;
    if (std::abs(cfo_hz) < kMinCFOForHilbertPreCorrectionHz || samples.empty()) {
        pre_correction_cfo_ = 0.0f;
        return 0.0f;  // Skip if negligible
    }

    // Convert to analytic signal (real + j*Hilbert) for proper frequency shift
    HilbertTransform hilbert(65);
    auto analytic = hilbert.process(SampleSpan(samples.data(), samples.size()));

    // Apply CFO correction: multiply analytic signal by exp(-j*2π*CFO*t)
    // Phase formula matches toBaseband(): phase_inc = -2π × CFO / fs per sample
    const float fs = 48000.0f;
    float phase_inc = -2.0f * static_cast<float>(M_PI) * cfo_hz / fs;
    float phase = -2.0f * static_cast<float>(M_PI) * cfo_hz *
                  static_cast<float>(absolute_start_sample) / fs;
    // Wrap to [-π, π]
    phase = std::fmod(phase, 2.0f * static_cast<float>(M_PI));
    if (phase > static_cast<float>(M_PI)) phase -= 2.0f * static_cast<float>(M_PI);
    if (phase < -static_cast<float>(M_PI)) phase += 2.0f * static_cast<float>(M_PI);

    // Apply rotation and take real part
    size_t len = std::min(samples.size(), analytic.size());
    for (size_t i = 0; i < len; i++) {
        Complex correction(std::cos(phase), std::sin(phase));
        samples[i] = (analytic[i] * correction).real();
        phase += phase_inc;
        if (phase > static_cast<float>(M_PI)) phase -= 2.0f * static_cast<float>(M_PI);
        else if (phase < -static_cast<float>(M_PI)) phase += 2.0f * static_cast<float>(M_PI);
    }

    pre_correction_cfo_ = cfo_hz;
    LOG_MODEM(DEBUG, "[%s] CFO pre-correction: %.2f Hz, abs_start=%zu, %zu samples",
              log_prefix_.c_str(), cfo_hz, absolute_start_sample, len);
    return cfo_hz;
}

void StreamingDecoder::populateDecodeMetrics(DecodeResult& result, bool is_ofdm,
                                             float residual_cfo_hz) const {
    result.sync_correlation = sync_correlation_;
    result.sync_quality_db = result.snr_db;
    const auto idle_snr = idle_noise_snr_estimator_.snapshot();
    result.has_idle_in_band_snr_db =
        idle_snr.valid && std::isfinite(idle_snr.idle_in_band_snr_db);
    result.idle_in_band_snr_db = idle_snr.idle_in_band_snr_db;

    if (is_ofdm && waveform_) {
        result.ofdm_internal_snr_db = waveform_->estimatedSNR();
        result.has_ofdm_broadband_snr_db = waveform_->hasLastOFDMBroadbandSNREstimate();
        result.ofdm_broadband_snr_db = waveform_->getLastOFDMBroadbandSNREstimate();
        result.lts_fading_index = waveform_->getFadingIndex();
        result.lts_timing_offset_samples = waveform_->getLastTimingOffsetSamples();
        result.pilot_frequency_cv = waveform_->getLastPilotFrequencyCV();
        result.pilot_temporal_cv = waveform_->getLastPilotTemporalCV();
        result.pilot_symbol_mean_cv = waveform_->getLastPilotSymbolMeanCV();
        result.lts_residual_cfo_hz = waveform_->getLastLTSResidualCFOHz();
        result.snr_source = SNRSource::SYNC_QUALITY;
        if (result.has_ofdm_broadband_snr_db) {
            result.snr_db = result.ofdm_broadband_snr_db;
            result.snr_source = SNRSource::OFDM_BROADBAND;
            last_ofdm_broadband_snr_db_valid_.store(true);
            last_ofdm_broadband_snr_db_.store(result.ofdm_broadband_snr_db);
        }
        LOG_MODEM(DEBUG, "[%s] OFDM quality: sync_quality=%.1f dB "
                  "ofdm_broadband=%s%.1f dB ofdm_internal=%.1f dB "
                  "idle_in_band=%s%.1f dB routed_snr=%.1f dB (%s) fading=%.3f",
                  log_prefix_.c_str(), result.sync_quality_db,
                  result.has_ofdm_broadband_snr_db ? "" : "unavailable/",
                  result.ofdm_broadband_snr_db, result.ofdm_internal_snr_db,
                  result.has_idle_in_band_snr_db ? "" : "unavailable/",
                  result.idle_in_band_snr_db, result.snr_db,
                  snrSourceToString(result.snr_source),
                  result.lts_fading_index);
    } else {
        result.snr_source = SNRSource::SYNC_QUALITY;
        const auto* mc_waveform = dynamic_cast<const MCDPSKWaveform*>(waveform_.get());
        if (connected_ && mode_ == protocol::WaveformMode::MC_DPSK &&
            mc_waveform && mc_waveform->hasEstimatedSNR() &&
            std::isfinite(mc_waveform->estimatedSNR())) {
            result.snr_db = mc_waveform->estimatedSNR();
            result.snr_source = SNRSource::MCDPSK_IN_BAND;
        } else if (result.has_idle_in_band_snr_db) {
            result.snr_db = result.idle_in_band_snr_db;
            result.snr_source = SNRSource::IDLE_IN_BAND;
        }
        result.ofdm_internal_snr_db = 0.0f;
        result.has_ofdm_broadband_snr_db = false;
        result.ofdm_broadband_snr_db = 0.0f;
        result.lts_fading_index = 0.0f;
        result.lts_residual_cfo_hz = residual_cfo_hz;
        last_ofdm_broadband_snr_db_valid_.store(false);
        last_ofdm_broadband_snr_db_.store(0.0f);
        LOG_MODEM(DEBUG, "[%s] non-OFDM quality: sync_quality=%.1f dB "
                  "idle_in_band=%s%.1f dB routed_snr=%.1f dB (%s) windows=%zu",
                  log_prefix_.c_str(), result.sync_quality_db,
                  result.has_idle_in_band_snr_db ? "" : "unavailable/",
                  result.idle_in_band_snr_db, result.snr_db,
                  snrSourceToString(result.snr_source), idle_snr.windows_observed);
    }
}

size_t StreamingDecoder::ringPosToAbsoluteLocked(size_t ring_pos) const {
    if (total_fed_ < buffer_capacity_samples_) {
        return ring_pos;
    }

    const size_t oldest_abs = total_fed_ - buffer_capacity_samples_;
    const size_t oldest_pos = write_pos_;
    const size_t offset = (ring_pos >= oldest_pos)
        ? (ring_pos - oldest_pos)
        : (buffer_capacity_samples_ - oldest_pos + ring_pos);
    return oldest_abs + offset;
}

size_t StreamingDecoder::absoluteToRingLocked(size_t abs_pos) const {
    if (total_fed_ < buffer_capacity_samples_) {
        return wrapRingIndexLocked(std::min(abs_pos, total_fed_));
    }

    const size_t oldest_abs = total_fed_ - buffer_capacity_samples_;
    abs_pos = std::clamp(abs_pos, oldest_abs, total_fed_);
    return wrapRingIndexLocked(write_pos_ + (abs_pos - oldest_abs));
}

void StreamingDecoder::setSearchFloorLocked(size_t abs_pos) {
    const size_t oldest_abs = (total_fed_ > buffer_capacity_samples_)
        ? (total_fed_ - buffer_capacity_samples_)
        : 0;
    search_floor_abs_ = std::clamp(abs_pos, oldest_abs, total_fed_);
    search_floor_abs_valid_ = true;
}

void StreamingDecoder::searchForSync() {
    if (!waveform_) return;

    // Save generation counter - if reset() is called during our search,
    // we'll detect it and discard our results
    uint32_t gen_at_start = reset_generation_.load();

    // Get preamble size from waveform
    size_t preamble = static_cast<size_t>(waveform_->getPreambleSamples());

    // Search buffer sizing depends on sync mode:
    // - Chirp sync (disconnected): needs ~120k samples for dual chirp correlation
    // - Light sync (connected, LTS): needs only ~24k samples (LTS is ~1024 samples)
    // Using a smaller buffer when connected cuts per-hop latency from ~2.8s to <1s
    constexpr size_t CHIRP_MAX_SEARCH = 120000;   // ~2.5s for dual chirp detection
    constexpr size_t LIGHT_SEARCH_SIZE = 9600;    // ~0.20s for connected LTS-only detection

    size_t chirp_min_search = std::min(preamble + 65000, CHIRP_MAX_SEARCH);
    bool connected_data_preamble = connected_ && waveform_->supportsDataPreamble();
    bool use_full_ofdm_anchor_search =
        connected_data_preamble && sync_controller_.expect_full_ofdm_anchor_ &&
        mode_ == protocol::WaveformMode::OFDM_CHIRP;
    bool use_light_search = connected_data_preamble && !use_full_ofdm_anchor_search;
    bool used_full_anchor_fallback = false;
    const bool use_short_reanchor_search =
        use_light_search &&
        adaptive_short_data_preamble_ &&
        mode_ == protocol::WaveformMode::OFDM_CHIRP;
    const float short_reanchor_chirp_ms =
        use_short_reanchor_search
            ? adaptive_reanchor_policy::shortReanchorChirpDurationMs()
            : 0.0f;
    const size_t light_data_preamble_samples =
        (use_short_reanchor_search && waveform_)
            ? static_cast<size_t>(std::max(0, waveform_->getDataPreambleSamples()))
            : 0;
    const size_t short_data_preamble_samples =
        (use_short_reanchor_search && waveform_)
            ? static_cast<size_t>(std::max(
                  0, waveform_->getShortDataPreambleSamples(short_reanchor_chirp_ms)))
            : 0;
    const size_t short_reanchor_lead_samples =
        short_data_preamble_samples > light_data_preamble_samples
            ? short_data_preamble_samples - light_data_preamble_samples
            : 0;
    size_t min_search = use_light_search
        ? (use_short_reanchor_search
            ? std::max(LIGHT_SEARCH_SIZE,
                       short_data_preamble_samples + LIGHT_SEARCH_SIZE)
            : LIGHT_SEARCH_SIZE)
        : chirp_min_search;
    const size_t data_symbol_samples =
        (use_light_search && waveform_)
            ? static_cast<size_t>(std::max(1, waveform_->getSamplesPerSymbol()))
            : 0;

    std::vector<float> search_buffer;
    size_t search_start = 0;
    bool used_warm_timed_window = false;
    bool used_warm_narrow_window = false;
    size_t warm_narrow_end_abs = 0;
    size_t warm_narrow_candidate_span_samples = 0;

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        float audio_sec = total_fed_ / 48000.0f;

        // Initialize correlation_pos_ if needed
        if (correlation_pos_ == 0 && total_fed_ > 0) {
            if (total_fed_ < buffer_capacity_samples_) {
                correlation_pos_ = 0;
            } else {
                correlation_pos_ = write_pos_;
            }
        }

        const size_t oldest_abs = (total_fed_ > buffer_capacity_samples_)
            ? (total_fed_ - buffer_capacity_samples_)
            : 0;
        const size_t correlation_abs = ringPosToAbsoluteLocked(correlation_pos_);
        // §16.8 step 2 v2: when warm-handoff is on AND we're in
        // post-BURST_HEADER warm state, the data group uses pure light
        // LTS (no short-chirp re-anchor lead). The legacy code shifts
        // the search expectation BACK by short_reanchor_lead_samples
        // because the adaptive short re-anchor preamble starts with a
        // short chirp at -150ms; in our warm-handoff path the LTS is
        // exactly at sync_controller_.next_expected_frame_sample_ with no lead. The
        // shift-back put correlation_pos_ PAST the search window's end
        // → !current_step_intersects → warm_plan.active stayed false.
        const char* s16_env_w =
            std::getenv("ULTRA_S16_WARM_HANDOFF");
        const bool s16_warm_handoff_w =
            s16_env_w && std::atoi(s16_env_w) != 0;
        const bool s16_skip_short_lead =
            s16_warm_handoff_w &&
            sync_controller_.warm_sync_phase_ ==
                arrival_policy::WarmSyncPhase::WARM &&
            !sync_controller_.expect_full_ofdm_anchor_;
        const size_t expected_sync_search_sample =
            (!s16_skip_short_lead &&
             use_short_reanchor_search &&
             sync_controller_.next_expected_frame_sample_valid_ &&
             sync_controller_.next_expected_frame_sample_ > short_reanchor_lead_samples)
                ? sync_controller_.next_expected_frame_sample_ - short_reanchor_lead_samples
                : sync_controller_.next_expected_frame_sample_;

        auto warm_plan = arrival_policy::planWarmSearchWindow(
            use_light_search,
            sync_controller_.warm_sync_active_,
            sync_controller_.next_expected_frame_sample_valid_,
            expected_sync_search_sample,
            sync_controller_.frame_arrival_confidence_,
            sync_controller_.consecutive_sync_misses_,
            sync_controller_.warm_sync_phase_,
            total_fed_,
            oldest_abs,
            search_floor_abs_valid_,
            search_floor_abs_,
            correlation_abs,
            data_symbol_samples,
            CORRELATION_STEP);

        if (use_short_reanchor_search &&
            short_reanchor_lead_samples > 0 &&
            (warm_plan.active || warm_plan.wait_for_more_samples)) {
            warm_plan.search_size_samples += short_reanchor_lead_samples;
            warm_plan.search_end_abs += short_reanchor_lead_samples;
        }

        if (warm_plan.wait_for_more_samples) {
            static int warm_wait_count = 0;
            if (++warm_wait_count % 50 == 1) {
                LOG_MODEM(INFO,
                          "[%s] warm-sync: wait for expected window, need_abs=%zu total=%zu expected=%zu",
                          log_prefix_.c_str(), warm_plan.search_end_abs, total_fed_,
                          sync_controller_.next_expected_frame_sample_);
            }
            return;
        }

        if (use_short_reanchor_search &&
            warm_plan.active &&
            total_fed_ < warm_plan.search_end_abs) {
            static int warm_short_wait_count = 0;
            if (++warm_short_wait_count % 50 == 1) {
                LOG_MODEM(INFO,
                          "[%s] warm-sync: wait for short re-anchor window, need_abs=%zu total=%zu expected_training=%zu",
                          log_prefix_.c_str(), warm_plan.search_end_abs, total_fed_,
                          sync_controller_.next_expected_frame_sample_);
            }
            return;
        }

        if (warm_plan.active) {
            used_warm_timed_window = true;
            used_warm_narrow_window = warm_plan.lower_threshold;
            warm_narrow_end_abs = warm_plan.search_end_abs;
            warm_narrow_candidate_span_samples = warm_plan.candidate_span_samples;
            min_search = warm_plan.search_size_samples;
            search_start = absoluteToRingLocked(warm_plan.search_start_abs);
        }
        // §16.8 step 2 v2 diagnostic: log warm-window decision once per
        // search invocation. ULTRA_S16_TRACE_WARM_WINDOW=1 enables.
        {
            const char* trace = std::getenv("ULTRA_S16_TRACE_WARM_WINDOW");
            if (trace && std::atoi(trace) != 0) {
                LOG_MODEM(INFO,
                    "[%s] s16-warm-window: active=%d wait=%d lower_threshold=%d "
                    "phase=%s active_flag=%d has_pred=%d expected=%llu "
                    "conf=%.2f misses=%d use_light=%d total=%llu "
                    "search[%llu..%llu] span=%zu",
                    log_prefix_.c_str(),
                    warm_plan.active ? 1 : 0,
                    warm_plan.wait_for_more_samples ? 1 : 0,
                    warm_plan.lower_threshold ? 1 : 0,
                    arrival_policy::warmSyncPhaseName(sync_controller_.warm_sync_phase_),
                    sync_controller_.warm_sync_active_ ? 1 : 0,
                    sync_controller_.next_expected_frame_sample_valid_ ? 1 : 0,
                    static_cast<unsigned long long>(sync_controller_.next_expected_frame_sample_),
                    sync_controller_.frame_arrival_confidence_,
                    sync_controller_.consecutive_sync_misses_,
                    use_light_search ? 1 : 0,
                    static_cast<unsigned long long>(total_fed_),
                    static_cast<unsigned long long>(warm_plan.search_start_abs),
                    static_cast<unsigned long long>(warm_plan.search_end_abs),
                    warm_plan.candidate_span_samples);
            }
        }

        // Need minimum samples before we can search
        if (total_fed_ < min_search) {
            static int skip_count = 0;
            if (++skip_count % 50 == 1)
                LOG_MODEM(INFO, "[%s] searchForSync: SKIP not enough samples, total=%.2fs, need=%.2fs",
                          log_prefix_.c_str(), audio_sec, min_search / 48000.0f);
            return;
        }

        // Calculate unsearched data available
        size_t unsearched;
        if (write_pos_ >= correlation_pos_) {
            unsearched = write_pos_ - correlation_pos_;
        } else {
            unsearched = buffer_capacity_samples_ - correlation_pos_ + write_pos_;
        }

        // Need at least min_search unsearched samples
        if (!used_warm_timed_window && unsearched < min_search) {
            static int skip_count2 = 0;
            if (++skip_count2 % 50 == 1)
                LOG_MODEM(INFO, "[%s] searchForSync: SKIP unsearched=%zu < min=%zu, total=%.2fs, corr_pos=%zu",
                          log_prefix_.c_str(), unsearched, min_search, audio_sec, correlation_pos_);
            return;
        }

        // Quick RMS check for signal presence. For disconnected MC-DPSK chirps,
        // use the strongest 20ms slice across the next 100ms search step. A
        // Watterson notch can erase one narrow chirp segment while the rest of
        // the sweep remains detectable; the correlator is the real detector,
        // this gate only keeps silence from burning CPU.
        const size_t rms_probe_pos = used_warm_timed_window ? search_start : correlation_pos_;
        float rms = 0.0f;
        for (size_t i = 0; i < 1000; i++) {
            float s = buffer_[wrapRingIndexLocked(rms_probe_pos + i)];
            rms += s * s;
        }
        rms = std::sqrt(rms / 1000.0f);

        const bool disconnected_mc_dpsk =
            !connected_ && mode_ == protocol::WaveformMode::MC_DPSK;
        if (disconnected_mc_dpsk) {
            float max_slice_rms = rms;
            constexpr size_t RMS_SLICE_SAMPLES = 1000;
            for (size_t off = RMS_SLICE_SAMPLES;
                 off + RMS_SLICE_SAMPLES <= CORRELATION_STEP;
                 off += RMS_SLICE_SAMPLES) {
                float slice_sum = 0.0f;
                for (size_t i = 0; i < RMS_SLICE_SAMPLES; ++i) {
                    float s = buffer_[wrapRingIndexLocked(rms_probe_pos + off + i)];
                    slice_sum += s * s;
                }
                max_slice_rms = std::max(
                    max_slice_rms,
                    std::sqrt(slice_sum / static_cast<float>(RMS_SLICE_SAMPLES)));
            }
            rms = max_slice_rms;
        }

        // OTA-connected mode can run at lower absolute amplitudes than simulator
        // defaults. Use an adaptive gate so valid low-level frames are not skipped.
        float rms_gate = CORR_NOISE_THRESHOLD;
        if (disconnected_mc_dpsk) {
            float noise_floor = std::max(0.0005f, noise_floor_);
            if (rms < CORR_NOISE_THRESHOLD) {
                noise_floor_ = 0.98f * noise_floor + 0.02f * rms;
            } else {
                noise_floor_ = 0.995f * noise_floor + 0.005f * rms;
            }

            // Before sync there is no SNR estimate. Use the measured audio
            // floor, but never raise the historical 0.025 gate; this only
            // relaxes acquisition when high-SNR fading leaves low absolute RMS.
            rms_gate = std::clamp(noise_floor_ * 3.0f, 0.006f, CORR_NOISE_THRESHOLD);
            if (audio_activity_.load(std::memory_order_relaxed)) {
                rms_gate = std::min(rms_gate, 0.012f);
            }
        } else if (connected_data_preamble) {
            float noise_floor = std::max(0.001f, noise_floor_);
            if (rms < noise_floor * 3.0f) {
                noise_floor_ = 0.98f * noise_floor + 0.02f * rms;
            } else {
                noise_floor_ = 0.995f * noise_floor + 0.005f * rms;
            }

            // Typical OTA values observed around 0.02-0.04 RMS; keep floor low enough
            // to avoid starving detectDataSync() while still skipping true silence.
            rms_gate = std::clamp(noise_floor_ * 2.2f, 0.015f, 0.040f);
            if (sync_controller_.sync_reject_streak_ >= 8) {
                float relax = std::min(0.010f,
                                       0.001f * static_cast<float>(sync_controller_.sync_reject_streak_ - 7));
                rms_gate = std::max(0.012f, rms_gate - relax);
            }
        }

        if (rms < rms_gate) {
            // No signal - advance by small step (100ms = 4800 samples)
            static int rms_skip_count = 0;
            if (++rms_skip_count % 10 == 1)
                LOG_MODEM(INFO, "[%s] searchForSync: RMS skip, rms=%.4f < %.3f, corr_pos=%zu, total=%.2fs",
                          log_prefix_.c_str(), rms, rms_gate, correlation_pos_, audio_sec);
            correlation_pos_ = wrapRingIndexLocked(correlation_pos_ + CORRELATION_STEP);
            return;
        }

        // Signal detected - log before running correlation (only occasionally to reduce spam)
        static int run_log_count = 0;
        if (++run_log_count % 10 == 1) {
            LOG_MODEM(INFO, "[%s] searchForSync: RUNNING correlation, rms=%.4f, corr_pos=%zu, total=%.2fs",
                      log_prefix_.c_str(), rms, correlation_pos_, audio_sec);
        }

        // Signal present - back up search start to catch chirp that might have started
        // in the lead-in silence. The TX lead-in is ~150ms (7200 samples), so we should
        // back up at least that much to ensure the chirp START is in our search window.
        // FIX: We may have skipped past the chirp start during low-RMS phases.
        constexpr size_t SEARCH_BACKTRACK = 9600; // Back up slightly more than lead-in

        if (!used_warm_timed_window) {
            if (correlation_pos_ >= SEARCH_BACKTRACK) {
                search_start = correlation_pos_ - SEARCH_BACKTRACK;
            } else if (total_fed_ < buffer_capacity_samples_) {
                // Buffer hasn't wrapped yet, start from beginning
                search_start = 0;
            } else {
                // Buffer wrapped, handle underflow
                search_start = wrapRingIndexLocked(buffer_capacity_samples_ + correlation_pos_ - SEARCH_BACKTRACK);
            }
        }

        // Do not let the backtrack window re-enter audio that a previous decode
        // already consumed. On sustained OFDM ACK traffic, searching the tail of a
        // just-decoded 1-CW control frame can find false LTS-like peaks; those
        // false locks then escalate into expensive fixed-frame LDPC attempts and delay
        // real ACKs long enough to trigger ARQ retransmission storms.
        if (!used_warm_timed_window && search_floor_abs_valid_) {
            if (search_floor_abs_ < oldest_abs) {
                search_floor_abs_ = oldest_abs;
            }
            if (search_floor_abs_ > total_fed_) {
                search_floor_abs_ = total_fed_;
            }

            size_t search_start_abs = ringPosToAbsoluteLocked(search_start);
            if (search_start_abs < search_floor_abs_) {
                if (total_fed_ - search_floor_abs_ < min_search) {
                    static int floor_wait_count = 0;
                    if (++floor_wait_count % 50 == 1) {
                        LOG_MODEM(INFO,
                                  "[%s] searchForSync: SKIP post-frame floor, available=%zu < min=%zu",
                                  log_prefix_.c_str(), total_fed_ - search_floor_abs_, min_search);
                    }
                    return;
                }
                search_start = absoluteToRingLocked(search_floor_abs_);
            }
        }

        search_buffer.resize(min_search);
        for (size_t i = 0; i < min_search; i++) {
            search_buffer[i] = buffer_[wrapRingIndexLocked(search_start + i)];
        }

        if (used_warm_timed_window) {
            correlation_pos_ = absoluteToRingLocked(warm_narrow_end_abs);
            LOG_MODEM(INFO,
                      "[%s] warm-sync: %s LTS search expected=%zu start_abs=%zu size=%zu confidence=%.2f",
                      log_prefix_.c_str(),
                      used_warm_narrow_window ? "narrow" : "degraded",
                      sync_controller_.next_expected_frame_sample_,
                      warm_plan.search_start_abs, min_search,
                      sync_controller_.frame_arrival_confidence_);
        } else {
            // Advance by small step (100ms = 4800 samples) for accurate detection
            correlation_pos_ = wrapRingIndexLocked(correlation_pos_ + CORRELATION_STEP);
        }
    }

    // DEBUG: Dump the search buffer on first few searches
    static int search_dump_count = 0;
    if (g_debug_dumps_enabled && search_dump_count < 5) {
        char label[64];
        snprintf(label, sizeof(label), "search_%d_pos%zu", search_dump_count, search_start);

        // Dump search buffer to file
        char filename[256];
        snprintf(filename, sizeof(filename), "%s_%s.f32", g_dump_prefix, label);
        std::ofstream file(filename, std::ios::binary);
        if (file) {
            file.write(reinterpret_cast<const char*>(search_buffer.data()),
                       search_buffer.size() * sizeof(float));
            file.close();

            // Compute stats
            float rms = 0, max_val = 0;
            for (size_t i = 0; i < std::min(search_buffer.size(), size_t(10000)); i++) {
                rms += search_buffer[i] * search_buffer[i];
                max_val = std::max(max_val, std::abs(search_buffer[i]));
            }
            rms = std::sqrt(rms / std::min(search_buffer.size(), size_t(10000)));

            LOG_MODEM(DEBUG, "StreamingDecoder: Search #%d: dumped %zu samples to %s (start=%zu, RMS=%.4f, peak=%.4f)",
                      search_dump_count, search_buffer.size(), filename, search_start, rms, max_val);
        }
        search_dump_count++;
    }

    // Search for sync (no lock held - this is the slow part)
    auto search_start_time = std::chrono::steady_clock::now();

    waveform_->reset();
    SyncResult sync_result;
    bool found = false;

    // When connected, use light sync (LTS training symbols, no chirp) after the
    // first connected OFDM frame has established an OFDM-specific chirp+LTS anchor.
    // Reject false positives where data autocorrelation produces spurious peaks
    // (observed up to 0.63). Real LTS correlation is always >0.81 even on
    // moderate fading.
    // Coherent modes need higher sync quality — badly-synced frames always fail
    // because stale LTS phases can't be recovered by DD tracking alone.
    const bool is_coherent = (current_modulation_ == Modulation::QPSK ||
                              current_modulation_ == Modulation::BPSK);
    // Narrowband LTS has ~35% of wideband energy (21 vs 59 carriers) → lower correlation peak
    const bool is_narrowband = (mode_ == protocol::WaveformMode::OFDM_NARROW);
    // LTS sync thresholds.
    // True LTS peaks: 0.85-0.99 (clean), but Moderate SNR12 hardware traces
    // show real tail DATA can dip to ~0.52-0.56. Data autocorrelation noise is
    // usually 0.20-0.45; candidates admitted near the low end still pass the
    // downstream LLR/LDPC gates before they can be accepted as frames.
    const size_t light_sync_candidate_window_samples =
        used_warm_narrow_window ? warm_narrow_candidate_span_samples : LIGHT_SEARCH_SIZE;
    const auto light_sync_thresholds = signal_policy::lightSyncThresholds(
        is_coherent, is_narrowband, connected_, sync_controller_.sync_reject_streak_,
        used_warm_narrow_window, LIGHT_SEARCH_SIZE,
        light_sync_candidate_window_samples);

    if (use_light_search) {
        float known_cfo = sync_controller_.last_cfo_.load();

        if (use_short_reanchor_search) {
            found = waveform_->detectShortDataSync(
                SampleSpan(search_buffer.data(), search_buffer.size()),
                sync_result, known_cfo, CORR_DETECT_THRESHOLD,
                short_reanchor_chirp_ms);
            if (found) {
                sync_controller_.sync_reject_streak_ = 0;
                LOG_MODEM(INFO,
                          "[%s] DATA sync detected by short re-anchor (chirp=%.0f ms, known CFO=%.1f Hz, corr=%.2f)",
                          log_prefix_.c_str(), short_reanchor_chirp_ms,
                          known_cfo, sync_result.correlation);
                if (data_sync_accepted_callback_) {
                    data_sync_accepted_callback_(sync_result.correlation);
                }
            }
        }

        if (!found) {
            found = waveform_->detectDataSync(
                SampleSpan(search_buffer.data(), search_buffer.size()),
                sync_result, known_cfo, CORR_DETECT_THRESHOLD);

            // Reject clear false positives (noise floor is ~0.2-0.4)
            auto sync_decision = signal_policy::evaluateLightSyncCandidate(
                found, sync_result.correlation, is_coherent, connected_,
                sync_controller_.sync_reject_streak_, light_sync_thresholds);
            // §16.8 step 2 (ULTRA_S16_WARM_HANDOFF): the coherent-QPSK
            // sync threshold is 0.90 because stale LTS phases can't be
            // recovered by DD tracking alone. In the warm-handoff regime we
            // are NOT stale — the BURST_HEADER just decoded with a fresh full
            // chirp+LTS anchor and seeded sync_controller_.last_cfo_. This override is a
            // BACKSTOP for a group-start DATA frame whose light-LTS dips just
            // under 0.90 right after a known-good anchor. The PRIMARY fix for
            // group-boundary acquisition is re-arming the descriptor chirp
            // anchor every group (streaming_burst_interleave.cpp end-of-group),
            // which keeps the contiguous data correlating high (~0.91); this
            // override should rarely fire once that anchor is used.
            const char* s16_env =
                std::getenv("ULTRA_S16_WARM_HANDOFF");
            const bool s16_warm_handoff =
                s16_env && std::atoi(s16_env) != 0;
            constexpr float kS16WarmHandoffMinCorrelation = 0.55f;
            const bool s16_warm_override =
                s16_warm_handoff && is_coherent &&
                sync_controller_.warm_sync_phase_ ==
                    arrival_policy::WarmSyncPhase::WARM &&
                sync_decision.rejected && found &&
                sync_result.correlation >= kS16WarmHandoffMinCorrelation;
            if (s16_warm_override) {
                LOG_MODEM(INFO,
                    "[%s] s16-warm-handoff: ACCEPT light-LTS sync corr=%.2f "
                    "(WARM phase, conf=%.2f, threshold-floor=%.2f); coherent "
                    "0.90 gate bypassed",
                    log_prefix_.c_str(), sync_result.correlation,
                    sync_controller_.frame_arrival_confidence_,
                    kS16WarmHandoffMinCorrelation);
                sync_decision.found = true;
                sync_decision.rejected = false;
                sync_decision.next_reject_streak = 0;
            } else if (found && sync_result.correlation < light_sync_thresholds.min_confidence) {
                if (sync_decision.weak_accept) {
                    LOG_MODEM(INFO, "[%s] DATA sync weak-accepted (corr=%.2f < %.2f, streak=%llu)",
                              log_prefix_.c_str(), sync_result.correlation,
                              light_sync_thresholds.min_confidence,
                              static_cast<unsigned long long>(sync_controller_.sync_reject_streak_));
                } else if (sync_decision.rejected) {
                    LOG_MODEM(INFO, "[%s] DATA sync rejected (corr=%.2f < %.2f, streak=%llu)",
                              log_prefix_.c_str(), sync_result.correlation,
                              light_sync_thresholds.min_confidence,
                              static_cast<unsigned long long>(sync_decision.next_reject_streak));
                }
            }

            // §16.8 WARM position-gating (low-SNR fix, 2026-05-31 — see
            // docs/SYNC_ACQUISITION_FIX_PLAN_2026_05_31.md). At low SNR the warm light-LTS
            // correlation floors at NOISE (~0.15 measured at DQPSK R1/4 AWGN@10), so the frame is
            // UNFINDABLE by search and the normal gate rejects it — even though the cadence
            // prediction is correct (descriptor-seeded, contiguous frames) and the data decodes
            // (legacy: 776 CW on the same signal). The audit §9.7 fix: in WARM with the narrow
            // predicted window, do NOT gate on LTS correlation — PROCESS at the predicted position
            // and let LDPC be the acceptance decision (the LTS there still gives the channel
            // estimate H). detectDataSync's reported position is noise here, so we use
            // next_expected. Engages ONLY when the normal correlation path already failed, so
            // higher-SNR locks (which find the true peak) are byte-identical. On a misprediction
            // the frame's LDPC simply fails → existing NACK / §16.4 full-chirp escalation handles
            // it. Flag-gated by ULTRA_S16_WARM_HANDOFF.
            if (s16_warm_handoff && !sync_decision.found &&
                sync_controller_.warm_sync_phase_ == arrival_policy::WarmSyncPhase::WARM &&
                light_sync_thresholds.narrow_expected_window &&
                sync_controller_.next_expected_frame_sample_valid_ &&
                sync_controller_.next_expected_frame_sample_ >= search_start &&
                (sync_controller_.next_expected_frame_sample_ - search_start) < search_buffer.size()) {
                sync_result.start_sample =
                    static_cast<int>(sync_controller_.next_expected_frame_sample_ - search_start);
                sync_result.cfo_hz = known_cfo;
                sync_result.correlation = 0.0f;  // position-gated, not correlation-found
                sync_decision.found = true;
                sync_decision.rejected = false;
                sync_decision.next_reject_streak = 0;
                LOG_MODEM(INFO,
                    "[%s] WARM position-gated: processing predicted frame at abs=%llu "
                    "(light-LTS corr below noise floor; cadence-located, LDPC validates)",
                    log_prefix_.c_str(),
                    static_cast<unsigned long long>(sync_controller_.next_expected_frame_sample_));
            }

            found = sync_decision.found;
            sync_controller_.sync_reject_streak_ = sync_decision.next_reject_streak;

            if (found) {
                if (light_sync_thresholds.narrow_expected_window) {
                    LOG_MODEM(INFO,
                              "[%s] DATA sync detected in warm window (known CFO=%.1f Hz, corr=%.2f, threshold=%.2f, window_reduction=%.2fx)",
                              log_prefix_.c_str(), known_cfo, sync_result.correlation,
                              light_sync_thresholds.min_confidence,
                              light_sync_thresholds.false_positive_window_reduction);
                } else {
                    LOG_MODEM(INFO, "[%s] DATA sync detected (training only, known CFO=%.1f Hz, corr=%.2f)",
                              log_prefix_.c_str(), known_cfo, sync_result.correlation);
                }
                if (data_sync_accepted_callback_) {
                    data_sync_accepted_callback_(sync_result.correlation);
                }
            }

            // §16.4 escalation: warm/light group-start acquisition has failed
            // for many consecutive candidates. On a coherent-QPSK Good@20
            // transfer the next group's light LTS sits below the 0.90 gate
            // (Obs 1.6.b) or simply isn't where warm predicted, so bravo
            // rejects forever and the transfer stalls. Arm a full chirp+LTS
            // re-anchor: the sender pays for a chirp on its RESEND
            // (force_full_preamble), and the full-anchor search path also
            // applies the 0.52 differential threshold that can admit a
            // still-arriving first-attempt light frame. sync_controller_.expect_full_ofdm_anchor_
            // is cleared again after the next clean data decode, so this is a
            // one-group escalation, not a permanent revert to per-group chirps.
            const char* s16_escalate_env =
                std::getenv("ULTRA_S16_WARM_HANDOFF");
            const bool s16_escalate_on =
                s16_escalate_env && std::atoi(s16_escalate_env) != 0;
            if (s16_escalate_on && !found && connected_ &&
                mode_ == protocol::WaveformMode::OFDM_CHIRP &&
                !sync_controller_.expect_full_ofdm_anchor_ &&
                sync_controller_.sync_reject_streak_ >=
                    signal_policy::kConnectedOFDMReanchorEscalateStreak) {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                sync_controller_.expect_full_ofdm_anchor_ = true;
                sync_controller_.sync_reject_streak_ = 0;
                LOG_MODEM(INFO,
                    "[%s] §16.4 escalation: %llu light rejects at group boundary; "
                    "arming full chirp+LTS re-anchor for sender RESEND",
                    log_prefix_.c_str(),
                    static_cast<unsigned long long>(
                        signal_policy::kConnectedOFDMReanchorEscalateStreak));
            }
        }
        // Short re-anchor fallback is enabled only by negotiated fading class;
        // otherwise the connected path remains LTS-only.
    } else {
        // Use full sync detection with chirp (wideband)
        found = waveform_->detectSync(
            SampleSpan(search_buffer.data(), search_buffer.size()),
            sync_result, CORR_DETECT_THRESHOLD);

        if (found && use_full_ofdm_anchor_search) {
            LOG_MODEM(INFO, "[%s] Full OFDM anchor sync detected while connected (corr=%.2f)",
                      log_prefix_.c_str(), sync_result.correlation);
            if (data_sync_accepted_callback_) {
                data_sync_accepted_callback_(sync_result.correlation);
            }
        }

        if (!found && use_full_ofdm_anchor_search) {
            // §16 Phase 5 instrumentation: detectSync failed to lock the
            // descriptor chirp. sync_result.correlation holds the peak dual-chirp
            // correlation (max up/down) even on failure. Logging it disambiguates
            // a THRESHOLD miss (peak just under 0.15 → SNR/window-edge) from a
            // NO-CHIRP-IN-WINDOW miss (peak ≈ 0 → search window misaligned with
            // the descriptor arrival). ULTRA_S16_TRACE_WARM_WINDOW gates it.
            if (const char* t = std::getenv("ULTRA_S16_TRACE_WARM_WINDOW");
                t && std::atoi(t) != 0) {
                LOG_MODEM(INFO,
                    "[%s] s16-phase5: detectSync MISS chirp_peak=%.3f (thr=%.2f) "
                    "corr_pos=%zu total=%zu search_start=%zu min_search=%zu",
                    log_prefix_.c_str(), sync_result.correlation,
                    CORR_DETECT_THRESHOLD, correlation_pos_, total_fed_,
                    search_start, min_search);
            }
            SyncResult light_sync_result;
            const float known_cfo = sync_controller_.last_cfo_.load();
            const bool light_found = waveform_->detectDataSync(
                SampleSpan(search_buffer.data(), search_buffer.size()),
                light_sync_result, known_cfo, CORR_DETECT_THRESHOLD);
            // In connected OFDM the next frame type is unknown at acquisition time.
            // Even when the data profile is coherent QPSK/QAM, ACK/SACK/TURN
            // controls use a hardened R1/4 control profile and are validated
            // by the downstream control-first LDPC parse. Do not apply the
            // coherent-data sync threshold to this full-anchor fallback, or
            // real control frames in Good fading can be rejected before the
            // robust decoder can see them.
            const bool unknown_frame_uses_control_sync_threshold = false;
            const auto fallback_thresholds = signal_policy::lightSyncThresholds(
                unknown_frame_uses_control_sync_threshold, is_narrowband,
                connected_, sync_controller_.sync_reject_streak_);
            auto sync_decision = signal_policy::evaluateLightSyncCandidate(
                light_found, light_sync_result.correlation,
                unknown_frame_uses_control_sync_threshold, connected_,
                sync_controller_.sync_reject_streak_, fallback_thresholds);
            if (light_found && light_sync_result.correlation < fallback_thresholds.min_confidence) {
                if (sync_decision.weak_accept) {
                    LOG_MODEM(INFO,
                              "[%s] Full-anchor wait fell back to weak DATA sync (corr=%.2f < %.2f, streak=%llu)",
                              log_prefix_.c_str(), light_sync_result.correlation,
                              fallback_thresholds.min_confidence,
                              static_cast<unsigned long long>(sync_controller_.sync_reject_streak_));
                } else if (sync_decision.rejected) {
                    LOG_MODEM(INFO,
                              "[%s] Full-anchor wait rejected DATA fallback (corr=%.2f < %.2f, streak=%llu)",
                              log_prefix_.c_str(), light_sync_result.correlation,
                              fallback_thresholds.min_confidence,
                              static_cast<unsigned long long>(sync_decision.next_reject_streak));
                }
            }
            sync_controller_.sync_reject_streak_ = sync_decision.next_reject_streak;
            if (sync_decision.found) {
                found = true;
                sync_result = light_sync_result;
                used_full_anchor_fallback = true;
                LOG_MODEM(INFO,
                          "[%s] Full OFDM anchor not found; accepted connected DATA sync fallback (corr=%.2f)",
                          log_prefix_.c_str(), sync_result.correlation);
                if (data_sync_accepted_callback_) {
                    data_sync_accepted_callback_(sync_result.correlation);
                }
            }
        }

        // Dual-listen: if wideband didn't find anything, try narrowband chirp
        if (!found && !connected_) {
            // Lazy-init narrowband waveform on first use
            if (!narrow_waveform_initialized_) {
                // Create narrowband MC-DPSK waveform for chirp detection
                // Uses 1250-1750 Hz chirp, 4 carriers @ 1300-1700 Hz
                narrow_waveform_ = WaveformFactory::createNarrowbandMCDPSK();
                narrow_waveform_initialized_ = true;
                LOG_MODEM(INFO, "[%s] Dual-listen: narrowband MC-DPSK waveform initialized",
                          log_prefix_.c_str());
            }

            if (narrow_waveform_) {
                SyncResult narrow_result;
                bool narrow_found = narrow_waveform_->detectSync(
                    SampleSpan(search_buffer.data(), search_buffer.size()),
                    narrow_result, CORR_DETECT_THRESHOLD);

                if (narrow_found) {
                    found = true;
                    sync_result = narrow_result;
                    detected_bandwidth_ = BandwidthMode::NARROW;
                    // Switch main waveform to narrowband MC-DPSK so CONNECT frames decode correctly
                    waveform_ = WaveformFactory::createNarrowbandMCDPSK();
                    LOG_MODEM(INFO, "[%s] Dual-listen: NARROWBAND chirp detected! corr=%.3f, CFO=%.1f Hz, switched to narrowband MC-DPSK",
                              log_prefix_.c_str(), narrow_result.correlation, narrow_result.cfo_hz);
                }
            }
        }

        // If wideband found something, mark as wide
        if (found && detected_bandwidth_ != BandwidthMode::NARROW) {
            detected_bandwidth_ = BandwidthMode::WIDE;
        }
    }

    auto search_end_time = std::chrono::steady_clock::now();
    float search_ms = std::chrono::duration<float, std::milli>(search_end_time - search_start_time).count();

    // Check if reset() was called during our search - if so, discard results
    if (reset_generation_.load() != gen_at_start) {
        LOG_MODEM(INFO, "[%s] searchForSync: ABORTED - reset() called during search", log_prefix_.c_str());
        return;
    }

    // Log timing: total_fed_ tells us how much audio has arrived
    float audio_sec = total_fed_ / 48000.0f;
    if (found || search_ms > 100) {  // Log if found or if search was slow
        LOG_MODEM(INFO, "[%s] searchForSync: audio=%.2fs, search=%.1fms, found=%d, corr=%.3f",
                  log_prefix_.c_str(), audio_sec, search_ms, found ? 1 : 0, sync_result.correlation);
    }

    if (found) {
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        sync_position_ = wrapRingIndexLocked(search_start + sync_result.start_sample);

        const bool timing_cfo_genie =
            qam16GenieTimingCfoEnabled() &&
            connected_data_preamble &&
            current_modulation_ == Modulation::QAM16 &&
            use_light_search &&
            used_warm_timed_window &&
            sync_controller_.next_expected_frame_sample_valid_;
        if (timing_cfo_genie) {
            const size_t detected_abs = ringPosToAbsoluteLocked(sync_position_);
            const size_t expected_abs = sync_controller_.next_expected_frame_sample_;
            sync_position_ = absoluteToRingLocked(expected_abs);
            LOG_MODEM(WARN,
                      "[%s] DIAG genie-timing-cfo: overriding light-LTS sync "
                      "detected_abs=%zu expected_abs=%zu delta=%lld corr=%.3f",
                      log_prefix_.c_str(),
                      detected_abs,
                      expected_abs,
                      static_cast<long long>(
                          arrival_policy::signedSampleError(detected_abs, expected_abs)),
                      sync_result.correlation);
        }

        // Anti-replay: reject sync at same position as last decoded frame (circular distance)
        if (last_decoded_sync_pos_ != SIZE_MAX) {
            size_t d1 = (sync_position_ >= last_decoded_sync_pos_)
                ? (sync_position_ - last_decoded_sync_pos_)
                : (buffer_capacity_samples_ - last_decoded_sync_pos_ + sync_position_);
            size_t dist = std::min(d1, buffer_capacity_samples_ - d1);
            if (dist < 200) {
                LOG_MODEM(INFO, "[%s] Anti-replay: duplicate sync at pos=%zu (prev=%zu), skipping",
                          log_prefix_.c_str(), sync_position_, last_decoded_sync_pos_);
                constexpr size_t SEARCH_BACKTRACK = 9600;
                correlation_pos_ = wrapRingIndexLocked(sync_position_ + SEARCH_BACKTRACK + CORRELATION_STEP);
                return;
            }
        }

        // Provide absolute training position to waveform so initial CFO phase is aligned.
        if (waveform_) {
            const size_t abs_training_pos = ringPosToAbsoluteLocked(sync_position_);
            waveform_->setAbsoluteTrainingPosition(abs_training_pos);
        }

        // CFO handling: On fading channels, chirp-based CFO measurement can be corrupted
        // by multipath (peaks shift differently for up vs down chirp).
        // When connected, trust the established CFO and limit drift.
        float new_cfo = sync_result.cfo_hz;
        float known_cfo = sync_controller_.last_cfo_.load();

        const auto cfo_decision = signal_policy::limitConnectedCFODrift(
            connected_, new_cfo, known_cfo);
        if (cfo_decision.clamped) {
            LOG_MODEM(INFO, "[%s] CFO sanity: measured=%.1f, known=%.1f, diff=%.1f > %.1f, using known",
                      log_prefix_.c_str(), new_cfo, known_cfo, cfo_decision.diff_hz,
                      signal_policy::kMaxSyncCFODriftHz);
            new_cfo = cfo_decision.accepted_cfo;  // Trust established CFO over noisy measurement
        }
        if (timing_cfo_genie) {
            LOG_MODEM(WARN,
                      "[%s] DIAG genie-timing-cfo: forcing sync CFO %.2f Hz -> 0.00 Hz",
                      log_prefix_.c_str(), new_cfo);
            new_cfo = 0.0f;
        }

        sync_cfo_ = new_cfo;
        sync_snr_ = chirpSyncQualityDb(sync_result.correlation, noise_floor_);
        sync_correlation_ = sync_result.correlation;
        sync_gap_error_samples_ = sync_result.gap_error_samples;
        sync_start_time_ = std::chrono::steady_clock::now();
        pending_total_cw_ = 0;
        sync_from_warm_timed_window_ = used_warm_timed_window;
        sync_from_full_anchor_fallback_ = used_full_anchor_fallback;

        state_ = DecoderState::SYNC_FOUND;

        last_snr_.store(sync_snr_);
        sync_controller_.last_cfo_.store(sync_cfo_);

        LOG_MODEM(INFO, "[%s] SYNC at pos=%zu, CFO=%.1f Hz, SNR=%.1f dB (%s)",
                  log_prefix_.c_str(), sync_position_, sync_cfo_, sync_snr_,
                  snrSourceToString(SNRSource::SYNC_QUALITY));

        // NOTE: Do NOT advance correlation_pos_ past the frame here.
        // It was already advanced by CORRELATION_STEP at line 323 during search.
        // The post-decode skip at decodeCurrentFrame() line 718 handles advancing
        // past the decoded frame when we return to SEARCHING.
        //
        // Previously, this code jumped correlation_pos_ past the entire frame,
        // which could place it AHEAD of write_pos_ in circular buffer space
        // (especially after buffer wraps). This caused feedAudio()'s overflow
        // check to compute unsearched ≈ buffer_size, triggering spurious
        // buffer overflows and data loss during async decode.
    } else if (!search_buffer.empty()) {
        if (used_warm_timed_window) {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            setSearchFloorLocked(warm_narrow_end_abs);
            noteFrameArrivalSyncMissLocked();
            LOG_MODEM(INFO,
                      "[%s] warm-sync: no LTS in %s expected window, misses=%d confidence=%.2f",
                      log_prefix_.c_str(),
                      used_warm_narrow_window ? "narrow" : "degraded",
                      sync_controller_.consecutive_sync_misses_,
                      sync_controller_.frame_arrival_confidence_);
        }
        const size_t idle_count = std::min(search_buffer.size(), CORRELATION_STEP);
        observeIdleNoiseCandidate(search_buffer.data(), idle_count);
    }
}

void StreamingDecoder::checkIfReadyToDecode() {
    if (!waveform_) {
        state_ = DecoderState::SEARCHING;
        return;
    }

    // How many samples do we have from sync position?
    size_t available;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        if (write_pos_ >= sync_position_) {
            available = write_pos_ - sync_position_;
        } else {
            available = buffer_capacity_samples_ - sync_position_ + write_pos_;
        }
    }

    // Check elapsed time after selecting the sample requirement. Large variable
    // OFDM frames can be longer than the legacy 5s fixed-frame wait.
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - sync_start_time_).count();

    // Calculate how much we need — must match decodeCurrentFrame() buffer sizing.
    bool is_ofdm_here = protocol::isOFDMMode(mode_);
    // 2026-05-29 channel-adaptive interleaver (RX decouple): mirror streaming_ofdm_decode —
    // burst_latched = group-start marker detected (interleave-independent); the
    // ConnectedOFDMBurst full-frame sizing is keyed on the marker + the burst regime.
    bool burst_latched = waveform_ && waveform_->wasBurstInterleaved();
    const bool burst_regime_active = use_burst_interleave_ || burst_transport_rx_;
    const size_t pending_samples = pending_total_cw_ > 0
        ? static_cast<size_t>(waveform_->getMinSamplesForCWCount(pending_total_cw_))
        : 0;
    const size_t ofdm_control_samples = (is_ofdm_here && connected_)
        ? getOFDMControlFrameSamplesForCurrentMode()
        : 0;
    const size_t full_frame_samples = (is_ofdm_here && connected_)
        ? static_cast<size_t>(waveform_->getMinSamplesForCWCount(fixed_frame_codewords_))
        : 0;
    const size_t control_frame_samples =
        static_cast<size_t>(waveform_->getMinSamplesForControlFrame());
    auto requirement = decode_policy::selectDecodeSampleRequirement(
        pending_total_cw_,
        is_ofdm_here,
        connected_,
        burst_regime_active,
        burst_latched,
        pending_samples,
        ofdm_control_samples,
        full_frame_samples,
        control_frame_samples);

    static constexpr int kAudioSampleRateHz = 48000;
    const int required_audio_ms = static_cast<int>(
        (requirement.samples * 1000 + kAudioSampleRateHz - 1) / kAudioSampleRateHz);
    const int frame_timeout_ms = std::max(FRAME_TIMEOUT_MS, required_audio_ms + 2000);
    if (elapsed > frame_timeout_ms) {
        LOG_MODEM(WARN, "[%s] Frame timeout after %lld ms (need=%zu samples, timeout=%d ms)",
                  log_prefix_.c_str(), (long long)elapsed, requirement.samples, frame_timeout_ms);
        state_ = DecoderState::SEARCHING;
        return;
    }

    if (available >= requirement.samples) {
        state_ = DecoderState::DECODING;
    }
}

// Observability counters for robust decode paths (check via debugger or periodic log)

} // namespace gui
} // namespace ultra
