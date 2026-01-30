# AFDM Implementation Plan

**Date:** 2026-01-29
**Status:** PLANNING
**Goal:** Implement AFDM (Affine Frequency Division Multiplexing) to achieve 2-4x throughput improvement on fading HF channels

---

## Executive Summary

AFDM is a chirp-based multicarrier waveform that outperforms both OFDM and OTFS on doubly-selective (time + frequency varying) channels. Key advantages:

- **~1+ dB gain over OTFS**, which itself beats OFDM significantly
- **Lower pilot overhead** than OTFS
- **FFT-based implementation** - reuses existing infrastructure
- **Natural chirp basis** - synergizes with our dual-chirp sync
- **Full diversity** on delay-Doppler channels when properly configured

**Target:** Enable reliable 2+ kbps on "good" HF channel (currently stuck at 938 bps)

---

## Part 1: Technical Foundation

### 1.1 DAFT Transform (Core of AFDM)

The Discrete Affine Fourier Transform generalizes the DFT with chirp modulation:

**Basis function (discrete chirp):**
```
φ_m[n] = exp(j2π(c₁n² + c₂m² + mn/N))
```

**IDAFT (TX - modulation):**
```
X[n] = Σ x[m] · exp(j2π(c₁n² + c₂m² + mn/N))    for n = 0..N-1
```

**DAFT (RX - demodulation):**
```
x[m] = (1/N) Σ X[n] · exp(-j2π(c₁n² + c₂m² + mn/N))    for m = 0..N-1
```

**Matrix form:**
```
X = Λ_c₂^H · F^H · Λ_c₁^H · x      (IDAFT)
x = Λ_c₁ · F · Λ_c₂ · X            (DAFT)

Where:
  Λ_c = diag(exp(-j2πc·n²))   - Chirp matrix
  F = DFT matrix
```

**Key insight:** IDAFT = chirp → FFT → chirp. We can implement using existing FFT!

### 1.2 Chirp Parameters (c₁, c₂)

**c₁ (time-chirp rate) - CRITICAL for full diversity:**
```
c₁ ≥ (2·k_max + 1) / (2N)

Where:
  k_max = maximum Doppler bin index = ceil(f_doppler_max / Δf)
  N = number of subcarriers
  Δf = subcarrier spacing
```

For HF with ±10 Hz Doppler, N=64, Δf=75 Hz:
```
k_max = ceil(10/75) = 1
c₁ ≥ (2·1 + 1) / (2·64) = 3/128 ≈ 0.0234
```

**c₂ (frequency-chirp rate) - flexible:**
- Can be any irrational number, or rational << 1/(2N)
- Typical choice: c₂ = 0 (simplifies implementation)
- Or c₂ = c₁ for symmetry

**Special cases:**
- c₁ = c₂ = 0 → OFDM (backward compatible!)
- c₁ > 0, c₂ = 0 → Standard AFDM

### 1.3 Channel Model in DAFT Domain

The effective channel matrix:
```
H_eff = Λ_c₂ · F · Λ_c₁ · H_time · Λ_c₁^H · F^H · Λ_c₂^H
```

**Key property:** H_eff becomes **quasi-banded** (near-diagonal) when c₁ is chosen correctly. This means:
- Channel appears sparse in DAFT domain
- Simple per-subcarrier equalization works
- Errors spread across all symbols (built-in interleaving)

### 1.4 Frame Structure

```
┌─────────────┬─────────────┬──────────────────────────────┬─────────────┐
│ Chirp Sync  │    CPP      │         AFDM Symbols         │   Guard     │
│  (preamble) │ (prefix)    │   [Pilots + Data in DAFT]    │             │
└─────────────┴─────────────┴──────────────────────────────┴─────────────┘
     ↑              ↑                    ↑
 Our existing   Chirp-Periodic    Embedded pilots
 dual-chirp     Prefix (like CP)  for channel est.
```

