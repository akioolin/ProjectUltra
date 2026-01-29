# FEC Abstraction Plan

## Date: 2026-01-29

## Current State

### What Exists
1. **ICodec interface** (`src/fec/codec_interface.hpp`) - well-designed but UNUSED
2. **LDPCEncoder/Decoder** (`include/ultra/fec.hpp`) - used directly everywhere
3. **Interleavers** - `Interleaver` and `ChannelInterleaver` classes

### Problems
- All code directly uses `LDPCEncoder`/`LDPCDecoder`
- No way to swap codec without major refactoring
- Hardcoded 648-bit codeword assumption in many places
- Cannot experiment with other FEC types

## Goal

Support multiple FEC codecs through the existing ICodec interface:
- LDPC (current) - 802.11n style, 648-bit codewords
- Convolutional + Viterbi - for streaming, low-latency
- Turbo codes - alternative to LDPC, better at very low SNR
- Polar codes - 5G standard, good for short blocks
- Reed-Solomon - for outer code in concatenated schemes

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         ICodec Interface                         │
├─────────────────────────────────────────────────────────────────┤
│  getName(), setRate(), getRate()                                 │
│  encode(Bytes) -> Bytes                                          │
│  decode(soft_bits) -> (success, Bytes)                          │
│  getCodewordBits(), getInfoBits(), getDataBytes()               │
└─────────────────────────────────────────────────────────────────┘
          ▲              ▲              ▲              ▲
          │              │              │              │
   ┌──────┴──────┐ ┌─────┴─────┐ ┌─────┴─────┐ ┌─────┴─────┐
   │  LDPCCodec  │ │ConvCodec  │ │TurboCodec │ │PolarCodec │
   │ (wrapper)   │ │(+Viterbi) │ │           │ │           │
   └─────────────┘ └───────────┘ └───────────┘ └───────────┘

┌─────────────────────────────────────────────────────────────────┐
│                       CodecFactory                               │
├─────────────────────────────────────────────────────────────────┤
│  createCodec(type, rate) -> unique_ptr<ICodec>                  │
│  getAvailableCodecs() -> vector<CodecInfo>                      │
│  getCodecCapabilities(type) -> CodecCapabilities                │
└─────────────────────────────────────────────────────────────────┘
```

## Implementation Plan

### Phase 1: LDPC Wrapper (Foundation)

**Goal:** Make existing LDPC work through ICodec without breaking anything.

**Files to create:**
```
src/fec/ldpc_codec.hpp      # LDPCCodec class declaration
src/fec/ldpc_codec.cpp      # LDPCCodec implementation (wraps LDPCEncoder/Decoder)
src/fec/codec_factory.hpp   # Factory for creating codecs
src/fec/codec_factory.cpp   # Factory implementation
```

**LDPCCodec class:**
```cpp
// src/fec/ldpc_codec.hpp
#pragma once

#include "codec_interface.hpp"
#include "ultra/fec.hpp"
#include <memory>

namespace ultra {
namespace fec {

class LDPCCodec : public ICodec {
public:
    explicit LDPCCodec(CodeRate rate = CodeRate::R1_2);
    ~LDPCCodec() override;

    // ICodec interface
    std::string getName() const override { return "802.11n LDPC"; }

    void setRate(CodeRate rate) override;
    CodeRate getRate() const override;

    void setMaxIterations(int iterations) override;
    int getMaxIterations() const override;

    Bytes encode(const Bytes& data) override;
    std::pair<bool, Bytes> decode(const std::vector<float>& soft_bits) override;
    DecodeResult decodeExtended(const std::vector<float>& soft_bits) override;

    size_t getCodewordBits() const override { return 648; }
    size_t getInfoBits() const override;
    size_t getParityBits() const override;
    size_t getCodewordBytes() const override { return 81; }
    size_t getDataBytes() const override;
    float getEffectiveRate() const override;

private:
    std::unique_ptr<LDPCEncoder> encoder_;
    std::unique_ptr<LDPCDecoder> decoder_;
    CodeRate rate_;
    int max_iterations_ = 50;
};

} // namespace fec
} // namespace ultra
```

**CodecFactory:**
```cpp
// src/fec/codec_factory.hpp
#pragma once

