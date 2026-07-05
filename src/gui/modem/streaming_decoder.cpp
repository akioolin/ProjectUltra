// StreamingDecoder - Continuous correlation receiver
//
// Architecture:
// - feedAudio(): Audio thread writes to ring buffer (fast, no processing)
// - processBuffer(): Decode thread runs state machine
//
// State machine:
//   SEARCHING → SYNC_FOUND → DECODING → SEARCHING
//
// Continuous correlation:
// - Search with small steps (100ms) to catch chirps quickly
// - Use RMS check to skip empty sections faster
// - FFT-based correlation for speed when signal present

#include "streaming_decoder.hpp"
#include "streaming_buffer_policy.hpp"
#include "streaming_decode_policy.hpp"
#include "streaming_decoder_debug.hpp"
#include "sync/frame_arrival_policy.hpp"
#include "streaming_frame_policy.hpp"
#include "sync/signal_policy.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
#include "fec/frame_interleaver.hpp"  // Frame-level interleaving for fixed-CW frames
#include "fec/burst_interleaver.hpp"  // Burst-level long interleaver
#include "ultra/fec.hpp"              // LDPCDecoder for robust single-CW decode
#include "fec/ldpc_codec.hpp"         // getRecommendedIterations
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

namespace {

bool isFixedFrameCwCount(int cw_count) {
    return cw_count >= v2::kMinFixedFrameCodewords &&
           cw_count <= v2::kMaxFixedFrameCodewords;
}

size_t mcDpskBitsPerSymbol(const MultiCarrierDPSKConfig& config) {
    return static_cast<size_t>(std::max(1, config.num_carriers * config.bits_per_symbol));
}

// Return a conservative 1-CW control-frame sample requirement for connected OFDM.
// Coherent data profiles use coherent QPSK R1/4 control; differential data
// profiles keep DQPSK R1/4 control. Either control profile can require more
// symbols than the current data profile.
size_t getOFDMControlFrameSamples(IWaveform* waveform,
                                  Modulation data_mod,
                                  CodeRate data_rate) {
    if (!waveform) {
        return 0;
    }

    size_t default_samples = static_cast<size_t>(waveform->getMinSamplesForControlFrame());

    // Avoid waveform reconfigure here (it recreates internal DSP state and can
    // clear constellation history). Estimate robust control size analytically.
    // OFDM control is always coherent QPSK R1/4; when the data profile already
    // matches that, estimateRobustOFDMControlSamples returns the default.
    const int carriers = waveform->getCarrierCount();
    const int samples_per_symbol = waveform->getSamplesPerSymbol();
    if (carriers <= 0 || samples_per_symbol <= 0) {
        return default_samples;
    }

    return decode_policy::estimateRobustOFDMControlSamples(
        default_samples, data_mod, data_rate, carriers, samples_per_symbol);
}

}  // namespace

// ============================================================================
// DEBUG: Buffer snapshot for external analysis
// ============================================================================
// Dumps buffer contents to .f32 files at key sample counts
// Use: sox -t f32 -r 48000 -c 1 snapshot_*.f32 snapshot_*.wav
// Or: audacity can import raw 32-bit float

static void dumpBufferSnapshot(const std::vector<float>& buffer, size_t write_pos,
                                size_t total_fed, const std::string& label) {
    if (!g_debug_dumps_enabled) return;

    // Create filename with label and sample count
    char filename[256];
    snprintf(filename, sizeof(filename), "%s_%s_%zu.f32", g_dump_prefix, label.c_str(), total_fed);

    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        LOG_MODEM(WARN, "StreamingDecoder: Failed to create dump file: %s", filename);
        return;
    }

    // Dump entire buffer in linear order (unwrap circular buffer)
    // Start from oldest data (write_pos) and wrap around
    size_t buf_size = buffer.size();
    size_t valid_samples = std::min(total_fed, buf_size);

    // For simplicity, dump from position 0 to write_pos (most recent data)
    // This is what the search sees
    if (total_fed < buf_size) {
        // Buffer hasn't wrapped yet - dump from 0 to write_pos
        file.write(reinterpret_cast<const char*>(buffer.data()), write_pos * sizeof(float));
    } else {
        // Buffer wrapped - dump from write_pos to end, then 0 to write_pos
        file.write(reinterpret_cast<const char*>(buffer.data() + write_pos),
                   (buf_size - write_pos) * sizeof(float));
        file.write(reinterpret_cast<const char*>(buffer.data()),
                   write_pos * sizeof(float));
    }

    file.close();

    // Also compute and log some stats
    float rms = 0, max_val = 0;
    for (size_t i = 0; i < valid_samples && i < 10000; i++) {
        size_t idx = (total_fed < buf_size) ? i : ((write_pos + i) % buf_size);
        float s = buffer[idx];
        rms += s * s;
        max_val = std::max(max_val, std::abs(s));
    }
    rms = std::sqrt(rms / std::min(valid_samples, size_t(10000)));

    LOG_MODEM(DEBUG, "StreamingDecoder: Dumped %s: %zu samples to %s (RMS=%.4f, peak=%.4f)",
              label.c_str(), valid_samples, filename, rms, max_val);
}

