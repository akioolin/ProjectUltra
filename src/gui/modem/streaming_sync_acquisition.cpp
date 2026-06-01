// StreamingDecoder module

#include "streaming_decoder.hpp"
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

namespace decode_policy = streaming_decode_policy;
namespace arrival_policy = ::ultra::sync::frame_arrival_policy;
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
    // (R4: the adaptive short-chirp re-anchor was removed — superseded by warm-handoff,
    // which is now the default. The light/full-anchor search are the only group-boundary paths.)
    size_t min_search = use_light_search ? LIGHT_SEARCH_SIZE : chirp_min_search;
    const size_t data_symbol_samples =
        (use_light_search && waveform_)
            ? static_cast<size_t>(std::max(1, waveform_->getSamplesPerSymbol()))
            : 0;

    const bool disconnected_mc_dpsk =
        !connected_ && mode_ == protocol::WaveformMode::MC_DPSK;
    auto win = sync_controller_.acquireSearchWindow(
        use_light_search, connected_data_preamble, disconnected_mc_dpsk,
        min_search, data_symbol_samples,
        audio_activity_.load(std::memory_order_relaxed),
        CORRELATION_STEP, CORR_NOISE_THRESHOLD);
    if (!win.ready) return;
    std::vector<float> search_buffer = std::move(win.search_buffer);
    size_t search_start = win.search_start;
    min_search = win.min_search;
    bool used_warm_timed_window = win.used_warm_timed_window;
    bool used_warm_narrow_window = win.used_warm_narrow_window;
    size_t warm_narrow_end_abs = win.warm_narrow_end_abs;
    size_t warm_narrow_candidate_span_samples = win.warm_narrow_candidate_span_samples;

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
        is_coherent, is_narrowband, connected_, sync_controller_.syncRejectStreak(),
        used_warm_narrow_window, LIGHT_SEARCH_SIZE,
        light_sync_candidate_window_samples);

    if (use_light_search) {
        // §7 C3 Phase 3b: the connected-data light-LTS DETECTION + acceptance + §16.4 escalation
        // now lives on the controller (detectConnectedLightSync); the decoder passes its current
        // waveform + the search window and fires the data-sync-accepted callback on success.
        found = sync_controller_.detectConnectedLightSync(
            waveform_.get(), search_buffer.data(), search_buffer.size(), search_start,
            is_coherent, connected_, mode_, light_sync_thresholds, CORR_DETECT_THRESHOLD,
            sync_result);
        if (found && data_sync_accepted_callback_) {
            data_sync_accepted_callback_(sync_result.correlation);
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
            // §7 C3 Phase 3b: the full-anchor light-LTS fallback (the §16 phase-5 trace +
            // detectDataSync + control-threshold evaluation + streak update + weak-accept) lives on
            // the controller; the decoder passes its waveform + the search window and fires the
            // data-sync-accepted callback on success.
            auto fb = sync_controller_.detectFullAnchorFallback(
                waveform_.get(), search_buffer.data(), search_buffer.size(), search_start,
                min_search, is_narrowband, connected_, CORR_DETECT_THRESHOLD, sync_result);
            if (fb.found) {
                found = true;
                used_full_anchor_fallback = fb.used_full_anchor_fallback;
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

    // Log timing: sync_controller_.ring_.total_fed_ tells us how much audio has arrived
    float audio_sec = sync_controller_.ring_.total_fed_ / 48000.0f;
    if (found || search_ms > 100) {  // Log if found or if search was slow
        LOG_MODEM(INFO, "[%s] searchForSync: audio=%.2fs, search=%.1fms, found=%d, corr=%.3f",
                  log_prefix_.c_str(), audio_sec, search_ms, found ? 1 : 0, sync_result.correlation);
    }

    if (found) {
        std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);

        sync_position_ = sync_controller_.ring_.wrapRingIndexLocked(search_start + sync_result.start_sample);

        const bool timing_cfo_genie =
            qam16GenieTimingCfoEnabled() &&
            connected_data_preamble &&
            current_modulation_ == Modulation::QAM16 &&
            use_light_search &&
            used_warm_timed_window &&
            sync_controller_.next_expected_frame_sample_valid_;
        if (timing_cfo_genie) {
            const size_t detected_abs = sync_controller_.ring_.ringPosToAbsoluteLocked(sync_position_);
            const size_t expected_abs = sync_controller_.next_expected_frame_sample_;
            sync_position_ = sync_controller_.ring_.absoluteToRingLocked(expected_abs);
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
                : (sync_controller_.ring_.buffer_capacity_samples_ - last_decoded_sync_pos_ + sync_position_);
            size_t dist = std::min(d1, sync_controller_.ring_.buffer_capacity_samples_ - d1);
            if (dist < 200) {
                LOG_MODEM(INFO, "[%s] Anti-replay: duplicate sync at pos=%zu (prev=%zu), skipping",
                          log_prefix_.c_str(), sync_position_, last_decoded_sync_pos_);
                constexpr size_t SEARCH_BACKTRACK = 9600;
                sync_controller_.ring_.correlation_pos_ = sync_controller_.ring_.wrapRingIndexLocked(sync_position_ + SEARCH_BACKTRACK + CORRELATION_STEP);
                return;
            }
        }

        // Provide absolute training position to waveform so initial CFO phase is aligned.
        if (waveform_) {
            const size_t abs_training_pos = sync_controller_.ring_.ringPosToAbsoluteLocked(sync_position_);
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
        sync_snr_ = chirpSyncQualityDb(sync_result.correlation, sync_controller_.ring_.noise_floor_);
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

        // NOTE: Do NOT advance sync_controller_.ring_.correlation_pos_ past the frame here.
        // It was already advanced by CORRELATION_STEP at line 323 during search.
        // The post-decode skip at decodeCurrentFrame() line 718 handles advancing
        // past the decoded frame when we return to SEARCHING.
        //
        // Previously, this code jumped sync_controller_.ring_.correlation_pos_ past the entire frame,
        // which could place it AHEAD of sync_controller_.ring_.write_pos_ in circular buffer space
        // (especially after buffer wraps). This caused feedAudio()'s overflow
        // check to compute unsearched ≈ buffer_size, triggering spurious
        // buffer overflows and data loss during async decode.
    } else if (!search_buffer.empty()) {
        if (used_warm_timed_window) {
            std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
            sync_controller_.ring_.setSearchFloorLocked(warm_narrow_end_abs);
            noteFrameArrivalSyncMissLocked();
            LOG_MODEM(INFO,
                      "[%s] warm-sync: no LTS in %s expected window, misses=%d confidence=%.2f",
                      log_prefix_.c_str(),
                      used_warm_narrow_window ? "narrow" : "degraded",
                      sync_controller_.consecutiveSyncMisses(),
                      sync_controller_.frameArrivalConfidence());
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
        std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);

        if (sync_controller_.ring_.write_pos_ >= sync_position_) {
            available = sync_controller_.ring_.write_pos_ - sync_position_;
        } else {
            available = sync_controller_.ring_.buffer_capacity_samples_ - sync_position_ + sync_controller_.ring_.write_pos_;
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