**Chirp-Periodic Prefix (CPP):**
- Length: L_cpp ≥ max_delay_samples
- Analogous to OFDM cyclic prefix
- Enables circular convolution property

**Embedded Pilots:**
- Place pilot symbols in DAFT domain
- Guard intervals Q on each side: Q ≥ 2·N·c₁·(l_max + 1) - 1
- Channel estimated from pilot response spread

---

## Part 2: Implementation Architecture

### 2.1 File Structure

```
src/afdm/
├── afdm.hpp                 # Public interface (AFDMModulator, AFDMDemodulator)
├── afdm.cpp                 # Core implementation
├── afdm_config.hpp          # AFDMConfig struct
├── daft.hpp                 # DAFT/IDAFT transforms
├── daft.cpp                 # Transform implementation
└── afdm_channel_estimator.hpp/cpp  # Embedded pilot channel estimation

src/waveform/
├── afdm_waveform.hpp        # IWaveform implementation
└── afdm_waveform.cpp

tools/
├── test_afdm.cpp            # Standalone AFDM test
└── test_afdm_vs_ofdm.cpp    # Comparison test (like test_otfs_vs_ofdm)

tests/
└── test_daft.cpp            # Unit tests for DAFT transform
```

### 2.2 Core Classes

```cpp
// afdm_config.hpp
struct AFDMConfig {
    uint32_t N = 64;              // Subcarriers (DAFT size)
    uint32_t cpp_length = 16;     // Chirp-periodic prefix samples
    float c1 = 0.025f;            // Time-chirp rate (≥ full diversity threshold)
    float c2 = 0.0f;              // Frequency-chirp rate (0 = standard AFDM)
    uint32_t pilot_spacing = 8;   // Pilot every N carriers in DAFT domain
    uint32_t pilot_guard = 2;     // Guard bins around each pilot
    float sample_rate = 48000.0f;
    float center_freq = 1500.0f;
    float bandwidth = 2400.0f;    // Hz

    // Derived
    float subcarrier_spacing() const { return bandwidth / N; }
    float symbol_duration() const { return N / sample_rate; }
    int max_doppler_bins() const { return static_cast<int>(std::ceil(10.0f / subcarrier_spacing())); }
    float min_c1_for_diversity() const {
        return (2.0f * max_doppler_bins() + 1) / (2.0f * N);
    }
};

// afdm.hpp
class AFDMModulator {
public:
    explicit AFDMModulator(const AFDMConfig& config = AFDMConfig());

    // Map data bytes to DAFT-domain symbols
    std::vector<Complex> mapToDAFT(ByteSpan data, Modulation mod);

    // Insert pilots into DAFT-domain frame
    std::vector<Complex> insertPilots(const std::vector<Complex>& data_symbols);

    // Generate time-domain samples via IDAFT
    Samples modulate(const std::vector<Complex>& daft_symbols);

    // Generate preamble (dual chirp - reuse existing)
    Samples generatePreamble();

    // Full TX: data → DAFT map → pilots → IDAFT → add preamble
    Samples transmit(ByteSpan data, Modulation mod);

    size_t bitsPerFrame(Modulation mod) const;
    size_t symbolsPerFrame() const;

private:
    AFDMConfig config_;
    std::vector<Complex> chirp_c1_;  // Pre-computed exp(-j2π·c1·n²)
    std::vector<Complex> chirp_c2_;  // Pre-computed exp(-j2π·c2·m²)
};

class AFDMDemodulator {
public:
    explicit AFDMDemodulator(const AFDMConfig& config = AFDMConfig());

    // Process received samples (after sync)
    bool process(SampleSpan samples);

    // Get demodulated DAFT-domain symbols
    std::vector<Complex> getDAFTSymbols() const;

    // Get soft bits for FEC decoder
    std::vector<float> getSoftBits() const;

    // Channel estimate in DAFT domain
    std::vector<Complex> getChannelEstimate() const;

    // Estimated SNR from pilots
    float getEstimatedSNR() const;

    // Fading index (coefficient of variation of channel magnitudes)
    float getFadingIndex() const;

    void reset();
    void setCFO(float cfo_hz);  // Apply CFO correction

private:
    AFDMConfig config_;
    std::vector<Complex> chirp_c1_, chirp_c2_;
    std::vector<Complex> channel_estimate_;
    float estimated_snr_ = 0.0f;
    float cfo_hz_ = 0.0f;
};
```

