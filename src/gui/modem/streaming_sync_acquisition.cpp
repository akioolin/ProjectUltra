// StreamingDecoder module

#include "streaming_decoder.hpp"
#include "streaming_buffer_policy.hpp"
#include "streaming_decode_policy.hpp"
#include "streaming_decoder_debug.hpp"
#include "sync/frame_arrival_policy.hpp"
#include "streaming_frame_policy.hpp"
#include "sync/signal_policy.hpp"
#include "waveform/mc_dpsk_waveform.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
#include "fec/frame_interleaver.hpp"
#include "fec/burst_interleaver.hpp"
#include "ultra/fec.hpp"
#include "fec/ldpc_codec.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/timing_profiler.hpp"
#include "protocol/connection_policy.hpp"  // kCoherence{Good,Moderate}Threshold (single source of truth)
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

// ULTRA_CONNECT_RATIOMETRIC_SNR (now DEFAULT-ON; opt-out via =0): route the ratiometric
// MC-DPSK training-symbol SNR for the UN-connected handshake (PING/CONNECT/
// CONNECT_ACK) control frames, not only for connected MC-DPSK data flow.
//
// WHY: the default connect-time rate decision consumes idle_in_band SNR, which
// is 10*log10(kModemReferencePower / measured_noise) -- a NOISE-ONLY meter that
// ASSUMES the received signal sits at the simulator's reference level
// (kModemReferenceInBandRms = 0.30482664). In the faithful sim that holds (AWGN
// is sized from encodePing() at exactly that RMS) so idle is correct. On a real
// radio the RX operating level is several dB below that reference, so idle
// credits signal power the link does not have and OVER-READS the SNR by the
// level deficit -> too-aggressive connect-time rate picks (e.g. MPG@10 -> QPSK
// R1/2 that stalls in the fades). The MC-DPSK training SNR
// (updateTrainingSNREstimate: 10*log10(signal_power/residual_power), both terms
// 50-2950 Hz in-band filtered) is a pure ratio of measured powers -> level-
// invariant by construction (= 10log10(|H|^2/noise_var)), honest at any level,
// and it is already populated for the inbound handshake preamble and already an
// accepted rate-selection source (MCDPSK_IN_BAND). The ONLY thing suppressing
// it during the handshake is the connected_ gate below; this knob relaxes it.
//
// PROMOTED to DEFAULT-ON 2026-07-01 after the multi-channel rig A/B this note demanded:
// on the rig the idle meter over-reads (RX below the reference level) and mis-selects a
// too-aggressive mode that STALLS on BOTH Good (#74, MPG@10) and Moderate (MPM@8, idle
// -> OFDM QPSK R1/2 no-delivery), while ratiometric picks the correct robust mode and
// delivers. The over-read is a LEVEL deficit, channel-independent -> generalizes to AWGN/
// Poor. On the faithful gui_qso gate (level AT reference) the ~-0.45 dB scale offset can
// nudge a pick at a ladder boundary but is more-conservative and never regresses delivery
// (verified good@20 no-regress). See docs/CHANGELOG.md (BUG-CONNECT-SNR-LEVEL).
bool connectRatiometricSnrEnabled() {
    // #74/#71 (2026-07-01): now DEFAULT-ON; opt OUT via ULTRA_CONNECT_RATIOMETRIC_SNR=0.
    // Promoted after multi-channel rig A/B showed the level-dependent idle meter OVER-READS
    // on a real radio and mis-selects a too-aggressive mode that STALLS, while the
    // level-invariant ratiometric SNR picks the correct robust mode and delivers:
    //   MPG@10 (Good, #74): idle 13.1 -> QPSK R1/2 stall; ratiometric 8.0 -> robust rung.
    //   MPM@8  (Moderate):  idle -> OFDM QPSK R1/2 STALL (0 ACKs, 8 timeout resends, no
    //                       delivery); ratiometric ~1 dB -> MC-DPSK DBPSK R1/4 CRC-clean 0-retx.
    // The over-read is a LEVEL deficit (RX below the 0.3048 sim reference), independent of
    // channel type, so it generalizes to AWGN/Poor. On the faithful sim (level AT reference)
    // the two estimators differ by only ~0.45 dB -- can nudge a pick at a ladder boundary but
    // never regresses delivery (more conservative). See docs/CHANGELOG.md.
    static const bool enabled = [] {
        const char* value = std::getenv("ULTRA_CONNECT_RATIOMETRIC_SNR");
        if (!value || value[0] == '\0') return true;      // default-ON
        return !(value[0] == '0' && value[1] == '\0');    // "0" opts out
    }();
    return enabled;
}