StreamingDecoder::StreamingDecoder(size_t buffer_capacity_samples)
    : sync_controller_(buffer_capacity_samples) {  // owns sync_controller_.ring_; its ctor validates + sizes the buffer
    waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);
    frame_decoder_.interleaver_ = std::make_unique<ChannelInterleaver>(
        mcDpskBitsPerSymbol(mc_dpsk_config_), v2::LDPC_CODEWORD_BITS);
    frame_decoder_.codec_ = fec::CodecFactory::create(fec::CodecType::LDPC, CodeRate::R1_4);

    LOG_MODEM(INFO, "StreamingDecoder: Initialized (buffer=%zu samples)", sync_controller_.ring_.buffer_capacity_samples_);

    // DESC-SWITCH Phase 1 (docs/MODE_SWITCH_PIGGYBACK_DESIGN_2026_07_03.md §5.1, knob
    // ULTRA_DESCRIPTOR_MODE_SWITCH, read once here — lockstep with the Connection-side
    // ctor read; default OFF = byte-identical). Gates the RX mode-hop warm-handoff
    // demotion + the decoder→protocol descriptor notification.
    if (const char* ds = std::getenv("ULTRA_DESCRIPTOR_MODE_SWITCH"); ds && ds[0] == '1') {
        descriptor_mode_switch_enabled_ = true;
    }

    // §15 step 4d-late: event-driven tone-burst ACK monitor. Detection
    // runs ONLY when the protocol layer arms the monitor (via
    // armToneBurstMonitor() — invoked from Connection right after queueing
    // a data burst). Outside the armed window the monitor is idle: zero
    // audio-thread CPU, no waterfall jitter.
    //
    // Tuning:
    //   - armed_only=true: production mode (unit tests with always-on
    //     polling use the default config).
    //   - detect_interval_samples_armed = 4800 (~100 ms @ 48 kHz). At a
    //     100 ms cadence, average detection latency = burst airtime (850
    //     ms baseline) + cadence wait (~50 ms avg) + processing (~30 ms).
    //     Compare to 1592 ms measured under the previous 1.5 s polling
    //     config — ~830 ms faster per ACK round-trip.
    //   - detect_interval_samples = 0 (background polling disabled).
    //     If the protocol forgets to arm, the monitor stays idle and the
    //     existing ack_timeout retransmit path handles recovery.
    //   - Single symbol duration {25 ms} for now. The 50 ms / 100 ms
    //     low-SNR rungs are deferred until SNR-aware ACK is wired.
    //   - Coarse sweep step 32 (vs test default 8): 4× less work per
    //     detection pass with negligible accuracy loss.
    //   - Buffer capacity 90k samples (~1.9 s): one burst + margin.
    {
        ultra::waveform::tone_burst_ack::ToneBurstAckMonitor::Config tba_cfg;
        tba_cfg.armed_only = true;
        tba_cfg.detect_interval_samples = 0;          // polling off
        tba_cfg.detect_interval_samples_armed = 4800; // 100 ms when armed
        // Scan the §15.5 staircase durations the sender may emit — the ACK
        // symbol duration is now SNR-adaptive (12 ms at high SNR ... 100 ms at
        // low). Before the staircase this was 25 ms ONLY; a shorter (12 ms) ACK
        // was then invisible to this monitor -> missed ACK -> timeout retx. The
        // detector tries each in order and stops at the first CRC-passing decode,
        // so the common 25 ms case still resolves quickly.
        tba_cfg.symbol_durations_ms = {
            ultra::waveform::tone_burst_ack::kSymbolMsHighSNR,   // 12 ms (408 ms ACK)
            ultra::waveform::tone_burst_ack::kBaselineSymbolMs,  // 25 ms (850 ms baseline)
            ultra::waveform::tone_burst_ack::kSymbolMsLowSNR,    // 50 ms
            ultra::waveform::tone_burst_ack::kSymbolMsMargSNR,   // 100 ms
        };
        tba_cfg.sweep_step_samples = 32;
        // TAIL-WINDOW sweep (2026-07-04): production monitors run on constrained
        // hosts (Pi5 ARM) at a 100 ms armed cadence — per-pass cost must not scale
        // with the (now 172.8 k) buffer. See the Config comment; the always-on
        // polling tests keep the legacy whole-buffer default.
        tba_cfg.tail_window_sweep = true;
        // GAPLESS armed sweep (ULTRA_ACK_MONITOR_GAPLESS; BUG-POSTTX-ACK-MISS
        // 2026-07-05): closes the tail-window coverage hole where one large
        // feedAudio append (post-TX capture-resume backlog) exceeds a bin's tail
        // window and a tone inside it is never scanned — the rig's
        // first-ACK-after-own-keydown misses (F76/F77, ~19 s RTO each). See the
        // monitor Config comment for the induction. DEFAULT-ON 2026-07-05
        // (=0 opts out): correctness-by-construction + 10-run rig batch F78-F87
        // (0 misses) + sim parity + tone-burst tests 5/5.
        {
            const char* e = std::getenv("ULTRA_ACK_MONITOR_GAPLESS");
            tba_cfg.gapless_armed_sweep = !(e && e[0] == '0');
        }
        // Buffer DERIVED from the scan set (2026-07-04, R3/4 ACK-miss forensics):
        // it must hold one full burst at the SLOWEST scanned rung + the armed
        // detection cadence (4800) + sweep margin, or runDetectionPass silently
        // skips that rung forever (`buffer_.size() < needed -> continue`). The old
        // hand-tuned 120,000 could never hold the 100 ms rung (34×100×48=163,200)
        // — the staircase's "a conservative choice is always decodable" contract
        // was structurally false and every symbol_ms=100 ACK was missed (3/3 in
        // the forensic run: clean delivery, idle window, 5× noise floor, undecoded
        // -> full RTO resend each time). Cost of the fix: +240 KB float buffer.
        {
            uint32_t max_ms = 0;
            for (uint32_t ms : tba_cfg.symbol_durations_ms) max_ms = std::max(max_ms, ms);
            const size_t slowest_burst =
                static_cast<size_t>(ultra::waveform::tone_burst_ack::kTotalSymbols) *
                ((48000u * max_ms) / 1000u);
            tba_cfg.buffer_capacity_samples = slowest_burst + 4800 + 4800;  // 172,800
        }
        tone_burst_monitor_ =
            ultra::waveform::tone_burst_ack::ToneBurstAckMonitor(tba_cfg);
        // Install default log-only callback. Step 4d replaces.
        tone_burst_monitor_.setCallback(
            [this](const ultra::waveform::tone_burst_ack::ToneBurstAckDetection& d) {
                LOG_MODEM(INFO,
                          "[%s] ToneBurstAck monitor: detected group_seq=%u type=%s "
                          "frame_mask=0x%04X rate_hint=%u drive_advisory=%u peak=%.1f "
                          "symbol_ms=%u hamming_corrected=%d stream_offset=%llu",
                          log_prefix_.c_str(),
                          static_cast<unsigned>(d.payload.group_seq),
                          d.payload.type ==
                                  ultra::waveform::tone_burst_ack::AckType::Nack
                              ? "NACK"
                              : "ACK",
                          static_cast<unsigned>(d.payload.frame_mask),
                          static_cast<unsigned>(d.payload.rate_hint),
                          static_cast<unsigned>(d.payload.drive_advisory),
                          d.correlation_peak,
                          static_cast<unsigned>(d.symbol_ms_used),
                          d.hamming_corrected_blocks,
                          static_cast<unsigned long long>(d.detected_stream_offset));
            });
    }
}

void StreamingDecoder::setToneBurstAckCallback(ToneBurstAckCallback cb) {
    // BUG-POSTTX-ACK-MISS forensics (2026-07-05): chain a monitor-level INFO log
    // in FRONT of the production callback (which replaces the construction-time
    // log-only default). Without this, production logs show only the SR-ARQ
    // consumption line — a detection dropped upstream (dedup/epoch/guards) is
    // indistinguishable from a never-detected tone. One line per detection.
    tone_burst_monitor_.setCallback(
        [this, cb = std::move(cb)](
            const ultra::waveform::tone_burst_ack::ToneBurstAckDetection& d) {
            LOG_MODEM(INFO,
                      "[%s] ToneBurstAck monitor: detected group_seq=%u type=%s "
                      "peak=%.1f symbol_ms=%u stream_offset=%llu",
                      log_prefix_.c_str(),
                      static_cast<unsigned>(d.payload.group_seq),
                      d.payload.type ==
                              ultra::waveform::tone_burst_ack::AckType::Nack
                          ? "NACK"
                          : "ACK",
                      d.correlation_peak,
                      static_cast<unsigned>(d.symbol_ms_used),
                      static_cast<unsigned long long>(d.detected_stream_offset));
            if (cb) cb(d);
        });
}

StreamingDecoder::~StreamingDecoder() {
    stop();
}

void StreamingDecoder::setBurstInterleaveGroupSize(int size) {
    // PHANTOM-FRAME fix (2026-07-04, R3/4 ACK-miss forensics): while a group is
    // mid-collection the size in force came from THAT group's own BURST_HEADER
    // descriptor (streaming_ofdm_decode.cpp ~752) — the receiver self-describes
    // from the wire and must NOT be clobbered by a config/policy write racing in
    // between descriptor consume and group end (the DESC-SWITCH adopt did exactly
    // that: descriptor said 5, policy default 6 overwrote it 2 ms later, and the
    // demod then read 1.2 s of post-burst noise as a phantom 6th frame whose
    // noise-vs-noise "SNR" poisoned the ACK staircase; the 16QAM adopts got 9->6
    // scrambled into 0/6 deinterleave failures the same way). Defer the config
    // value to group end; the next group's descriptor overrides it anyway.
    const int sanitized = ofdm_link_adaptation::sanitizeBurstGroupSize(size);
    if (descriptor_group_size_locked_) {
        // Dropped, not stashed: every multi-frame group's own descriptor re-declares
        // the size (the config value is only the descriptor-missed fallback), so a
        // deferred config write would be overridden before it could ever matter.
        // v2 (acceptance rerun caught v1): the lock spans BURST_HEADER consume ->
        // group finalize/abort — a buffer-emptiness key missed the DESC-SWITCH
        // adopt, which fires ~2 ms after the header, before any frame collects.
        LOG_MODEM(INFO,
                  "StreamingDecoder: burst group size cfg %d IGNORED — descriptor-"
                  "declared group (%d) in flight (%zu frames held); the wire "
                  "descriptor is authoritative for the in-flight group",
                  sanitized, burst_group_size_, burst_soft_buffer_.size());
        return;
    }
    burst_group_size_ = sanitized;
}

// Descriptor-consume path (streaming_ofdm_decode.cpp BURST_HEADER): the wire value
// ALWAYS wins immediately — including over a stale partial collection (a retransmit
// group's header must re-describe the new group unconditionally) — and LOCKS the
// size until the group finalizes/aborts (every burst_soft_buffer_ clear site).
void StreamingDecoder::setBurstGroupSizeFromDescriptor(int size) {
    burst_group_size_ = ofdm_link_adaptation::sanitizeBurstGroupSize(size);
    descriptor_group_size_locked_ = true;
}

