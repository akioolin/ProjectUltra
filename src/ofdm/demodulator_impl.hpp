#pragma once

// OFDMDemodulator::Impl - Private implementation struct
// Split across multiple .cpp files for maintainability

#include "ultra/ofdm.hpp"
#include "ultra/dsp.hpp"
#include "demodulator_constants.hpp"
#include <atomic>
#include <mutex>
#include <vector>

namespace ultra {

// Forward declaration
class LDPCDecoder;

struct OFDMDemodulator::Impl {
    ModemConfig config;
    FFT fft;
    NCO mixer;

    // Carrier indices (must match modulator)
    std::vector<int> data_carrier_indices;
    std::vector<int> pilot_carrier_indices;
    std::vector<Complex> pilot_sequence;

    // Synchronization state (atomic for thread-safe UI access)
    enum class State { SEARCHING, SYNCED };
    std::atomic<State> state{State::SEARCHING};
    std::atomic<int> synced_symbol_count{0};
    std::atomic<int> idle_call_count{0};

    // Sample buffer
    Samples rx_buffer;
    size_t symbol_samples;

    // Reused OFDM hot-path scratch. These buffers are single-thread owned by
    // the demodulator instance and avoid heap churn in per-symbol processing.
    std::vector<Complex> baseband_scratch;
    std::vector<Complex> symbol_scratch;
    std::vector<Complex> freq_domain_scratch;
    std::vector<Complex> equalized_scratch;
    std::vector<Complex> constellation_update_scratch;
    std::vector<Complex> differential_symbols_scratch;
    std::vector<float> differential_signal_power_scratch;
    std::vector<Complex> d8psk_constellation_update_scratch;
    std::vector<Complex> dqpsk_constellation_update_scratch;
    std::vector<float> dqpsk_valid_errors_scratch;
    std::vector<Complex> interp_h_full_scratch;
    std::vector<Complex> interp_h_cir_scratch;
    std::vector<Complex> interp_h_clean_scratch;
    std::vector<int> interp_pilot_logical_pos_scratch;
    std::vector<Complex> interp_idft_phasors;
    std::vector<Complex> interp_dft_phasors;

    // Channel estimate (per carrier)
    std::vector<Complex> channel_estimate;
    float noise_variance = 0.1f;

    // SNR estimation (from pilots)
    float estimated_snr_linear = 1.0f;
    float snr_alpha = 0.3f;
    int snr_symbol_count = 0;

    // Measurement-quality in-band SNR estimate for operator display and future
    // adaptation. Valid only after OFDM LTS/pilot residuals have been measured;
    // unlike estimated_snr_linear, this must not feed LLR scaling.
    bool last_snr_db_estimate_valid = false;
    float last_snr_db_estimate = 0.0f;

    // Last LTS estimate quality. False training locks on silence/noise have
    // near-zero values here even when the clipped LLR stream looks plausible.
    float last_lts_signal_power = 1.0f;
    float last_lts_channel_magnitude = 1.0f;
    float last_lts_residual_cfo_hz = 0.0f;

    // Fading index (from pilot magnitude variance)
    // 0 = flat channel, > 0.15 = significant fading
    float last_fading_index = 0.0f;

    // Output data
    Bytes demod_data;
    std::vector<float> soft_bits;
    ChannelQuality quality;

    // Constellation display (latest equalized symbols)
    std::vector<Complex> constellation_symbols;
    mutable std::mutex constellation_mutex;

    // Sync detection
    std::vector<Complex> sync_sequence;
    float sync_threshold;
    size_t last_sync_offset = 0;

    // Noise floor tracking for amplitude-independent sync detection
    float noise_floor_energy = 0.0f;

    // Pre-computed interpolation lookup
    struct InterpInfo {
        int fft_idx;
        int lower_pilot;
        int upper_pilot;
        float alpha;
    };
    std::vector<InterpInfo> interp_table;

    // All carrier FFT indices in logical order (for DFT interpolation)
    // Indices 0..N_carriers-1, mapping logical carrier to FFT bin
    std::vector<int> all_carrier_fft_indices;
    // Which logical indices are pilots (bitmask-style: pilot_logical_indices[i] = true)
    std::vector<bool> is_pilot_logical;

    // Frequency offset estimation and correction
    float freq_offset_hz = 0.0f;
    float freq_offset_filtered = 0.0f;
    bool chirp_cfo_estimated = false;  // True if CFO was set externally (e.g., chirp)
    std::vector<Complex> prev_pilot_phases;
    int symbols_since_sync = 0;
    float freq_correction_phase = 0.0f;

    // Pilot-based phase tracking for differential modulation
    Complex pilot_phase_correction = Complex(1, 0);

    // Symbol timing recovery
    float timing_offset_samples = 0.0f;

    // Carrier phase recovery
    Complex carrier_phase_correction = Complex(1, 0);
    bool carrier_phase_initialized = false;

    // Per-carrier phase from LTS
    std::vector<Complex> lts_carrier_phases;

    // Per-carrier phase slope from timing offset (radians per carrier index)
    // Estimated from LTS, used to de-slope pilot H before complex interpolation
    float lts_phase_slope = 0.0f;

    // Decision-directed per-carrier phase corrections from previous symbol.
    // Stored in equalize(), applied after interpolation in next updateChannelEstimate().
    // Each entry is the phase error (radians) measured at that data carrier.
    std::vector<float> dd_phase_corrections;