### 2.3 DAFT Implementation

```cpp
// daft.hpp
namespace afdm {

// Compute DAFT: time-domain → DAFT-domain
// x[m] = (1/N) Σ X[n] · exp(-j2π(c₁n² + c₂m² + mn/N))
std::vector<Complex> daft(
    const std::vector<Complex>& time_samples,
    float c1, float c2
);

// Compute IDAFT: DAFT-domain → time-domain
// X[n] = Σ x[m] · exp(j2π(c₁n² + c₂m² + mn/N))
std::vector<Complex> idaft(
    const std::vector<Complex>& daft_symbols,
    float c1, float c2
);

// Efficient implementation using chirp + FFT + chirp
// IDAFT(x) = conj(chirp_c1) ⊙ IFFT(conj(chirp_c2) ⊙ x)
// DAFT(X) = chirp_c2 ⊙ FFT(chirp_c1 ⊙ X)

} // namespace afdm
```

### 2.4 IWaveform Integration

```cpp
// afdm_waveform.hpp
class AFDMWaveform : public IWaveform {
public:
    explicit AFDMWaveform(const ModemConfig& config);

    // IWaveform interface
    std::string getName() const override { return "AFDM"; }
    protocol::WaveformMode getMode() const override { return protocol::WaveformMode::AFDM; }

    Samples modulateFrame(ByteSpan payload, Modulation mod, CodeRate rate) override;

    bool detectSync(SampleSpan samples, SyncResult& result) override;
    void prepareForFrame(const SyncResult& sync) override;
    DecodeResult demodulateAndDecode(SampleSpan samples) override;

    float estimatedSNR() const override;
    float estimatedCFO() const override;
    float getFadingIndex() const override;

    size_t getFrameSamples() const override;
    size_t getPreambleSamples() const override;

private:
    AFDMConfig afdm_config_;
    std::unique_ptr<AFDMModulator> modulator_;
    std::unique_ptr<AFDMDemodulator> demodulator_;
    std::unique_ptr<sync::ChirpSync> chirp_sync_;  // Reuse existing!
    fec::CodecPtr codec_;
};
```

---

## Part 3: Implementation Phases

### Phase 1: DAFT Core (3-4 days)

**Goal:** Working DAFT/IDAFT transforms with unit tests

**Tasks:**
1. [ ] Create `src/afdm/` directory structure
2. [ ] Implement `daft.cpp` with:
   - [ ] Naive DAFT/IDAFT (for reference/testing)
   - [ ] Efficient chirp-FFT-chirp implementation
   - [ ] Pre-computed chirp tables
3. [ ] Create `tests/test_daft.cpp`:
   - [ ] Verify IDAFT(DAFT(x)) = x (roundtrip)
   - [ ] Verify c1=c2=0 matches DFT
   - [ ] Test with various c1 values
   - [ ] Benchmark vs direct FFT
4. [ ] Add to CMakeLists.txt

**Validation criteria:**
- Roundtrip error < 1e-6
- c1=c2=0 matches FFT output exactly
- Performance within 2x of plain FFT

### Phase 2: AFDM Modulator (3-4 days)

**Goal:** Generate valid AFDM frames