void StreamingDecoder::observeIdleNoiseCandidate(const float* samples, size_t count) {
    if (!samples || count == 0) {
        return;
    }

    // Idle classification deliberately reuses the StreamingDecoder acquisition
    // state: this is called only after SEARCHING-state audio has gone through the
    // existing chirp/LTS detector and produced no lock. If any sync/decode state
    // is active, the samples are not an idle-noise observation.
    if (state_ != DecoderState::SEARCHING ||
        pending_total_cw_ != 0 ||
        mc_burst_pending_frame_) {
        return;
    }

    idle_noise_snr_estimator_.observeIdleAudio(samples, count);
}

// §7.4 A2: the warm-sync transition logic now lives in SyncController. These three
// StreamingDecoder methods are thin forwarders, preserving their callers (the *Locked
// convention: caller holds sync_controller_.ring_.buffer_mutex_). The connected_/OFDM_CHIRP eligibility guard stays
// here (it reads decoder state the controller does not own).
void StreamingDecoder::resetFrameArrivalTrackingLocked() {
    sync_controller_.resetFrameArrivalTracking();
}

void StreamingDecoder::noteFrameArrivalSuccess(size_t frame_start_abs,
                                               size_t frame_end_abs) {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    noteFrameArrivalSuccessLocked(frame_start_abs, frame_end_abs);
}

void StreamingDecoder::noteFrameArrivalSuccessLocked(size_t frame_start_abs,
                                                     size_t frame_end_abs) {
    if (!connected_ || mode_ != protocol::WaveformMode::OFDM_CHIRP) {
        return;
    }
    sync_controller_.noteFrameArrivalSuccess(frame_start_abs, frame_end_abs);
}

void StreamingDecoder::noteFrameArrivalSyncMissLocked() {
    sync_controller_.noteFrameArrivalSyncMiss();
}

size_t StreamingDecoder::getOFDMControlFrameSamplesForCurrentMode() const {
    return getOFDMControlFrameSamples(waveform_.get(), current_modulation_, code_rate_);
}

// ============================================================================
// AUDIO THREAD - Just buffer samples, nothing else
// ============================================================================

void StreamingDecoder::feedAudio(const float* samples, size_t count) {
    if (!samples || count == 0) return;

    // §15 step 4b: feed the always-on tone-burst ACK monitor. Runs BEFORE
    // the OFDM sync_controller_.ring_.buffer_mutex_ lock so the monitor never blocks OFDM decode
    // (and the monitor has its own internal storage; no mutex needed since
    // feedAudio is called from a single audio thread). The monitor's
    // callback fires synchronously here on the audio thread — keep it
    // cheap (log-only in 4b, lightweight queue-push in 4d).
    tone_burst_monitor_.feedAudio(samples, count);

    // Audio-activity instrumentation: detect "transmission arrived" events
    // independent of the chirp-search path. We measure RMS over each incoming
    // chunk and log low->high and high->low transitions. Each transmission
    // (chirp + data) should produce one low->high then one high->low. Compare
    // these to successful sync events to know if frames are arriving at the
    // soundcard but the chirp-search isn't catching them.
    float sum_sq = 0.0f;
    for (size_t i = 0; i < count; ++i) sum_sq += samples[i] * samples[i];
    const float chunk_rms = std::sqrt(sum_sq / static_cast<float>(count));
    {
        constexpr float ACTIVITY_GATE_HIGH = 0.030f;  // signal threshold
        constexpr float ACTIVITY_GATE_LOW  = 0.010f;  // silence threshold
        bool was_active = audio_activity_.load(std::memory_order_relaxed);
        if (!was_active && chunk_rms >= ACTIVITY_GATE_HIGH) {
            audio_activity_.store(true, std::memory_order_relaxed);
            uint64_t evt = audio_activity_events_.fetch_add(1, std::memory_order_relaxed) + 1;
            LOG_MODEM(INFO, "[%s] AudioActivity #%llu: ARRIVED rms=%.3f total_fed=%.2fs",
                      log_prefix_.c_str(),
                      static_cast<unsigned long long>(evt),
                      chunk_rms,
                      sync_controller_.ring_.total_fed_ / 48000.0f);
        } else if (was_active && chunk_rms < ACTIVITY_GATE_LOW) {
            audio_activity_.store(false, std::memory_order_relaxed);
            // No log on departure to keep noise down — "arrived" events are enough.
        }
    }

    // RX operating-level AGC (ULTRA_RX_AGC, default off) — see member docs in streaming_decoder.hpp.
    // SLOW, AMPLIFY-ONLY: raises a low operating level toward the modem reference so the downstream
    // absolute gates work; a normally-leveled signal (every TX-normalized sim run) is an exact no-op.
    // Applied to the ring path only (the §15 ACK monitor above already consumed raw audio).
    static const bool kRxAgc = [] {
        const char* e = std::getenv("ULTRA_RX_AGC");
        return e && e[0] == '1';
    }();
    const float* write_samples = samples;
    std::vector<float> agc_scaled;
    if (kRxAgc) {
        constexpr float kAgcTargetRms   = 0.176f;  // measured sim active-chunk broadband operating
                                                   // level (the point the absolute gates are
                                                   // calibrated at); deadband keeps it an exact no-op
        constexpr float kAgcActivityRms = 0.030f;  // update the level estimate only when signal present
        constexpr float kAgcLevelAlpha  = 0.02f;   // slow level EMA (tracks over seconds of active audio)
        constexpr float kAgcGainSlew    = 0.03f;   // slow gain slew (never jumps within a burst)
        constexpr float kAgcEngageRatio = 3.0f;    // only amplify when level < target/3 (i.e. > 9.5 dB
                                                   // below reference) — a SEVERE deficit where the
                                                   // absolute gates genuinely break. This excludes the
                                                   // modem's lower MC-DPSK handshake operating level
                                                   // (~0.09 in sim → ratio ~1.9 < 3 → exact no-op) while
                                                   // still catching a low channel like IONOS (~0.05)
        constexpr float kAgcGainMax     = 6.0f;    // clamp correction to ~+15.6 dB
        if (chunk_rms > kAgcActivityRms) {
            agc_level_est_ =
                (1.0f - kAgcLevelAlpha) * agc_level_est_ + kAgcLevelAlpha * chunk_rms;
        }
        const float ratio = kAgcTargetRms / std::max(agc_level_est_, 1e-4f);
        const float target_gain =
            (ratio > kAgcEngageRatio) ? std::clamp(ratio, 1.0f, kAgcGainMax) : 1.0f;
        agc_gain_ = (1.0f - kAgcGainSlew) * agc_gain_ + kAgcGainSlew * target_gain;
        if (agc_gain_ > 1.001f) {
            agc_scaled.assign(samples, samples + count);
            for (float& s : agc_scaled) s *= agc_gain_;
            write_samples = agc_scaled.data();
            if ((agc_log_counter_++ % 64) == 0) {
                LOG_MODEM(INFO, "[%s] RX-AGC engaged: level_est=%.4f gain=%.2f (+%.1f dB)",
                          log_prefix_.c_str(), agc_level_est_, agc_gain_,
                          20.0f * std::log10(std::max(agc_gain_, 1e-6f)));
            }
        }
    }

    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);

    size_t prev_total = sync_controller_.ring_.total_fed_;

    auto overflow = buffer_policy::planOverflowRecovery(
        sync_controller_.ring_.write_pos_, sync_controller_.ring_.correlation_pos_, sync_controller_.ring_.total_fed_, count, sync_controller_.ring_.buffer_capacity_samples_,
        CORR_INVARIANT_GUARD, OVERFLOW_RECOVERY_KEEP);

    if (overflow.pointer_drift_detected) {
        sync_controller_.ring_.correlation_pos_ = sync_controller_.ring_.write_pos_;
        sync_controller_.ring_.setSearchFloorLocked(sync_controller_.ring_.total_fed_);
        LOG_MODEM(WARN, "[%s] Correlation pointer drift detected, resetting search cursor",
                  log_prefix_.c_str());
    }

    // If adding these samples would overflow, drop backlog aggressively enough
    // to recover in one step. Small drops (~1k) cause repeated overflow storms.
    if (overflow.overflow) {
        sync_controller_.ring_.correlation_pos_ = overflow.new_correlation_pos;
        sync_controller_.ring_.setSearchFloorLocked(sync_controller_.ring_.ringPosToAbsoluteLocked(sync_controller_.ring_.correlation_pos_));

        // Once overloaded, any in-flight frame context is stale. Force a clean
        // resync from current audio instead of chasing old sync positions.
        bool reset_decode_state = false;
        if (state_ != DecoderState::SEARCHING) {
            state_ = DecoderState::SEARCHING;
            pending_total_cw_ = 0;
            burst_blocks_decoded_ = 0;
            burst_soft_buffer_.clear();
            descriptor_group_size_locked_ = false;  // group ended/aborted — cfg writes may apply again
            burst_metric_templates_.clear();
            mc_burst_pending_frame_ = false;
            mc_burst_pending_soft_bits_.clear();
            reset_decode_state = true;
        }

        overflow_events_++;
        {
            std::lock_guard<std::mutex> slock(stats_mutex_);
            stats_.buffer_overflows = overflow_events_;
            stats_.overflow_samples_dropped += overflow.samples_to_drop;
            if (reset_decode_state) {
                stats_.overflow_state_resets++;
            }
        }
        if (overflow_events_ <= 3 || (overflow_events_ % 25) == 0) {
            LOG_MODEM(WARN, "[%s] Buffer overflow, dropped %zu unsearched samples (corr_pos=%zu, keep=%zu, state_reset=%d, total=%llu)",
                      log_prefix_.c_str(), overflow.samples_to_drop, sync_controller_.ring_.correlation_pos_,
                      overflow.target_after_write,
                      reset_decode_state ? 1 : 0,
                      static_cast<unsigned long long>(overflow_events_));
        }
    }

    sync_controller_.ring_.writeSamplesToRingLocked(write_samples, count);

    sync_controller_.ring_.total_fed_ += count;

    // Update backlog telemetry for UI/CLI diagnostics.
    const auto backlog = buffer_policy::computeBacklog(
        sync_controller_.ring_.write_pos_, sync_controller_.ring_.correlation_pos_, sync_controller_.ring_.total_fed_, sync_controller_.ring_.buffer_capacity_samples_, 48000.0f);
    {
        std::lock_guard<std::mutex> slock(stats_mutex_);
        stats_.current_unsearched_samples = backlog.unsearched_samples;
        stats_.peak_unsearched_samples = std::max<uint64_t>(stats_.peak_unsearched_samples,
                                                            static_cast<uint64_t>(backlog.unsearched_samples));
        stats_.backlog_ms = backlog.backlog_ms;
        stats_.peak_backlog_ms = std::max(stats_.peak_backlog_ms, backlog.backlog_ms);
        stats_.buffer_fill_percent = backlog.fill_percent;
    }

    // DEBUG: Dump buffer snapshots at key sample counts
    // These help analyze what audio is actually in the buffer
    // Chirp structure: 7200 lead-in + 24000 up + 4800 gap + 24000 down + 4800 trail = 64800
    // So we should have full chirp around 72000 samples (64800 + margin)
    auto crossedThreshold = [prev_total, this](size_t threshold) {
        return prev_total < threshold && sync_controller_.ring_.total_fed_ >= threshold;
    };

    if (crossedThreshold(24000)) {
        dumpBufferSnapshot(sync_controller_.ring_.buffer_, sync_controller_.ring_.write_pos_, sync_controller_.ring_.total_fed_, "early_24k");
    }
    if (crossedThreshold(48000)) {
        dumpBufferSnapshot(sync_controller_.ring_.buffer_, sync_controller_.ring_.write_pos_, sync_controller_.ring_.total_fed_, "mid_48k");
    }
    if (crossedThreshold(72000)) {
        dumpBufferSnapshot(sync_controller_.ring_.buffer_, sync_controller_.ring_.write_pos_, sync_controller_.ring_.total_fed_, "full_chirp_72k");
    }
    if (crossedThreshold(96000)) {
        dumpBufferSnapshot(sync_controller_.ring_.buffer_, sync_controller_.ring_.write_pos_, sync_controller_.ring_.total_fed_, "late_96k");
    }

    // Log at key thresholds to track audio arrival
    float audio_sec = sync_controller_.ring_.total_fed_ / 48000.0f;
    auto crossedSecond = [prev_total, this](float sec) {
        size_t threshold = static_cast<size_t>(sec * 48000);
        return prev_total < threshold && sync_controller_.ring_.total_fed_ >= threshold;
    };

    // Log every 0.5 seconds of audio
    if (crossedSecond(0.5f) || crossedSecond(1.0f) || crossedSecond(1.5f) ||
        crossedSecond(2.0f) || crossedSecond(2.5f) || crossedSecond(3.0f)) {
        float rms = 0.0f;
        for (size_t i = 0; i < count; i++) rms += samples[i] * samples[i];
        rms = std::sqrt(rms / count);
        LOG_MODEM(INFO, "[%s] feed: audio=%.2fs, RMS=%.4f",
                  log_prefix_.c_str(), audio_sec, rms);
    }

    // Wake up decode thread
    new_data_available_ = true;
    sync_controller_.ring_.data_cv_.notify_one();
}