// #58 BUG-CONNECT-SNR-VARIANCE (2026-07-02): when a decoded MC-DPSK frame also
// carries a data-aided whole-frame SNR estimate, route THAT as MCDPSK_IN_BAND
// instead of the ~170 ms training snapshot. Same basis (level-invariant,
// in-band 50-2950 Hz ratio), lower variance: the training window is ONE fade
// state (Tc ~ 4.2 s on Good -> rig connect snapshots spread ~10 dB), while the
// frame's CWs span seconds, so the per-symbol average is fade-averaged by
// construction. Decode-then-measure: only routed when THIS frame's LDPC decode
// succeeded. Default-ON; opt OUT via ULTRA_CONNECT_DATA_AIDED_SNR=0.
bool connectDataAidedSnrEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ULTRA_CONNECT_DATA_AIDED_SNR");
        if (!value || value[0] == '\0') return true;      // default-ON
        return !(value[0] == '0' && value[1] == '\0');    // "0" opts out
    }();
    return enabled;
}

}  // namespace

void StreamingDecoder::populateDecodeMetrics(DecodeResult& result, bool is_ofdm,
                                             float residual_cfo_hz) const {
    stampRxSignal();  // F129: decoder evidence of incoming signal (per-frame)
    // Handoff §2: per-frame physical channel SNR from the waveform (power
    // ratio over the exact training span vs THIS frame's burst-time noise
    // ref) — replaces the old handshake-latched decoder-side computation.
    if (!is_ofdm && waveform_ && waveform_->hasPhysicalSNR()) {
        const float phys = waveform_->getPhysicalSNRdB();
        last_physical_snr_db_.store(phys);
        last_physical_snr_valid_.store(true);
        notePhysicalSnrSample(phys);  // §5: distribution ring
    }
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
        // Good/Moderate discriminator: feed one per-frame |H|^2 snapshot (the LTS channel
        // magnitude) into the decoder-hosted Doppler-coherence estimator. populateDecodeMetrics
        // runs ~once per decoded OFDM frame, so the snapshot cadence IS the inter-frame spacing
        // (~1-2 s) — the lag at which Good (slow fading) and Moderate (fast) diverge. Hosting it
        // here (not in the demodulator) survives the per-group demodulator recreation.
        const float lts_mag = waveform_->getLastLTSChannelMagnitude();
        // ESTIMATOR HYGIENE (F142): feed the coherence estimator ONLY from
        // successful decodes. Failed/false-lock attempts (noise windows, our own
        // deferred-ACK echo) contributed |H| snapshots of TONES (across-carrier
        // CV ~3) — four such snapshots poisoned the cumulative lag-1 score below
        // the Moderate threshold, coherenceAdjustedFadingIndex then PINNED
        // effective fading at 0.85 while the real channel read 0.15-0.5, and the
        // ladder ground to the R1/4 basement on a 24 dB link. The decode verdict
        // is the admission ticket for channel-statistics evidence.
        if (result.success && std::isfinite(lts_mag) && lts_mag > 0.0f) {
            doppler_coherence_.addSnapshot(lts_mag * lts_mag);
            // COH-DIAG (read-only diagnostic, env ULTRA_COH_DIAG=1, default OFF): per-frame
            // raw disc inputs for the noise-floor / Doppler-discriminator investigation —
            // snap=|H|^2, h_mag=mean_c|H_c|, lts_noise_var=E|H1-H0|^2/4, lts_sig_pow=mean_c|H_c|^2.
            // Used to classify whether the snapshot decorrelation is LTS estimation noise
            // (de-biasable) vs common-mode cheap-card wander. Gated so it does not spam logs.
            static const bool kCohDiag = [] {
                const char* e = std::getenv("ULTRA_COH_DIAG");
                return e && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
            }();
            if (kCohDiag) {
                const float lts_noise_var = waveform_->getLastLTSNoiseVariance();
                const float lts_sig_pow = waveform_->getLastLTSSignalPower();
                LOG_MODEM(INFO,
                          "[%s] COH-DIAG snap=%.6g h_mag=%.6g lts_noise_var=%.6g lts_sig_pow=%.6g",
                          log_prefix_.c_str(), lts_mag * lts_mag, lts_mag, lts_noise_var,
                          lts_sig_pow);
            }
        }
        result.doppler_coherence_score = doppler_coherence_.coherenceScore();
        result.doppler_hz = doppler_coherence_.dopplerHz();
        result.doppler_coherence_valid = doppler_coherence_.valid();
        last_doppler_coherence_score_.store(result.doppler_coherence_score);
        last_doppler_hz_.store(result.doppler_hz);
        last_doppler_coherence_valid_.store(result.doppler_coherence_valid);
        if (result.doppler_coherence_valid) {
            const float sc = result.doppler_coherence_score;
            const bool confident_good = sc >= protocol::connection_policy::kCoherenceGoodThreshold;
            const bool confident_mod = sc <= protocol::connection_policy::kCoherenceModerateThreshold;
            const char* cls = confident_good ? "GOOD"
                                             : (confident_mod ? "MODERATE/POOR" : "uncertain");
            // ADAPTIVITY_AUDIT Case #2: de-pessimize the Wiener correlation model ONLY on a
            // confident Good (slower 0.1 Hz / 0.5 ms keeps more pilot history -> better
            // estimate). Uncertain / Moderate keep the env-aware Moderate-HF default — and the
            // per-group demod rebuild resets the override, so we never strand a stale Good.
            if (confident_good) waveform_->setWienerChannelParams(0.1f, 0.5e-3f);
            // Stage A (read-only): the scale-invariant coherence-AREA verdict, logged alongside the
            // legacy lag-1 score for live cross-platform validation (the area is the radio-agnostic
            // discriminator; threshold ~0.19, hysteresis enter 0.20/exit 0.12 — see
            // docs/SCALE_INVARIANT_COHERENCE_DISC_2026_06_20.md). NOT yet consumed — Stage B/C wire it.
            const float area = doppler_coherence_.coherenceArea();
            const char* acls = (area >= protocol::connection_policy::kCoherenceAreaEnterGood) ? "GOOD"
                             : (area <  protocol::connection_policy::kCoherenceAreaExitGood) ? "MOD/POOR"
                                                                                            : "uncertain";
            LOG_MODEM(INFO,
                      "[%s] Doppler coherence: score=%.3f doppler=%.3f Hz [%s] area=%.3f [%s] (snaps=%zu) "
                      "vs fading_index=%.3f%s",
                      log_prefix_.c_str(), sc, result.doppler_hz, cls, area, acls,
                      doppler_coherence_.snapshotCount(), result.lts_fading_index,
                      confident_good ? " (Wiener->Good 0.1Hz/0.5ms)" : "");
        }
        result.snr_source = SNRSource::SYNC_QUALITY;
        if (result.has_ofdm_broadband_snr_db) {
            // §5: the recalibrated OFDM broadband reading is truth-calibrated
            // (OFDMSnrCalibration ±1.5 dB) — feed the channel-SNR distribution
            // ring so the mean±spread display keeps updating through OFDM
            // transfers (MC-DPSK frames stop at the handshake).
            notePhysicalSnrSample(result.ofdm_broadband_snr_db);
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
        const bool training_snr_valid =
            mc_waveform && mc_waveform->hasEstimatedSNR() &&
            std::isfinite(mc_waveform->estimatedSNR());
        const bool data_aided_snr_valid =
            mc_waveform && mc_waveform->hasDataAidedSNR() &&
            std::isfinite(mc_waveform->getDataAidedSNRdB());
        if (training_snr_valid) {
            result.has_mcdpsk_training_snr_db = true;
            result.mcdpsk_training_snr_db = mc_waveform->estimatedSNR();
        }
        if (data_aided_snr_valid) {
            result.has_mcdpsk_data_aided_snr_db = true;
            result.mcdpsk_data_aided_snr_db = mc_waveform->getDataAidedSNRdB();
        }
        // connected_ OR the opt-in knob: when ULTRA_CONNECT_RATIOMETRIC_SNR is
        // set, the level-invariant MC-DPSK training SNR also routes for the
        // un-connected handshake frames (so the connect-time rate decision uses
        // an honest, level-agnostic SNR instead of the reference-assuming idle
        // meter). Knob OFF => (connected_ || false) == connected_ => byte-identical.
        if ((connected_ || connectRatiometricSnrEnabled()) &&
            mode_ == protocol::WaveformMode::MC_DPSK && training_snr_valid) {
            // #58 BUG-CONNECT-SNR-VARIANCE: prefer the whole-frame data-aided
            // fade-AVERAGED estimate over the single-fade-state training
            // snapshot when this frame's LDPC decode SUCCEEDED
            // (decode-then-measure). Same source tag: same basis, lower
            // variance. Fall back to the training value otherwise.
            const bool route_data_aided = connectDataAidedSnrEnabled() &&
                                          result.success && data_aided_snr_valid;
            result.snr_db = route_data_aided ? result.mcdpsk_data_aided_snr_db
                                             : result.mcdpsk_training_snr_db;
            result.snr_source = SNRSource::MCDPSK_IN_BAND;
            result.mcdpsk_snr_routed_data_aided = route_data_aided;
            if (data_aided_snr_valid) {
                LOG_MODEM(INFO,
                          "[%s] MC-DPSK SNR: training=%.1f data_aided=%.1f (routed=%s)",
                          log_prefix_.c_str(), result.mcdpsk_training_snr_db,
                          result.mcdpsk_data_aided_snr_db,
                          route_data_aided ? "data_aided" : "training");
            } else {
                LOG_MODEM(INFO,
                          "[%s] MC-DPSK SNR: training=%.1f data_aided=n/a (routed=training)",
                          log_prefix_.c_str(), result.mcdpsk_training_snr_db);
            }
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

        // ═══ SNR-SANITY (handoff §4): usable can NEVER exceed channel ═══
        // The demod-usable (effective) SNR is the channel's physical S:N minus
        // implementation loss — physics forbids usable > channel. When this
        // frame carries BOTH a routed usable reading and the per-frame physical
        // measurement, a violation beyond the combined measurement slop means
        // ONE OF THE METERS IS BROKEN for this frame: log it and downgrade the
        // routed source to SYNC_QUALITY, which acceptsRateSelectionSNR already
        // rejects — the suspect reading cannot steer a rate decision. One
        // frame, never a latch. Margin 2.0 dB = the physical readout's known
        // −0.66 dB noise-ref residual + both estimators' per-frame variance;
        // tighten alongside the §2 residual fix. This exact invariant caught
        // the pre-recalibration OFDM optimism from a single operator log line.
        if (result.snr_source == SNRSource::MCDPSK_IN_BAND &&
            last_physical_snr_valid_.load() &&
            result.snr_db > last_physical_snr_db_.load() + 2.0f) {
            LOG_MODEM(WARN,
                      "[%s] SNR-SANITY: usable %.1f dB EXCEEDS channel %.1f dB "
                      "(+%.1f > 2.0 margin) — meter suspect, reading excluded "
                      "from rate selection for this frame",
                      log_prefix_.c_str(), result.snr_db,
                      last_physical_snr_db_.load(),
                      result.snr_db - last_physical_snr_db_.load());
            result.snr_source = SNRSource::SYNC_QUALITY;
        }
    }
}

void StreamingDecoder::searchForSync() {
    if (!waveform_) return;

    // ACK-LISTEN tone-lock guard (ULTRA_ACKLISTEN_SUPPRESS_OFDM): while this station's
    // tone-burst ACK monitor is armed (we sent a burst and await the peer's 4-FSK ACK —
    // half-duplex, the peer cannot be sending OFDM), suppress the warm DATA-sync
    // acceptance paths in the SyncController so the ACK tone cannot S&C-false-lock the
    // OFDM searcher and race the tone monitor for the same samples (the F73/F74
    // missed-ACK spirals). The dual-chirp path stays live. Refreshed every search
    // pass; disarms automatically with the monitor.
    static const bool kAckListenSuppressOfdm = [] {
        const char* e = std::getenv("ULTRA_ACKLISTEN_SUPPRESS_OFDM");
        // DEFAULT-ON 2026-07-05 (=0 opts out): rig-proven — F75 record 2.62 kbps +
        // 10-run batch F78-F87 (~280 ack exchanges, 0 misses, 0 expired-undetected);
        // half-duplex-provably safe (the peer cannot send OFDM during our ACK window).
        return !(e && e[0] == '0');
    }();
    sync_controller_.setAckListenSuppressDataSync(
        kAckListenSuppressOfdm && connected_ &&
        mode_ == protocol::WaveformMode::OFDM_CHIRP && tone_burst_monitor_.isArmed());

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

    // F165 ANCHORED-BURST ACK BACKSTOP: fire once when the sample clock passes
    // the max group window since an accepted expected anchor with nothing
    // framed (descriptor unreadable in a deep fade -> no group -> no ack -> the
    // sender RTO-starves at a pinned rung). ~12.5 s covers descriptor + anchor
    // + an 8-frame group; the sender's RTO fires ~11 s AFTER its burst ends
    // (~anchor+21 s), so the backstop ack beats it comfortably.
    if (anchored_burst_backstop_armed_) {
        size_t fed_now;
        {
            std::lock_guard<std::mutex> abl(sync_controller_.ring_.buffer_mutex_);
            fed_now = sync_controller_.ring_.total_fed_;
        }
        // DERIVED from the burst policy (GROUP-SIZE co-fix #1): the ceiling
        // clamp maxes at 12,000 ms of group air + 1,410 ms full descriptor +
        // ~1.2 s margin ≈ 14.6 s. A fixed 600k (12.5 s) fired MID-BURST once
        // groups exceed ~10 frames — the adaptivity-bug class. Still beats the
        // sender RTO (~air-end + 11.4 s) by ~9 s.
        constexpr size_t kBackstopWindowSamples = 48 * (12000 + 1410 + 1200);
        if (fed_now > anchored_burst_backstop_arm_abs_ + kBackstopWindowSamples) {
            anchored_burst_backstop_armed_ = false;
            LOG_MODEM(WARN,
                      "[%s] ANCHORED-BURST BACKSTOP: expected anchor framed no "
                      "group within the window — requesting re-confirm ack + "
                      "crater verdict",
                      log_prefix_.c_str());
            if (anchored_burst_no_group_callback_) {
                anchored_burst_no_group_callback_();
            }
        }
    }
    size_t chirp_min_search = std::min(preamble + 65000, CHIRP_MAX_SEARCH);
    bool connected_data_preamble = connected_ && waveform_->supportsDataPreamble();
    bool use_full_ofdm_anchor_search =
        connected_data_preamble && sync_controller_.expect_full_ofdm_anchor_ &&
        mode_ == protocol::WaveformMode::OFDM_CHIRP;
    // F147: each search attempt re-derives the expected-anchor mark; only the
    // connected full-anchor accept below sets it.
    last_sync_expected_full_anchor_ = false;
    bool use_light_search = connected_data_preamble && !use_full_ofdm_anchor_search;
    bool used_full_anchor_fallback = false;
    // (R4: the adaptive short-chirp re-anchor was removed — superseded by warm-handoff,
    // which is now the default. The light/full-anchor search are the only group-boundary paths.)
    //
    // Buffer sizing by search type:
    //  - light LTS: small (latency-optimized; the LTS is ~1k samples).
    //  - full dual-chirp anchor: MUST be the full chirp search size. The reduced
    //    connected window (preamble+65k ≈ 96k) is a light-LTS latency optimization and
    //    is WRONG for dual-chirp detection: when the search position lags live audio
    //    (e.g. after a burst group, BRAVO trails ~2.4s), the up-chirp lands deep in the
    //    window and the down-chirp (≈28.8k samples later) falls outside a 96k buffer →
    //    detectDualChirp reports "down NOT found" and MISSes despite a 0.99 up-chirp.
    //    This stranded the post-burst Winlink-B2F FF/turnaround frame (BUG-TNC-B2F turnaround).
    //  - disconnected chirp: the existing chirp_min_search (no lag at connect time).
    size_t min_search = use_light_search
                            ? LIGHT_SEARCH_SIZE
                            : (use_full_ofdm_anchor_search ? CHIRP_MAX_SEARCH : chirp_min_search);
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
            cfo_tracker_.tracked(), sync_result);
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
            // F147: mark this lock as an EXPECTED connected full anchor — the
            // false-lock LLR gate must not bounce it back to re-search (a
            // strong chirp match at an armed resend boundary is near-certain
            // genuine; mush LLRs there mean a faded/cold-CFO group, and the
            // group/erasure machinery — not re-search — owns that failure).
            last_sync_expected_full_anchor_ = true;
            // F165: arm the anchored-burst ack backstop — if this anchor's
            // burst frames nothing, the Connection still acks after the window.
            anchored_burst_backstop_armed_ = true;
            {
                std::lock_guard<std::mutex> abl(sync_controller_.ring_.buffer_mutex_);
                anchored_burst_backstop_arm_abs_ = sync_controller_.ring_.total_fed_;
            }
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
                min_search, is_narrowband, connected_, CORR_DETECT_THRESHOLD,
                cfo_tracker_.tracked(), sync_result);
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
        // #70: the audio BEFORE the detected up-chirp START is ambient by
        // construction — feed it to the idle-noise estimator (state is still
        // SEARCHING here, so the candidate gate accepts it). The no-lock branch
        // below only observes after a full search pass finds nothing, which
        // never happens for a station that hears the peer's probe on its very
        // first pass (fresh boot answering a handshake) — leaving the
        // noise-relative PING gap test without a floor reference exactly when
        // the responder needs it. Uses preamble_start_sample (frame start), NOT
        // start_sample (training start — the two chirps before it are full-level
        // signal and would poison the floor 2-3x high). BOOT-ONLY (hasEstimate
        // gate): in steady state the true-idle passes are the clean source; a
        // prefix can contain an undetected earlier transmission's tail, which
        // would over-read the floor and widen the false-PING window. Cap at
        // 1 s: five estimator windows, enough to go valid in one shot.
        if (sync_result.preamble_start_sample > 0 &&
            !idle_noise_snr_estimator_.hasEstimate()) {
            const size_t chirp_off = std::min(
                static_cast<size_t>(sync_result.preamble_start_sample),
                search_buffer.size());
            const size_t prefix = std::min(chirp_off, static_cast<size_t>(48000));
            observeIdleNoiseCandidate(
                search_buffer.data() + (chirp_off - prefix), prefix);
        }

        // Burst-time noise reference: in-band RMS of the silent inter-chirp gap
        // of THIS frame. Correct on constant-noise channels (real radio, OTASim)
        // AND on S:N-holding channel sims whose noise level tracks the signal
        // (idle floor != burst floor there — F224). Center 60% of the gap only:
        // the skipped leading edge doubles as FIR-transient priming and dodges
        // chirp ringing + multipath tails; the trailing edge dodges timing error.
        sync_noise_ref_rms_ = 0.0f;
        if (sync_result.interchirp_gap_len > 0 &&
            sync_result.interchirp_gap_start_sample >= 0) {
            const size_t gs = std::min(
                static_cast<size_t>(sync_result.interchirp_gap_start_sample),
                search_buffer.size());
            const size_t glen = std::min(
                static_cast<size_t>(sync_result.interchirp_gap_len),
                search_buffer.size() - gs);
            const size_t skip = glen / 5;
            if (glen > 2 * skip + 480) {  // >=10 ms usable
                FIRFilter ref_filter =
                    FIRFilter::bandpass(101, 50.0f, 2950.0f, 48000.0f);
                for (size_t i = gs; i < gs + skip; ++i) {
                    ref_filter.process(search_buffer[i]);
                }
                double sum_sq = 0.0;
                const size_t meas_end = gs + glen - skip;
                for (size_t i = gs + skip; i < meas_end; ++i) {
                    const float y = ref_filter.process(search_buffer[i]);
                    sum_sq += static_cast<double>(y) * static_cast<double>(y);
                }
                sync_noise_ref_rms_ = static_cast<float>(
                    std::sqrt(sum_sq / static_cast<double>(meas_end - gs - skip)));
                // Handoff §2: hand THIS frame's burst-time noise reference to
                // the waveform so its training-span decode computes the
                // physical channel SNR (per-frame — no stale latch).
                // ROBUSTIFIED (F231, measured on the IONOS bench): that box's
                // S:N-tracking noise BREATHES around bursts — the inter-chirp
                // gap measured 0.021-0.035 vs a steady idle floor of 0.044
                // (up to 6 dB quieter, 2x frame-to-frame swing), which made
                // the physical readout unstable there. A noise estimate can
                // read falsely LOW (tracker decay in silence) but the
                // min-statistics idle floor bounds it from below, so the
                // reference is the MAX of the two independent measurements.
                // On constant-noise channels (real radios, OTASim) gap == floor
                // and this is a no-op.
                if (waveform_) {
                    float ref = sync_noise_ref_rms_;
                    const auto ns = idle_noise_snr_estimator_.snapshot();
                    if (ns.valid && ns.floor_noise_rms > ref) {
                        ref = ns.floor_noise_rms;
                    }
                    waveform_->setNoiseReferenceRMS(ref);
                }
                LOG_MODEM(INFO,
                          "[%s] burst-noise ref: rms=%.4f gap=[%zu+%zu..%zu) of buf=%zu",
                          log_prefix_.c_str(), sync_noise_ref_rms_, gs, skip,
                          meas_end, search_buffer.size());
            }
        }

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
        // §7 C-CFO-2: the chirp-CFO drift clamp now lives on the tracker — it reads its own tracked
        // value as `known` and logs the clamp. On fading a multipath-distorted chirp can read a false
        // CFO, so the per-frame drift is clamped to the established estimate.
        // BUG-ANCHOR-CFO-KILL (2026-07-05): the CONNECTED full-anchor re-anchor is a
        // TIMING event — once the tracker is pilot-refined, its warm CFO (<0.1 Hz)
        // beats the fade-jittered chirp gap estimate (sigma 0.3-1.15 Hz), whose
        // sub-clamp phantoms killed 25% of full-anchor groups at 16QAM (0/N, all
        // frames, confident-but-rotated LLRs) vs 0% on the warm-LTS path. Cold /
        // idle / PING / MC-DPSK / narrow keep full chirp trust (other arm + the
        // pilot_seeded fallback inside).
        float new_cfo = use_full_ofdm_anchor_search
            ? cfo_tracker_.seedFromChirpConnectedAnchor(
                  sync_result.cfo_hz, sync_result.correlation, log_prefix_.c_str())
            : cfo_tracker_.seedFromChirp(sync_result.cfo_hz, sync_result.correlation,
                                         connected_, log_prefix_.c_str());
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
        stampRxSignal();  // F129: sync itself is RX evidence

        last_snr_.store(sync_snr_);
        cfo_tracker_.store(sync_cfo_);

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