#include "codec_interface.hpp"
#include <string>
#include <vector>

namespace ultra {
namespace fec {

enum class CodecType {
    LDPC,           // 802.11n LDPC (current default)
    LDPC_5G,        // 5G NR LDPC (future)
    CONVOLUTIONAL,  // K=7 convolutional + Viterbi (future)
    TURBO,          // 3GPP Turbo (future)
    POLAR,          // 5G Polar (future)
    REED_SOLOMON,   // RS for outer code (future)
};

struct CodecInfo {
    CodecType type;
    std::string name;
    std::string description;
    std::vector<CodeRate> supported_rates;
    size_t min_block_bits;
    size_t max_block_bits;
    bool supports_soft_decode;
    bool is_available;  // Implementation exists
};

class CodecFactory {
public:
    // Create a codec instance
    static CodecPtr create(CodecType type, CodeRate rate = CodeRate::R1_2);

    // Create by name (for config files)
    static CodecPtr createByName(const std::string& name, CodeRate rate = CodeRate::R1_2);

    // Get info about available codecs
    static std::vector<CodecInfo> getAvailableCodecs();

    // Get default codec type
    static CodecType getDefaultType() { return CodecType::LDPC; }
};

} // namespace fec
} // namespace ultra
```

### Phase 2: Update Consumers

**Goal:** Update all code to use ICodec instead of direct LDPC classes.

**Files to modify:**

1. **ModemEngine** (`src/gui/modem/modem_engine.hpp/cpp`)
   - Replace `LDPCEncoder` with `CodecPtr`
   - Replace `LDPCDecoder` with `CodecPtr`
   - Use factory to create codec

2. **StreamingDecoder** (`src/gui/modem/streaming_decoder.hpp/cpp`)
   - Use `ICodec` for decoding
   - Remove direct LDPC dependency

3. **Waveforms** (if they use FEC directly)
   - `mc_dpsk_waveform.cpp`
   - `ofdm_chirp_waveform.cpp`
   - `ofdm_cox_waveform.cpp`

**Example refactoring:**
```cpp
// Before:
std::unique_ptr<LDPCEncoder> encoder_;
std::unique_ptr<LDPCDecoder> decoder_;
encoder_ = std::make_unique<LDPCEncoder>(CodeRate::R1_2);

// After:
fec::CodecPtr codec_;
codec_ = fec::CodecFactory::create(fec::CodecType::LDPC, CodeRate::R1_2);
```

### Phase 3: Convolutional Codec (First New Codec)

**Goal:** Add K=7 convolutional code with Viterbi decoder for comparison.

**Why convolutional first:**
- Well-understood, mature algorithm
- Good baseline for comparison
- Useful for streaming/low-latency modes
- Simpler than Turbo/Polar

**Implementation:**
```cpp
// src/fec/conv_codec.hpp
class ConvolutionalCodec : public ICodec {
public:
    explicit ConvolutionalCodec(CodeRate rate = CodeRate::R1_2);

    // K=7, G1=0171, G2=0133 (industry standard)
    // Punctured for different rates