// ============================================================================
// DECODE THREAD - State machine for continuous correlation
// ============================================================================

void StreamingDecoder::processBuffer() {
    // Wait for new data or timeout
    bool woke_with_data = false;
    {
        std::unique_lock<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
        woke_with_data = sync_controller_.ring_.data_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
            return shutdown_.load() || new_data_available_;
        });
        if (shutdown_.load()) return;
        if (!woke_with_data) return;  // Timeout with no new audio; avoid stale re-search churn
        new_data_available_ = false;  // Clear flag after waking
        // §14.36: apply pending waveform changes BEFORE any decode work in
        // this processBuffer iteration. The actual configure() / waveform_
        // reconstruction (replaces modulator_/demodulator_/chirp_sync_
        // unique_ptrs) runs here at a clean boundary — never mid-decode.
        // Connected-OFDM-mode rebuild MUST run before descriptor-driven rate
        // change (it constructs the waveform_ that the rate change then
        // reconfigures). Both routes through deferred pending so RX never
        // crashes in HilbertTransform::process when ALPHA adapts its rate.
        applyPendingConnectedOFDMMode();
        applyPendingDescriptorDataMode();
    }

    // ===== [RXLAG-DIAG] TEMPORARY RX-processing-lag instrumentation (ULTRA_RX_LAG_DIAG=1) =====
    // Measures how far the decode/search position trails LIVE audio — the ~2.4s post-burst backlog
    // the SEARCHING comment below describes, suspected to be the bulk of the rig's ~3.1s turnaround
    // (vs OTASim's ~0.8s). backlog = total_fed_ - search position, in seconds. Rate-limited ~1/s.
    // REMOVE by deleting this whole block (grep RXLAG-DIAG). No behavior change when the env is unset.
    {
        static const bool kRxLagDiag = [] {
            const char* e = std::getenv("ULTRA_RX_LAG_DIAG");
            return e && e[0] == '1';
        }();
        if (kRxLagDiag) {
            size_t fed = 0, corr_abs = 0;
            {
                std::lock_guard<std::mutex> lk(sync_controller_.ring_.buffer_mutex_);
                fed = sync_controller_.ring_.total_fed_;
                corr_abs = sync_controller_.ring_.ringPosToAbsoluteLocked(
                    sync_controller_.ring_.correlation_pos_);
            }
            const double backlog_s = (fed > corr_abs ? fed - corr_abs : 0) / 48000.0;
            static const auto t0 = std::chrono::steady_clock::now();
            const double wall_s = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
            static double last_log_s = -1e9;
            if (wall_s - last_log_s >= 1.0) {
                last_log_s = wall_s;
                const char* st =
                    state_ == DecoderState::SEARCHING ? "SEARCH" :
                    state_ == DecoderState::SYNC_FOUND ? "SYNC" :
                    state_ == DecoderState::DECODING ? "DECODE" :
                    state_ == DecoderState::BURST_ACCUMULATING ? "BURST_ACC" : "MCDPSK_CONT";
                // Also surface the modem's OWN backlog metric (the GUI "RXQ" value): it is sampled
                // at feedAudio cadence (every chunk), so its running PEAK captures transient spikes my
                // 1/s [RXLAG] sampling misses (the user-observed ~40s RXQ peak even on a good run).
                float official_ms = 0.0f, peak_ms = 0.0f;
                {
                    std::lock_guard<std::mutex> slock(stats_mutex_);
                    official_ms = stats_.backlog_ms;
                    peak_ms = stats_.peak_backlog_ms;
                }
                LOG_MODEM(INFO,
                          "[RXLAG] wall=%.1fs audio_fed=%.1fs backlog=%.2fs state=%s "
                          "RXQ=%.0fms RXQ_peak=%.0fms",
                          wall_s, fed / 48000.0, backlog_s, st, official_ms, peak_ms);
            }
        }
    }
    // ===== [RXLAG-DIAG] end =====

    switch (state_) {
        case DecoderState::SEARCHING: {
            // SEARCH CATCH-UP DRAIN. processBuffer runs once per audio-chunk wake-up
            // (single new_data_available_ flag) and a plain searchForSync() advances
            // correlation_pos_ by one correlation_step (~100 ms). Since audio also
            // arrives at ~real time, the search position advances at the SAME rate as
            // incoming audio: once it falls behind it never catches up. After a burst
            // it trails live audio by ~2.4 s, so a post-burst full-anchor frame's
            // up-chirp lands at the search window's trailing edge BEFORE its down-chirp
            // has been received → detectDualChirp reports "down NOT found" and MISSes
            // (stranded the Winlink-B2F FF/turnaround frame). Drain the unsearched
            // backlog by stepping repeatedly this wake-up until the search is tracking
            // live audio again (searchForSync self-limits at min_search unsearched
            // samples, so backlog stops shrinking at the structural floor) or sync is
            // found. Warm-sync (normal in-burst group boundaries) keeps the backlog
            // small, so this loop is a no-op there and does not change burst behavior.
            constexpr int kMaxSearchCatchupSteps = 64;
            size_t prev_backlog = SIZE_MAX;
            for (int step = 0; step < kMaxSearchCatchupSteps; ++step) {
                searchForSync();
                if (state_ != DecoderState::SEARCHING) break;  // sync acquired
                size_t backlog;
                {
                    std::lock_guard<std::mutex> lk(sync_controller_.ring_.buffer_mutex_);
                    const size_t corr_abs = sync_controller_.ring_.ringPosToAbsoluteLocked(
                        sync_controller_.ring_.correlation_pos_);
                    backlog = sync_controller_.ring_.total_fed_ > corr_abs
                                  ? sync_controller_.ring_.total_fed_ - corr_abs
                                  : 0;
                }
                // No further progress (reached the structural floor or waiting for
                // more samples) → done draining for this wake-up.
                if (backlog >= prev_backlog) break;
                prev_backlog = backlog;
            }
            break;
        }

        case DecoderState::SYNC_FOUND:
            checkIfReadyToDecode();
            break;

        case DecoderState::DECODING:
            decodeCurrentFrame();
            break;

        case DecoderState::BURST_ACCUMULATING:
            accumulateBurstFrames();
            break;

        case DecoderState::MCDPSK_BURST_CONTINUING:
            continueMCDPSKBurst();
            break;
    }
}