    // QAM16 decision-directed per-carrier channel observations from the previous
    // data symbol. The receiver loop updates pilots before equalization, so the
    // DD H=Y/X_hat observations are produced in equalize() and consumed once by
    // the next updateChannelEstimate() call after pilot interpolation.
    std::vector<Complex> dd_qam16_channel_observations_;
    std::vector<float> dd_qam16_measurement_var_;
    std::vector<float> dd_qam16_reliability_;
    std::vector<float> dd_qam16_channel_var_;

    // LTS time-domain reference for fine timing (passband templates)
    std::vector<float> lts_passband_I;
    std::vector<float> lts_passband_Q;

    // Phase inversion detection
    bool llr_sign_flip = false;

    // Manual timing offset adjustment
    int manual_timing_offset = 0;

    // DBPSK state
    std::vector<Complex> dbpsk_prev_equalized;
    bool dbpsk_first_symbol = true;
    bool dqpsk_skip_first_symbol = false;

    // Phase offset from LTS for DQPSK initialization
    // This captures any residual phase between training and data
    Complex lts_phase_offset = Complex(1, 0);

    // Adaptive equalizer state
    std::vector<Complex> lms_weights;
    std::vector<Complex> last_decisions;
    std::vector<float> rls_P;

    // Per-carrier noise variance after equalization
    std::vector<float> carrier_noise_var;

    // RX-local hard-erasure decisions for the current data symbol.
    // Set by equalize() from gamma_k = |H_k|^2 / sigma^2, consumed by
    // demodulateSymbol() to write exact zero LLRs.
    bool rx_carrier_erasure_enabled_ = false;
    std::vector<uint8_t> carrier_erasure_flags_;
    std::vector<uint8_t> differential_prev_erased_;

    // Per-carrier adaptive LLR scaling: track |equalized| stability over symbols
    // Stable carriers keep full LLR confidence; fading carriers get inflated noise
    std::vector<float> carrier_eq_mag_ema_;   // EMA of |equalized[i]| per carrier
    std::vector<float> carrier_eq_mag_var_;   // EMA of (|eq| - ema)² per carrier

    // Two-pass D8PSK decoding (DQPSK-assisted phase correction)
    // Uses embedded DQPSK grid (45° margins) for robust phase estimation
    bool d8psk_two_pass_enabled_ = true;
    static constexpr float TWO_PASS_FADING_THRESHOLD = 0.30f;  // Above AWGN noise floor (~0.12-0.28)

    // Two-pass DQPSK decoding (per-carrier phase correction)
    // Estimates phase error from hard decisions, corrects before soft demapping
    bool dqpsk_two_pass_enabled_ = true;  // Enable for fading channel improvement

    // ==========================================================================
    // CONSTRUCTOR
    // ==========================================================================
    Impl(const ModemConfig& cfg);

    // ==========================================================================
    // INITIALIZATION (demodulator.cpp)
    // ==========================================================================
    void setupCarriers();
    void generateSequences();
    void buildInterpTable();
    void buildInterpolationPhasors();

    // ==========================================================================
    // SYNC DETECTION (ofdm_sync.cpp)
    // ==========================================================================
    bool hasMinimumEnergy(size_t offset, size_t window_len);
    std::vector<Complex> toAnalytic(const float* samples, size_t len);
    float measureRealCorrelation(size_t offset, float* out_energy = nullptr);
    float measureSchmidlCoxCorrelation(size_t offset, Complex* out_P = nullptr, float* out_energy = nullptr);
    float measureAnalyticCorrelation(size_t offset, Complex* out_P = nullptr, float* out_energy = nullptr);
    float measureCorrelation(size_t offset, float* out_energy = nullptr);
    bool detectSync(size_t offset);
    float estimateCoarseCFO(size_t sync_offset);
    float estimateCFOFromTraining(const float* samples, size_t num_symbols, float coarse_cfo_hz = 0.0f);
    size_t refineLTSTiming(size_t coarse_sts_start);
    std::vector<float> trialDemodulate(size_t data_start_offset, size_t num_symbols);
    std::pair<bool, int> huntForCodeword(size_t candidate_sync_pos);

    // ==========================================================================
    // CHANNEL ESTIMATION & EQUALIZATION (channel_equalizer.cpp)
    // ==========================================================================
    const std::vector<Complex>& toBaseband(SampleSpan samples);
    const std::vector<Complex>& extractSymbol(const std::vector<Complex>& baseband, size_t offset);
    void updateChannelEstimate(const std::vector<Complex>& freq_domain);
    void estimateChannelFromLTS(const float* training_samples, size_t num_symbols);
    void updateLastSNREstimate(float signal_power, float noise_power,
                               size_t independent_bins, float alpha,
                               bool fitted_common_gain = false,
                               bool noise_reference_only = false,
                               float noise_power_reference_scale = 1.0f);
    void interpolateChannel();
    Complex hardDecision(Complex sym, Modulation mod) const;
    void lmsUpdate(int idx, Complex received, Complex reference);
    void rlsUpdate(int idx, Complex received, Complex reference);
    const std::vector<Complex>& equalize(const std::vector<Complex>& freq_domain, Modulation mod);
    const std::vector<Complex>& equalize(const std::vector<Complex>& freq_domain);

    // ==========================================================================
    // DEMODULATION (demodulator.cpp)
    // ==========================================================================
    void demodulateSymbol(const std::vector<Complex>& equalized, Modulation mod);
    bool demodulateD8PSKTwoPass(const std::vector<Complex>& equalized, float noise_variance);
    void demodulateDQPSKTwoPass(const std::vector<Complex>& equalized, float noise_variance);
    float computeFadingIndex() const;
    void updateQuality();
};

} // namespace ultra