**Tasks:**
1. [ ] Implement `AFDMConfig` with HF-optimized defaults
2. [ ] Implement `AFDMModulator`:
   - [ ] Symbol mapping (BPSK, QPSK, DQPSK)
   - [ ] Pilot insertion with guard intervals
   - [ ] IDAFT modulation
   - [ ] Chirp-periodic prefix addition
   - [ ] Preamble generation (reuse ChirpSync)
3. [ ] Create `tools/test_afdm_tx.cpp`:
   - [ ] Generate AFDM frames
   - [ ] Verify spectrum shape (chirped subcarriers)
   - [ ] Write to .f32 file for analysis
4. [ ] Verify in Audacity/spectrum analyzer

**Validation criteria:**
- Clean spectrum within 2.4 kHz bandwidth
- Chirp structure visible in spectrogram
- Preamble correlates correctly

### Phase 3: AFDM Demodulator (4-5 days)

**Goal:** Decode AFDM frames on AWGN channel

**Tasks:**
1. [ ] Implement `AFDMDemodulator`:
   - [ ] CPP removal
   - [ ] DAFT demodulation
   - [ ] Pilot extraction
   - [ ] Channel estimation from pilots
   - [ ] MMSE equalization
   - [ ] Soft bit generation
2. [ ] Integrate with existing LDPC decoder
3. [ ] Create `tools/test_afdm.cpp`:
   - [ ] AWGN loopback test
   - [ ] Measure BER vs SNR
   - [ ] Compare to OFDM baseline
4. [ ] Test with CFO injection

**Validation criteria:**
- 100% decode at SNR ≥ 10 dB on AWGN
- BER curve matches theoretical
- CFO tolerance ±50 Hz

### Phase 4: Fading Channel Optimization (4-5 days)

**Goal:** Verify AFDM advantage on fading channels

**Tasks:**
1. [ ] Tune c1 for HF Doppler spread (0.5-10 Hz)
2. [ ] Optimize pilot density vs overhead tradeoff
3. [ ] Implement decision-directed channel tracking
4. [ ] Create `tools/test_afdm_vs_ofdm.cpp`:
   - [ ] Test on AWGN, good, moderate, poor channels
   - [ ] Compare AFDM vs OFDM vs MC-DPSK
   - [ ] Measure throughput at target BER (1e-3)
5. [ ] Document optimal parameters per channel type

**Validation criteria:**
- AFDM decodes on "good" channel where OFDM fails
- ≥50% throughput improvement over MC-DPSK on fading
- Matches or exceeds OTFS performance

### Phase 5: IWaveform Integration (2-3 days)

**Goal:** AFDM available as standard waveform mode

**Tasks:**
1. [ ] Implement `AFDMWaveform` (IWaveform interface)
2. [ ] Integrate with ChirpSync for preamble detection
3. [ ] Register in `WaveformFactory`
4. [ ] Add `WaveformMode::AFDM` to protocol enums
5. [ ] Update `recommendWaveformAndRate()` to include AFDM
6. [ ] Add to StreamingDecoder
7. [ ] Test with `test_iwaveform --waveform afdm`

**Validation criteria:**
- Works with existing test infrastructure
- Seamless switching between OFDM/AFDM/MC-DPSK
- Protocol negotiation includes AFDM

### Phase 6: Production Hardening (3-4 days)

**Goal:** Ready for real-world use

**Tasks:**
1. [ ] Add to regression test matrix
2. [ ] Optimize performance (SIMD, lookup tables)
3. [ ] Add GUI visualization (DAFT-domain constellation)
4. [ ] Update documentation
5. [ ] Real HF recording tests (if available)

**Validation criteria:**
- All regression tests pass
- Real-time capable on target hardware
- Documentation complete

---

## Part 4: Test Plan

### 4.1 Unit Tests

| Test | Description | Pass Criteria |
|------|-------------|---------------|
| `test_daft_roundtrip` | IDAFT(DAFT(x)) = x | Error < 1e-6 |
| `test_daft_ofdm_fallback` | c1=c2=0 equals FFT | Exact match |
| `test_daft_chirp_property` | Verify chirp structure | Spectrum analysis |
| `test_afdm_awgn` | AWGN loopback | 100% @ 10 dB |
| `test_afdm_cfo` | CFO tolerance | 100% @ ±30 Hz |