bool StreamingDecoder::hasFrame() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return !frame_queue_.empty();
}

DecodeResult StreamingDecoder::getFrame() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (frame_queue_.empty()) return DecodeResult{};
    DecodeResult r = std::move(frame_queue_.front());
    frame_queue_.pop();
    return r;
}

// ============================================================================
// MODE CONTROL
// ============================================================================

void StreamingDecoder::setMode(protocol::WaveformMode mode, bool connected) {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);

    if (mode_ == mode && connected_ == connected) return;

    const bool waveform_mode_changed = mode_ != mode;
    mode_ = mode;
    connected_ = connected;

    // Disconnect backstop for the burst BURST_HEADER z-latch
    // (sync_controller_.have_burst_descriptor_ / activeBurstLiftingZ). 2026-06-05
    // (BUG-TNC-B2F-002): the latch is now cleared PER GROUP at group-end
    // (finalizeBurstGroup, streaming_burst_interleave.cpp) — the correct z lifecycle
    // is default z=27, lift on BURST_HEADER, drop at group-end (each next group's
    // BURST_HEADER re-sets it; set at streaming_ofdm_decode.cpp:762). The old
    // whole-connection persistence wrongly kept post-burst non-burst frames (the
    // Winlink-B2F FF terminator, any interactive frame after a file transfer) gated
    // as mid-burst and mis-sized as z=81. This `!connected` clear remains as a
    // backstop for a session that ends before a group finalizes.
    if (!connected) {
        sync_controller_.have_burst_descriptor_ = false;
    }

    if (waveform_mode_changed) {
        if (mode == protocol::WaveformMode::MC_DPSK) {
            waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);
        } else {
            waveform_ = WaveformFactory::create(mode);
        }
        if (waveform_) {
            waveform_->setCarrierMask(carrier_mask_);
        }

        size_t bps = mcDpskBitsPerSymbol(mc_dpsk_config_);
        if (protocol::isOFDMMode(mode)) {
            bps = 60;
        }
        // Z-aware interleaver block size from the active burst descriptor's
        // lifting_z (single RX source of truth — activeBurstLiftingZ()); falls
        // back to 648 when no descriptor has been seen yet (cold start).
        const size_t ldpc_codeword_bits_ci =
            (activeBurstLiftingZ() == 81) ? size_t{1944} : v2::LDPC_CODEWORD_BITS;
        frame_decoder_.interleaver_ = std::make_unique<ChannelInterleaver>(bps, ldpc_codeword_bits_ci);
    }

    state_ = DecoderState::SEARCHING;
    pending_total_cw_ = 0;
    sync_controller_.clearRejectStreak();
    sync_controller_.expect_full_ofdm_anchor_ = false;
    sync_from_warm_timed_window_ = false;
    resetFrameArrivalTrackingLocked();
    constellation_cache_.clear();
    constellation_cache_time_ = std::chrono::steady_clock::time_point{};

    // Clear burst interleave state on mode change
    burst_soft_buffer_.clear();
    descriptor_group_size_locked_ = false;  // group ended/aborted — cfg writes may apply again
    burst_metric_templates_.clear();
    mc_burst_pending_frame_ = false;
    mc_burst_pending_soft_bits_.clear();
    use_burst_interleave_ = false;  // Re-enabled by caller if needed

    // CRITICAL: Reset sync_controller_.ring_.correlation_pos_ to current write position
    // Otherwise we'll search old data from previous mode
    sync_controller_.ring_.correlation_pos_ = sync_controller_.ring_.write_pos_;
    sync_controller_.ring_.setSearchFloorLocked(sync_controller_.ring_.total_fed_);

    LOG_MODEM(INFO, "StreamingDecoder: Mode=%s (%s), reset corr_pos=%zu",
              protocol::waveformModeToString(mode), connected ? "connected" : "disconnected", sync_controller_.ring_.correlation_pos_);
}

void StreamingDecoder::setCarrierMask(uint64_t active_mask) {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    carrier_mask_ = active_mask;
    if (waveform_) {
        waveform_->setCarrierMask(active_mask);
    }
}

void StreamingDecoder::setCarrierLdpcInterleaver(bool enable) {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    use_carrier_ldpc_interleaver_ = enable;
    if (waveform_) {
        waveform_->setCarrierLdpcInterleaverEnabled(enable);
    }
}

void StreamingDecoder::expectFullOFDMAnchorOnce() {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    if (!connected_ || mode_ != protocol::WaveformMode::OFDM_CHIRP) {
        sync_controller_.expect_full_ofdm_anchor_ = false;
        return;
    }

    sync_controller_.expect_full_ofdm_anchor_ = true;
    sync_controller_.clearRejectStreak();
    LOG_MODEM(INFO, "StreamingDecoder: expecting full OFDM chirp+LTS timing anchor");
}

void StreamingDecoder::clearFullOFDMAnchorExpectation() {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    if (sync_controller_.expect_full_ofdm_anchor_) {
        LOG_MODEM(INFO, "StreamingDecoder: clearing pending full OFDM DATA anchor");
    }
    sync_controller_.expect_full_ofdm_anchor_ = false;
}