    std::string getName() const override { return "K=7 Convolutional"; }
    // ... implement ICodec interface

private:
    // Viterbi decoder state
    struct ViterbiState;
    std::unique_ptr<ViterbiState> viterbi_;
};
```

**Rates via puncturing:**
| Rate | Puncture Pattern |
|------|-----------------|
| R1/2 | No puncturing |
| R2/3 | P=[1,1,0,1] |
| R3/4 | P=[1,1,0,1,1,0] |

### Phase 4: Turbo Codec (Optional)

3GPP-style parallel concatenated convolutional code.

**Advantages:**
- Better than LDPC at very low SNR
- Good for short blocks

**Disadvantages:**
- Higher complexity
- Slower decoding

### Phase 5: Polar Codec (Optional)

5G NR polar codes with successive cancellation decoding.

**Advantages:**
- Theoretically optimal for short blocks
- Lower complexity than LDPC for small sizes

**Disadvantages:**
- Less mature implementations
- Needs careful CRC design

### Phase 6: Concatenated Codes

RS(255,223) outer + inner code for very reliable transmission.

```cpp
class ConcatenatedCodec : public ICodec {
public:
    ConcatenatedCodec(CodecPtr outer, CodecPtr inner);
    // Encode: inner(outer(data))
    // Decode: outer_decode(inner_decode(soft_bits))
};
```

## Configuration Integration

### Protocol Negotiation

Add codec type to CONNECT frame capabilities:
```cpp
// In frame_v2.hpp
namespace CodecCapabilities {
    constexpr uint8_t LDPC      = 0x01;
    constexpr uint8_t CONV      = 0x02;
    constexpr uint8_t TURBO     = 0x04;
    constexpr uint8_t POLAR     = 0x08;
}
```

### Settings

Add to `AppSettings`:
```cpp
struct FECSettings {
    fec::CodecType preferred_codec = fec::CodecType::LDPC;
    int max_iterations = 50;
    bool allow_codec_negotiation = true;
};
```

## Testing Strategy

### Unit Tests

1. **Codec conformance tests:**
   - Encode/decode round-trip at various SNRs
   - Rate switching
   - Edge cases (all zeros, all ones, random)

2. **Performance tests:**
   - BER vs Eb/N0 curves
   - Decode latency
   - Memory usage

### Integration Tests

1. **test_iwaveform with different codecs:**
   ```bash
   ./build/test_iwaveform --snr 5 -w mc_dpsk --codec ldpc
   ./build/test_iwaveform --snr 5 -w mc_dpsk --codec conv
   ```

2. **CLI simulator with codec selection:**
   ```bash
   ./build/cli_simulator --snr 10 --codec ldpc
   ```

## File Structure (Final)

```
src/fec/
├── codec_interface.hpp    # ICodec interface (existing)
├── codec_factory.hpp      # Factory interface
├── codec_factory.cpp      # Factory implementation
├── ldpc_codec.hpp         # LDPC wrapper
├── ldpc_codec.cpp         # LDPC wrapper implementation
├── conv_codec.hpp         # Convolutional (Phase 3)
├── conv_codec.cpp
├── turbo_codec.hpp        # Turbo (Phase 4)
├── turbo_codec.cpp
├── polar_codec.hpp        # Polar (Phase 5)
├── polar_codec.cpp
├── concat_codec.hpp       # Concatenated (Phase 6)
├── concat_codec.cpp
├── ldpc_encoder.cpp       # Existing LDPC encoder impl
└── ldpc_decoder.cpp       # Existing LDPC decoder impl
```

## Migration Path

1. **Phase 1:** Create LDPCCodec wrapper, CodecFactory - NO breaking changes
2. **Phase 2:** Gradually update consumers to use ICodec - one file at a time
3. **Phase 3+:** Add new codecs - consumers automatically support them

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Performance regression | Benchmark before/after, inline wrapper methods |
| Breaking existing tests | Run regression suite after each change |
| Codec compatibility issues | Negotiate codec in CONNECT, fallback to LDPC |
| Memory overhead | Use shared codec instances where possible |

## Success Criteria

1. All existing tests pass with LDPCCodec wrapper
2. Can swap codec type via config without code changes
3. At least one alternative codec (convolutional) working
4. No performance regression >5%
5. Clean separation - waveforms don't know codec details

## Priority Order

1. **P0 (Must have):** Phase 1 + Phase 2 - abstraction layer
2. **P1 (Should have):** Phase 3 - convolutional codec
3. **P2 (Nice to have):** Phases 4-6 - additional codecs

## Estimated Effort

| Phase | Effort | Dependencies |
|-------|--------|--------------|
| Phase 1 | 2-4 hours | None |
| Phase 2 | 4-8 hours | Phase 1 |
| Phase 3 | 8-16 hours | Phase 2 |
| Phase 4 | 16-24 hours | Phase 2 |
| Phase 5 | 16-24 hours | Phase 2 |
| Phase 6 | 4-8 hours | Any codec |