### 4.2 Integration Tests

| Test | Description | Pass Criteria |
|------|-------------|---------------|
| `test_afdm_ldpc` | With LDPC R1/4, R1/2, R2/3 | Correct decode |
| `test_afdm_streaming` | Via StreamingDecoder | Frame callback works |
| `test_afdm_iwaveform` | Full IWaveform interface | All methods work |

### 4.3 Performance Tests (The Critical Ones)

| Channel | SNR | OFDM | MC-DPSK | AFDM Target | Pass |
|---------|-----|------|---------|-------------|------|
| AWGN | 10 | 100% | 100% | 100% | Baseline |
| Good (0.32 fading) | 10 | 40% | 100% | **≥80%** | KEY TEST |
| Moderate (0.43 fading) | 10 | 0% | 100% | **≥60%** | KEY TEST |
| Poor (2ms delay) | 10 | 0% | 80% | **≥60%** | Stretch |

### 4.4 Throughput Comparison

| Channel | MC-DPSK | OFDM R2/3 | AFDM R1/2 | AFDM R2/3 |
|---------|---------|-----------|-----------|-----------|
| AWGN | 938 bps | 2300 bps | 1700 bps | 2300 bps |
| Good | 938 bps | FAILS | **1700 bps** | **1500 bps** |
| Moderate | 938 bps | FAILS | **1200 bps** | FAILS |

**Success = AFDM R1/2 works on "good" channel = 80%+ increase over MC-DPSK**

---

## Part 5: Risk Assessment

| Risk | Impact | Probability | Mitigation |
|------|--------|-------------|------------|
| AFDM doesn't beat OFDM on our channels | High | Low | Theory is solid; test OTFS first as backup |
| c1 tuning is channel-specific | Medium | Medium | Adaptive c1 selection; conservative default |
| Pilot overhead too high | Medium | Low | Optimize spacing; compare to OTFS overhead |
| Implementation complexity | Medium | Medium | Leverage existing FFT, sync, LDPC |
| Real-time performance | Low | Low | DAFT is O(N log N) like FFT |

---

## Part 6: Success Metrics

### Minimum Viable Success
- [ ] AFDM decodes at 80%+ on "good" channel at SNR=10
- [ ] Throughput ≥ 1.5 kbps on "good" channel (vs 938 bps MC-DPSK)
- [ ] Integrated into waveform selection algorithm

### Full Success
- [ ] AFDM decodes at 60%+ on "moderate" channel
- [ ] Throughput ≥ 2 kbps on "good" channel with R2/3
- [ ] Works with real HF recordings
- [ ] Performance matches published AFDM papers

### Stretch Goals
- [ ] Adaptive c1 based on measured Doppler
- [ ] AFDM works on "poor" channel (2ms delay)
- [ ] GUI shows DAFT-domain visualization

---

## Part 7: Timeline

| Week | Phase | Deliverable |
|------|-------|-------------|
| 1 | Phase 1-2 | DAFT transforms + AFDM TX |
| 2 | Phase 3 | AFDM RX + AWGN validation |
| 3 | Phase 4 | Fading channel optimization |
| 4 | Phase 5-6 | Integration + hardening |

**Total: ~4 weeks for full implementation**

**Quick validation path (1 week):**
- Implement basic DAFT + AFDM mod/demod
- Test on good/moderate channels
- If promising, continue to full integration

---

## Part 8: References