bool StreamingDecoder::expectsFullOFDMAnchorForTesting() const {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    return sync_controller_.expect_full_ofdm_anchor_;
}

void StreamingDecoder::applyPendingConfigForTesting() {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    applyPendingConnectedOFDMMode();
    applyPendingDescriptorDataMode();
}

void StreamingDecoder::setMCDPSKCarriers(int n) {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    if (mc_dpsk_carriers_ == n) return;
    mc_dpsk_carriers_ = n;
    mc_dpsk_config_.num_carriers = n;
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);
        frame_decoder_.interleaver_ = std::make_unique<ChannelInterleaver>(
            mcDpskBitsPerSymbol(mc_dpsk_config_), v2::LDPC_CODEWORD_BITS);
    }
}

void StreamingDecoder::setMCDPSKConfig(const MultiCarrierDPSKConfig& config) {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    const bool changed =
        mc_dpsk_config_.num_carriers != config.num_carriers ||
        mc_dpsk_config_.samples_per_symbol != config.samples_per_symbol ||
        mc_dpsk_config_.bits_per_symbol != config.bits_per_symbol ||
        mc_dpsk_config_.freq_low != config.freq_low ||
        mc_dpsk_config_.freq_high != config.freq_high ||
        mc_dpsk_config_.chirp_f_start != config.chirp_f_start ||
        mc_dpsk_config_.chirp_f_end != config.chirp_f_end ||
        mc_dpsk_config_.chirp_duration_ms != config.chirp_duration_ms ||
        mc_dpsk_config_.use_dual_chirp != config.use_dual_chirp ||
        mc_dpsk_config_.track_clock_offset != config.track_clock_offset;
    if (!changed) return;

    mc_dpsk_config_ = config;
    mc_dpsk_carriers_ = config.num_carriers;
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);
        frame_decoder_.interleaver_ = std::make_unique<ChannelInterleaver>(
            mcDpskBitsPerSymbol(mc_dpsk_config_), v2::LDPC_CODEWORD_BITS);
    }

    LOG_MODEM(INFO, "StreamingDecoder: MC-DPSK config carriers=%d sps=%d bits/sym=%d raw=%.1f bps",
              mc_dpsk_config_.num_carriers, mc_dpsk_config_.samples_per_symbol,
              mc_dpsk_config_.bits_per_symbol, mc_dpsk_config_.getRawBitRate());
}

void StreamingDecoder::setOFDMConfig(const ModemConfig& config) {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);

    // Store carrier counts regardless of current mode (used when switching to OFDM later)
    ofdm_carriers_ = config.num_carriers;
    ofdm_data_carriers_ = ofdm_link_adaptation::dataCarrierCount(
        static_cast<int>(config.num_carriers),
        config.use_pilots,
        static_cast<int>(config.pilot_spacing));

    // Only recreate waveform if currently in an OFDM mode.
    // When in MC-DPSK mode (e.g., disconnected waiting for PINGs), do NOT replace
    // the MC-DPSK waveform — that would break chirp-based sync detection.
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        LOG_MODEM(INFO, "StreamingDecoder: OFDM config stored (mode=MC-DPSK, not replacing waveform)");
        return;
    }

    if (mode_ == protocol::WaveformMode::OFDM_CHIRP) {
        waveform_ = std::make_unique<OFDMChirpWaveform>(config);
        LOG_MODEM(INFO, "StreamingDecoder: OFDM_CHIRP config set (FFT=%d, carriers=%d)",
                  config.fft_size, config.num_carriers);
    } else if (mode_ == protocol::WaveformMode::OFDM_NARROW) {
        waveform_ = std::make_unique<OFDMChirpWaveform>(config, protocol::WaveformMode::OFDM_NARROW);
        LOG_MODEM(INFO, "StreamingDecoder: OFDM_NARROW config set (FFT=%d, carriers=%d)",
                  config.fft_size, config.num_carriers);
    } else {
        LOG_MODEM(WARN, "StreamingDecoder: unexpected OFDM mode %d; defaulting to OFDM-CHIRP",
                  static_cast<int>(mode_));
        waveform_ = std::make_unique<OFDMChirpWaveform>(config);
    }
    if (waveform_) {
        waveform_->setCarrierMask(carrier_mask_);
    }

    // Update interleaver for new carrier count (using current modulation)
    size_t bps = static_cast<size_t>(ofdm_data_carriers_) * getBitsPerSymbol(current_modulation_);
    frame_decoder_.interleaver_ = std::make_unique<ChannelInterleaver>(bps, v2::LDPC_CODEWORD_BITS);
}

void StreamingDecoder::setConnectedOFDMMode(protocol::WaveformMode mode,
                                            const ModemConfig& config,
                                            Modulation mod,
                                            CodeRate rate) {
    // §14.36 crash fix v3 (2026-05-28): defer the waveform_ reconstruction
    // to the safe top-of-processBuffer boundary. Inline construction races
    // the RX thread (which releases sync_controller_.ring_.buffer_mutex_ before searchForSync) and
    // SIGSEGVs in HilbertTransform::process — same root cause as v2 setDataMode.
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    pending_connected_ofdm_mode_ = mode;
    pending_connected_ofdm_config_ = config;
    pending_connected_ofdm_mod_ = mod;
    pending_connected_ofdm_rate_ = rate;
    pending_connected_ofdm_change_ = true;
}

void StreamingDecoder::applyPendingConnectedOFDMMode() {
    // Caller already holds sync_controller_.ring_.buffer_mutex_ and we are at the safe boundary
    // (top of processBuffer, before any decode work in this iteration).
    if (!pending_connected_ofdm_change_) return;
    pending_connected_ofdm_change_ = false;
    const protocol::WaveformMode mode = pending_connected_ofdm_mode_;
    const ModemConfig& config = pending_connected_ofdm_config_;
    const Modulation mod = pending_connected_ofdm_mod_;
    const CodeRate rate = pending_connected_ofdm_rate_;

    const bool preserve_full_anchor =
        sync_controller_.expect_full_ofdm_anchor_ || mode == protocol::WaveformMode::OFDM_CHIRP;

    mode_ = mode;
    connected_ = true;
    code_rate_ = rate;
    current_modulation_ = mod;
    ofdm_carriers_ = config.num_carriers;
    ofdm_data_carriers_ = ofdm_link_adaptation::dataCarrierCount(
        static_cast<int>(config.num_carriers),
        config.use_pilots,
        static_cast<int>(config.pilot_spacing));

    if (mode_ == protocol::WaveformMode::OFDM_CHIRP) {
        waveform_ = std::make_unique<OFDMChirpWaveform>(config);
    } else if (mode_ == protocol::WaveformMode::OFDM_NARROW) {
        waveform_ = std::make_unique<OFDMChirpWaveform>(config, protocol::WaveformMode::OFDM_NARROW);
    } else {
        waveform_ = std::make_unique<OFDMChirpWaveform>(config);
    }

    if (waveform_) {
        waveform_->configure(mod, rate);
        waveform_->setCarrierMask(carrier_mask_);
    }

    // Query waveform for effective pilot layout after configure().
    int pilot_spacing = waveform_ ? waveform_->getPilotSpacing() : 0;
    ofdm_data_carriers_ = ofdm_link_adaptation::dataCarrierCount(
        ofdm_carriers_, pilot_spacing > 0, pilot_spacing);

    size_t bps = static_cast<size_t>(ofdm_link_adaptation::bitsPerOFDMSymbol(
        ofdm_carriers_, pilot_spacing > 0, pilot_spacing, mod));
    frame_decoder_.interleaver_ = std::make_unique<ChannelInterleaver>(bps, v2::LDPC_CODEWORD_BITS);

    state_ = DecoderState::SEARCHING;
    pending_total_cw_ = 0;
    sync_controller_.clearRejectStreak();
    sync_controller_.expect_full_ofdm_anchor_ = preserve_full_anchor;
    sync_from_warm_timed_window_ = false;
    resetFrameArrivalTrackingLocked();
    constellation_cache_.clear();
    constellation_cache_time_ = std::chrono::steady_clock::time_point{};
    mc_burst_pending_frame_ = false;
    mc_burst_pending_soft_bits_.clear();
    // MODE-HOP CURSOR FIX (2026-07-04, 16QAM zero-decode forensics): when this
    // reconfig is the DESCRIPTOR-ADOPT's redundant second reset (a wire-declared
    // group is IN FLIGHT — descriptor_group_size_locked_), the BURST_HEADER
    // consume already parked correlation_pos_/search-floor EXACTLY at the group
    // anchor's up-chirp start. Overwriting the cursor with write_pos_/total_fed_
    // here beheads that anchor by the decode lag (~9.7 k samples measured), after
    // which the 120 k chirp-FFT quota STARVES in equilibrium against the light
    // search's 4800-per-run shared-cursor advance, and first-group arming
    // degenerates to the stochastic decayed-threshold fallback (the 18 s no-ACK
    // RTO the operator watched; byte-exact counterfactual: cursor preserved ->
    // the FFT runs at the next quota poll with its window starting AT the
    // up-chirp). Preserve cursor + in-flight group state; KEEP the waveform
    // rebuild, COLD demotion and expect_full_ofdm_anchor_ semantics. Legacy-path
    // reconfigs (half-duplex turnaround, receiver idle, no group in flight) keep
    // the full reset — there the cursor jump is harmless by construction.
    if (!descriptor_group_size_locked_) {
        burst_soft_buffer_.clear();
        burst_metric_templates_.clear();
        sync_controller_.ring_.correlation_pos_ = sync_controller_.ring_.write_pos_;
        sync_controller_.ring_.setSearchFloorLocked(sync_controller_.ring_.total_fed_);
    } else {
        LOG_MODEM(INFO,
                  "StreamingDecoder: connected-OFDM reconfig with descriptor group in "
                  "flight — search cursor/floor and group state PRESERVED (mode-hop)");
    }

    LOG_MODEM(INFO, "StreamingDecoder: connected OFDM mode=%s, mod=%s, rate=%s, carriers=%d data=%d bps=%zu",
              protocol::waveformModeToString(mode_),
              modulationToString(mod), codeRateToString(rate),
              ofdm_carriers_, ofdm_data_carriers_, bps);
    if (sync_controller_.expect_full_ofdm_anchor_) {
        LOG_MODEM(INFO, "StreamingDecoder: connected OFDM config armed full chirp+LTS timing anchor");
    }
}

void StreamingDecoder::setDataMode(Modulation mod, CodeRate rate) {
    // §14.36 crash fix v2 (2026-05-28): processBuffer RELEASES sync_controller_.ring_.buffer_mutex_
    // before calling searchForSync (line 493). If we apply configure() here
    // (which replaces modulator_/demodulator_/chirp_sync_ unique_ptrs), the
    // RX thread mid-searchForSync runs with stale pointers -> SIGSEGV in
    // HilbertTransform::process (ultra_gui-2026-05-28-005140.ips, alpha
    // adaptive R3/4->R2/3 at 45.138s, crashed at 45.142s).
    //
    // Route ALL configure() calls through the defer-pending channel — the
    // actual configure() runs at the TOP of processBuffer (post-wait, before
    // any decode work). Same channel the BURST_HEADER intercept uses; setting
    // the same fields from setDataMode is correct (last write wins, the
    // controller's latest decision is what we want for the next iteration).
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    pending_descriptor_mod_ = mod;
    pending_descriptor_rate_ = rate;
    pending_descriptor_rate_change_ = true;
}

void StreamingDecoder::applyPendingDescriptorDataMode() {
    // Caller already holds sync_controller_.ring_.buffer_mutex_. Apply the deferred descriptor-driven
    // rate switch, if any (set by the BURST_HEADER intercept). This runs at the
    // top of processBuffer — between iterations — so the configure() can swap
    // modulator_/demodulator_/chirp_sync_ unique_ptrs without any concurrent
    // decode work using them. Idempotent + no-op when nothing's pending.
    if (!pending_descriptor_rate_change_) return;
    pending_descriptor_rate_change_ = false;
    const Modulation mod = pending_descriptor_mod_;
    const CodeRate rate = pending_descriptor_rate_;
    if (mod == current_modulation_ && rate == code_rate_) {
        return;  // already matches, nothing to do
    }
    LOG_MODEM(INFO,
              "StreamingDecoder: applying deferred descriptor rate change %s %s -> %s %s",
              modulationToString(current_modulation_), codeRateToString(code_rate_),
              modulationToString(mod), codeRateToString(rate));
    applyDataModeUnlocked(mod, rate);
}

void StreamingDecoder::applyDataModeUnlocked(Modulation mod, CodeRate rate) {
    // §14.36: callable from contexts that already hold sync_controller_.ring_.buffer_mutex_ (e.g. the
    // BURST_HEADER intercept inside processBuffer), so the receiver can switch
    // rate from the descriptor's declaration mid-transfer.
    code_rate_ = rate;
    current_modulation_ = mod;
    if (waveform_) waveform_->configure(mod, rate);

    // After configure(), the waveform has updated pilot config
    // Query waveform for actual pilot_spacing (coherent modes use denser pilots)
    if (mode_ != protocol::WaveformMode::MC_DPSK && waveform_) {
        int pilot_spacing = waveform_->getPilotSpacing();
        ofdm_data_carriers_ = ofdm_link_adaptation::dataCarrierCount(
            ofdm_carriers_, pilot_spacing > 0, pilot_spacing);
    }

    // Update interleaver for new modulation
    // Use data carriers (not total) to account for pilot overhead
    int carriers = (mode_ == protocol::WaveformMode::MC_DPSK) ? mc_dpsk_carriers_ : ofdm_data_carriers_;
    size_t bps = static_cast<size_t>(carriers) * getBitsPerSymbol(mod);
    frame_decoder_.interleaver_ = std::make_unique<ChannelInterleaver>(bps, v2::LDPC_CODEWORD_BITS);
    LOG_MODEM(INFO, "StreamingDecoder: interleaver updated for %s (%zu bits/symbol)",
              modulationToString(mod), bps);
}

void StreamingDecoder::setCodecType(fec::CodecType type) {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    if (codec_type_ == type) return;
    codec_type_ = type;
    frame_decoder_.codec_ = fec::CodecFactory::create(type, code_rate_);
}

// ============================================================================
// STATUS
// ============================================================================

float StreamingDecoder::getBufferFillPercent() const {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    size_t used = std::min(sync_controller_.ring_.total_fed_, sync_controller_.ring_.buffer_capacity_samples_);
    return 100.0f * used / sync_controller_.ring_.buffer_capacity_samples_;
}

DecoderStats StreamingDecoder::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

StreamingDecoder::FrameArrivalSnapshot StreamingDecoder::getFrameArrivalSnapshot() const {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);

    FrameArrivalSnapshot snapshot;
    snapshot.warm_sync_active = sync_controller_.warmSyncActive();
    snapshot.warm_sync_phase = sync_controller_.derivePhase();
    snapshot.has_prediction = sync_controller_.next_expected_frame_sample_valid_;
    snapshot.next_expected_frame_sample = sync_controller_.next_expected_frame_sample_;
    snapshot.frame_arrival_confidence = sync_controller_.frameArrivalConfidence();
    snapshot.consecutive_sync_misses = sync_controller_.consecutiveSyncMisses();
    snapshot.has_last_frame = sync_controller_.lastFrameArrivalValid();
    snapshot.last_frame_start_sample = sync_controller_.lastFrameStartSample();
    snapshot.last_frame_end_sample = sync_controller_.lastFrameEndSample();
    snapshot.has_last_arrival_error = sync_controller_.lastFrameArrivalErrorValid();
    snapshot.last_arrival_error_samples = sync_controller_.lastFrameArrivalErrorSamples();
    snapshot.expected_frame_gap_samples = sync_controller_.expectedFrameGapSamples();
    return snapshot;
}

void StreamingDecoder::setExpectedFrameGapSamples(size_t samples) {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    sync_controller_.setExpectedFrameGapSamples(samples);
}

void StreamingDecoder::seedExpectedFrameArrivalAfterSamples(size_t delay_samples,
                                                            float confidence) {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    if (!connected_ || mode_ != protocol::WaveformMode::OFDM_CHIRP) {
        return;
    }
    sync_controller_.seedArrivalAfterDelay(sync_controller_.ring_.total_fed_, delay_samples, confidence);
}