### Primary Sources
- [AFDM for 6G (arXiv 2507.21704)](https://arxiv.org/abs/2507.21704) - Comprehensive overview
- [AFDM: A Full Diversity Waveform (arXiv 2104.11331)](https://arxiv.org/abs/2104.11331) - Original paper
- [AFDM Requirements and Challenges (arXiv 2509.16643)](https://arxiv.org/html/2509.16643v1) - Implementation details
- [Ali Bemani PhD Thesis](https://theses.hal.science/tel-04606502) - Most complete reference

### Key Equations Reference
```
DAFT basis:     φ_m[n] = exp(j2π(c₁n² + c₂m² + mn/N))
Full diversity: c₁ ≥ (2·k_max + 1) / (2N)
Guard interval: Q ≥ 2·N·c₁·(l_max + 1) - 1
Channel matrix: H_eff = Λ_c₂ · F · Λ_c₁ · H · Λ_c₁^H · F^H · Λ_c₂^H
```

---

## Appendix A: Quick Start Implementation

For fastest validation, implement in this order:

```cpp
// 1. DAFT transform (2 hours)
std::vector<Complex> daft(const std::vector<Complex>& x, float c1, float c2) {
    const size_t N = x.size();
    std::vector<Complex> result(N);

    // Pre-chirp with c1
    std::vector<Complex> chirped(N);
    for (size_t n = 0; n < N; n++) {
        float phase = 2.0f * M_PI * c1 * n * n;
        chirped[n] = x[n] * Complex(std::cos(phase), std::sin(phase));
    }

    // FFT
    auto fft_result = fft(chirped);  // Use existing FFT

    // Post-chirp with c2
    for (size_t m = 0; m < N; m++) {
        float phase = 2.0f * M_PI * c2 * m * m;
        result[m] = fft_result[m] * Complex(std::cos(phase), std::sin(phase));
    }

    return result;
}

// 2. IDAFT is conjugate operation
std::vector<Complex> idaft(const std::vector<Complex>& x, float c1, float c2) {
    const size_t N = x.size();
    std::vector<Complex> result(N);

    // Pre-chirp with -c2 (conjugate)
    std::vector<Complex> chirped(N);
    for (size_t m = 0; m < N; m++) {
        float phase = -2.0f * M_PI * c2 * m * m;
        chirped[m] = x[m] * Complex(std::cos(phase), std::sin(phase));
    }

    // IFFT
    auto ifft_result = ifft(chirped);

    // Post-chirp with -c1 (conjugate)
    for (size_t n = 0; n < N; n++) {
        float phase = -2.0f * M_PI * c1 * n * n;
        result[n] = ifft_result[n] * Complex(std::cos(phase), std::sin(phase));
    }

    return result;
}

// 3. Test roundtrip
void test_daft_roundtrip() {
    std::vector<Complex> x = generateRandomSymbols(64);
    float c1 = 0.025f, c2 = 0.0f;

    auto transformed = daft(x, c1, c2);
    auto recovered = idaft(transformed, c1, c2);

    float error = computeError(x, recovered);
    assert(error < 1e-5);
}
```

---

## Appendix B: Decision Points

### Decision 1: c2 value
- **Option A:** c2 = 0 (simplest, standard AFDM)
- **Option B:** c2 = c1 (symmetric chirping)
- **Recommendation:** Start with c2 = 0, test c2 = c1 later

### Decision 2: Pilot structure
- **Option A:** Embedded pilots in DAFT domain (like OTFS)
- **Option B:** Separate pilot symbol (like OFDM LTS)
- **Recommendation:** Embedded pilots for better tracking

### Decision 3: Sync method
- **Option A:** Reuse dual-chirp sync (already working)
- **Option B:** AFDM-native sync using first symbol
- **Recommendation:** Reuse dual-chirp (proven, no risk)

### Decision 4: Modulation
- **Option A:** Coherent (QPSK, 16QAM) with pilots
- **Option B:** Differential (DQPSK) without pilots
- **Recommendation:** Start with DQPSK for simplicity, add coherent later

---

*Document created: 2026-01-29*
*Last updated: 2026-01-29*