StreamingDecoder::DecoderConfig StreamingDecoder::getConfig() const {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);

    DecoderConfig cfg;
    cfg.mode = mode_;
    cfg.modulation = current_modulation_;
    cfg.code_rate = code_rate_;
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        cfg.num_carriers = mc_dpsk_config_.num_carriers;
        cfg.data_carriers = mc_dpsk_config_.num_carriers;
        cfg.bits_per_symbol = static_cast<int>(mcDpskBitsPerSymbol(mc_dpsk_config_));
    } else {
        cfg.num_carriers = ofdm_carriers_;
        cfg.data_carriers = ofdm_data_carriers_;
        cfg.bits_per_symbol = ofdm_data_carriers_ * static_cast<int>(getBitsPerSymbol(current_modulation_));
    }

    // Get pilot config from waveform (coherent modes use denser pilots)
    cfg.use_pilots = true;
    cfg.pilot_spacing = waveform_ ? waveform_->getPilotSpacing() : 10;

    // Interleaving settings
    cfg.use_channel_interleave = use_channel_interleave_;
    cfg.use_frame_interleave = protocol::isOFDMMode(mode_);

    return cfg;
}

size_t StreamingDecoder::samplesInBuffer() const {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    return std::min(sync_controller_.ring_.total_fed_, sync_controller_.ring_.buffer_capacity_samples_);
}

bool StreamingDecoder::isSynced() const {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    return state_ == DecoderState::SYNC_FOUND || state_ == DecoderState::DECODING
        || state_ == DecoderState::BURST_ACCUMULATING
        || state_ == DecoderState::MCDPSK_BURST_CONTINUING;
}

void StreamingDecoder::captureConstellationSnapshot() {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    if (!waveform_) {
        return;
    }

    auto symbols = waveform_->getConstellationSymbols();
    if (!symbols.empty()) {
        constellation_cache_ = std::move(symbols);
        constellation_cache_time_ = std::chrono::steady_clock::now();
    }
}

std::vector<std::complex<float>> StreamingDecoder::getConstellationSymbols() const {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    auto now = std::chrono::steady_clock::now();

    if (waveform_) {
        auto symbols = waveform_->getConstellationSymbols();
        if (!symbols.empty()) {
            constellation_cache_ = symbols;
            constellation_cache_time_ = now;
        }
        static int log_count = 0;
        if (log_count < 10 && !symbols.empty()) {
            // Phase histogram to diagnose constellation display
            int bins[8] = {0}; // 45° bins: 0-45, 45-90, ..., 315-360
            float mag_sum = 0, mag_min = 1e9, mag_max = 0;
            for (const auto& s : symbols) {
                float phase = std::atan2(s.imag(), s.real()) * 180.0f / 3.14159265f;
                if (phase < 0) phase += 360.0f;
                int bin = static_cast<int>(phase / 45.0f) % 8;
                bins[bin]++;
                float mag = std::abs(s);
                mag_sum += mag;
                if (mag < mag_min) mag_min = mag;
                if (mag > mag_max) mag_max = mag;
            }
            float mag_avg = mag_sum / symbols.size();
            LOG_MODEM(INFO, "getConstellationSymbols: %zu symbols (mode=%d) mag=[%.3f,%.3f,%.3f] phase_hist: %d %d %d %d %d %d %d %d",
                      symbols.size(), static_cast<int>(mode_),
                      mag_min, mag_avg, mag_max,
                      bins[0], bins[1], bins[2], bins[3], bins[4], bins[5], bins[6], bins[7]);
            // Log first 10 symbols
            if (log_count < 2) {
                for (size_t i = 0; i < std::min(size_t(20), symbols.size()); i++) {
                    float ph = std::atan2(symbols[i].imag(), symbols[i].real()) * 180.0f / 3.14159265f;
                    LOG_MODEM(INFO, "  sym[%zu]: (%.4f, %.4f) mag=%.4f phase=%.1f°",
                              i, symbols[i].real(), symbols[i].imag(), std::abs(symbols[i]), ph);
                }
            }
            log_count++;
        }
        if (!symbols.empty()) {
            return symbols;
        }
    }

    // Hold last non-empty constellation briefly so GUI doesn't flicker to empty
    // during control-profile reconfiguration between frames.
    if (!constellation_cache_.empty()) {
        auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - constellation_cache_time_).count();
        if (age_ms <= CONSTELLATION_CACHE_HOLD_MS) {
            return constellation_cache_;
        }
    }
    return {};
}

Modulation StreamingDecoder::getConstellationModulation() const {
    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
    if (waveform_) {
        return waveform_->getConstellationModulation();
    }
    return Modulation::QPSK;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void StreamingDecoder::reset(bool reset_doppler_coherence) {
    // Increment generation BEFORE acquiring lock - any ongoing search will see new value
    reset_generation_.fetch_add(1);

    std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);

    sync_controller_.ring_.write_pos_ = 0;
    sync_controller_.ring_.correlation_pos_ = 0;
    sync_position_ = 0;
    sync_correlation_ = 0.0f;
    sync_gap_error_samples_ = 0.0f;
    samples_since_sync_ = 0;
    sync_controller_.ring_.total_fed_ = 0;
    feed_iter_ = 0;
    overflow_events_ = 0;
    sync_controller_.clearRejectStreak();
    sync_controller_.expect_full_ofdm_anchor_ = false;
    sync_from_warm_timed_window_ = false;
    resetFrameArrivalTrackingLocked();
    state_ = DecoderState::SEARCHING;
    pending_total_cw_ = 0;
    burst_blocks_decoded_ = 0;
    burst_soft_buffer_.clear();
    descriptor_group_size_locked_ = false;  // group ended/aborted — cfg writes may apply again
    burst_metric_templates_.clear();
    mc_burst_pending_frame_ = false;
    mc_burst_pending_soft_bits_.clear();
    constellation_cache_.clear();
    constellation_cache_time_ = std::chrono::steady_clock::time_point{};
    use_burst_interleave_ = false;
    new_data_available_ = false;
    last_decoded_sync_pos_ = SIZE_MAX;
    sync_controller_.ring_.search_floor_abs_ = 0;
    sync_controller_.ring_.search_floor_abs_valid_ = false;

    std::fill(sync_controller_.ring_.buffer_.begin(), sync_controller_.ring_.buffer_.end(), 0.0f);
    if (waveform_) waveform_->reset();
    idle_noise_snr_estimator_.reset();

    {
        std::lock_guard<std::mutex> qlock(queue_mutex_);
        while (!frame_queue_.empty()) frame_queue_.pop();
    }

    {
        std::lock_guard<std::mutex> slock(stats_mutex_);
        stats_ = DecoderStats{};
    }

    sync_controller_.ring_.noise_floor_ = 0.001f;
    agc_level_est_ = 0.5f;  // RX-AGC: re-arm HIGH → gain 1 (no-op) until a low level is confirmed
    agc_gain_ = 1.0f;
    last_snr_.store(0.0f);
    last_ofdm_broadband_snr_db_valid_.store(false);
    last_ofdm_broadband_snr_db_.store(0.0f);
    cfo_tracker_.store(0.0f);
    last_fading_index_.store(0.0f);
    // Doppler-coherence disc: SLOW per-connection channel-state estimator (needs ~31 per-frame
    // |H|^2 snapshots to validate). It MUST survive the pre-TX clearRxBuffer() reset (called
    // before every half-duplex ACK turnaround for echo prevention) — otherwise its snapshot pool
    // is wiped every ~5-frame group and it NEVER reaches the 8-snapshot/24-reading minimum (the
    // bug that left the discriminator dead on every half-duplex transfer, task #55). Only a true
    // connection/mode reset clears it; clearRxBuffer passes reset_doppler_coherence=false.
    if (reset_doppler_coherence) {
        doppler_coherence_.reset();
        last_doppler_coherence_score_.store(0.0f);
        last_doppler_hz_.store(0.0f);
        last_doppler_coherence_valid_.store(false);
    }
}

void StreamingDecoder::stop() {
    shutdown_.store(true);
    sync_controller_.ring_.data_cv_.notify_all();
}

} // namespace gui
} // namespace ultra
