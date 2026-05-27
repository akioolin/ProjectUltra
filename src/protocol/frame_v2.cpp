#include "frame_v2.hpp"
#include "ultra/fec.hpp"  // LDPC encoder/decoder + ChannelInterleaver
#include "../fec/frame_interleaver.hpp"  // Frame-level interleaving
#include "../fec/ldpc_codec.hpp"  // For getRecommendedIterations
#include "ultra/logging.hpp"  // LOG_MODEM
#include "ultra/timing_profiler.hpp"
#include <cstring>
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <random>

namespace ultra {
namespace protocol {

namespace {

constexpr float kFixedFrameDefaultMinSumFactor = 0.9375f;

int codeRateCacheIndex(CodeRate rate) {
    switch (rate) {
        case CodeRate::R1_4: return 0;
        case CodeRate::R1_3: return 1;
        case CodeRate::R1_2: return 2;
        case CodeRate::R2_3: return 3;
        case CodeRate::R3_4: return 4;
        case CodeRate::R5_6: return 5;
        case CodeRate::R7_8: return 6;
        default: return 2;
    }
}

LDPCDecoder& fixedFrameDecoderForRate(CodeRate rate) {
    struct DecoderCacheEntry {
        CodeRate rate = CodeRate::AUTO;
        std::unique_ptr<LDPCDecoder> decoder;
    };

    thread_local std::array<DecoderCacheEntry, 7> cache;
    DecoderCacheEntry& entry = cache[codeRateCacheIndex(rate)];
    if (!entry.decoder || entry.rate != rate) {
        entry.decoder = std::make_unique<LDPCDecoder>(rate);
        entry.rate = rate;
    }

    LDPCDecoder& decoder = *entry.decoder;
    decoder.setRate(rate);
    decoder.setMaxIterations(fec::LDPCCodec::getRecommendedIterations(rate));
    decoder.setMinSumFactor(kFixedFrameDefaultMinSumFactor);
    return decoder;
}

bool harqDebugLogEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ULTRA_HARQ_DEBUG_LOG");
        return value && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
    }();
    return enabled;
}

int harqDebugFilter(const char* name) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return -1;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 0);
    return (end && *end == '\0') ? static_cast<int>(parsed) : -1;
}

bool harqDebugKeySelected(const fec::SoftCombineBuffer::Key& key) {
    if (!harqDebugLogEnabled()) {
        return false;
    }
    const int seq_filter = harqDebugFilter("ULTRA_HARQ_DEBUG_SEQ");
    if (seq_filter >= 0 && static_cast<int>(key.seq) != seq_filter) {
        return false;
    }
    const int cw_filter = harqDebugFilter("ULTRA_HARQ_DEBUG_CW");
    if (cw_filter >= 0 && static_cast<int>(key.cw_index) != cw_filter) {
        return false;
    }
    return true;
}

float meanAbsLlr(const std::vector<float>& llrs) {
    if (llrs.empty()) {
        return 0.0f;
    }
    double sum = 0.0;
    for (float llr : llrs) {
        sum += std::abs(llr);
    }
    return static_cast<float>(sum / static_cast<double>(llrs.size()));
}

struct LlrAbsSummary {
    float mean = 0.0f;
    float min = 0.0f;
    float p10 = 0.0f;
    float p50 = 0.0f;
    float p90 = 0.0f;
};

LlrAbsSummary summarizeAbsLlrs(const std::vector<float>& llrs) {
    LlrAbsSummary out;
    if (llrs.empty()) {
        return out;
    }

    std::vector<float> abs_values;
    abs_values.reserve(llrs.size());
    double sum = 0.0;
    float min_value = std::numeric_limits<float>::max();
    for (float llr : llrs) {
        const float a = std::abs(llr);
        abs_values.push_back(a);
        sum += static_cast<double>(a);
        min_value = std::min(min_value, a);
    }

    std::sort(abs_values.begin(), abs_values.end());
    auto percentile = [&](float q) -> float {
        if (abs_values.empty()) {
            return 0.0f;
        }
        const float pos = q * static_cast<float>(abs_values.size() - 1);
        const size_t idx = static_cast<size_t>(std::round(pos));
        return abs_values[std::min(idx, abs_values.size() - 1)];
    };

    out.mean = static_cast<float>(sum / static_cast<double>(llrs.size()));
    out.min = min_value;
    out.p10 = percentile(0.10f);
    out.p50 = percentile(0.50f);
    out.p90 = percentile(0.90f);
    return out;
}

} // namespace

// ============================================================================
// Shared Protocol Types Implementation
// ============================================================================

const char* waveformModeToString(WaveformMode mode) {
    switch (mode) {
        case WaveformMode::OTFS_EQ:    return "OTFS-EQ";
        case WaveformMode::OTFS_RAW:   return "OTFS-RAW";
        case WaveformMode::MFSK:       return "MFSK";
        case WaveformMode::MC_DPSK:    return "MC-DPSK";
        case WaveformMode::OFDM_CHIRP: return "OFDM-CHIRP";
        case WaveformMode::OFDM_NARROW: return "OFDM-NARROW";
        case WaveformMode::AUTO:       return "AUTO";
        default:                       return "UNKNOWN";
    }
}

Bytes ChannelReport::encode() const {
    Bytes data(5);
    // SNR: 0-50 dB mapped to 0-250 (0.2 dB resolution)
    data[0] = static_cast<uint8_t>(std::min(250.0f, std::max(0.0f, snr_db * 5.0f)));
    // Delay spread: 0-25 ms mapped to 0-250 (0.1 ms resolution)
    data[1] = static_cast<uint8_t>(std::min(250.0f, std::max(0.0f, delay_spread_ms * 10.0f)));
    // Doppler: 0-25 Hz mapped to 0-250 (0.1 Hz resolution)
    data[2] = static_cast<uint8_t>(std::min(250.0f, std::max(0.0f, doppler_spread_hz * 10.0f)));
    // Recommended mode
    data[3] = static_cast<uint8_t>(recommended_mode);
    // Capabilities
    data[4] = capabilities;
    return data;
}

ChannelReport ChannelReport::decode(const Bytes& data) {
    ChannelReport report;
    if (data.size() >= 5) {
        report.snr_db = static_cast<float>(data[0]) / 5.0f;
        report.delay_spread_ms = static_cast<float>(data[1]) / 10.0f;
        report.doppler_spread_hz = static_cast<float>(data[2]) / 10.0f;
        report.recommended_mode = static_cast<WaveformMode>(data[3]);
        report.capabilities = data[4];
    }
    return report;
}

const char* ChannelReport::getConditionName() const {
    if (snr_db >= 35.0f && delay_spread_ms < 1.0f && doppler_spread_hz < 1.0f) {
        return "Excellent";
    } else if (snr_db >= 28.0f && delay_spread_ms < 2.0f && doppler_spread_hz < 2.0f) {
        return "Good";
    } else if (snr_db >= 20.0f) {
        return "Moderate";
    } else if (snr_db >= 13.0f) {
        return "Poor";
    } else {
        return "Flutter";
    }
}

namespace v2 {

// ============================================================================
// Callsign hashing (DJB2, 24-bit)
// ============================================================================
uint32_t hashCallsign(const std::string& callsign) {
    uint32_t hash = 5381;
    for (char c : callsign) {
        hash = ((hash << 5) + hash) ^ static_cast<uint8_t>(std::toupper(c));
    }
    return hash & 0xFFFFFF;  // 24 bits
}

// ============================================================================
// Frame type to string
// ============================================================================
const char* frameTypeToString(FrameType type) {
    switch (type) {
        case FrameType::PING:        return "PING";
        case FrameType::PONG:        return "PONG";
        case FrameType::PROBE:       return "PROBE";
        case FrameType::PROBE_ACK:   return "PROBE_ACK";
        case FrameType::CONNECT:     return "CONNECT";
        case FrameType::CONNECT_ACK: return "CONNECT_ACK";
        case FrameType::CONNECT_NAK: return "CONNECT_NAK";
        case FrameType::DISCONNECT:  return "DISCONNECT";
        case FrameType::KEEPALIVE:   return "KEEPALIVE";
        case FrameType::MODE_CHANGE: return "MODE_CHANGE";
        case FrameType::ACK:         return "ACK";
        case FrameType::NACK:        return "NACK";
        case FrameType::TURNOVER:    return "TURNOVER";
        case FrameType::TURN_REQUEST: return "TURN_REQUEST";
        case FrameType::FILE_CANCEL: return "FILE_CANCEL";
        case FrameType::BEACON:      return "BEACON";
        case FrameType::DATA:        return "DATA";
        case FrameType::DATA_START:  return "DATA_START";
        case FrameType::DATA_CONT:   return "DATA_CONT";
        case FrameType::DATA_END:    return "DATA_END";
        case FrameType::DATA_REPAIR: return "DATA_REPAIR";
        default:                     return "UNKNOWN";
    }
}

// ============================================================================
// CRC-16 CCITT (same as v1 for compatibility)
// ============================================================================
uint16_t ControlFrame::calculateCRC(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

// ============================================================================
// PHYMaskHeader implementation
// ============================================================================

Bytes PHYMaskHeader::serialize() const {
    Bytes out(SIZE, 0);

    out[0] = MAGIC0;
    out[1] = MAGIC1;
    out[2] = packVersionScheme(version, scheme);
    out[3] = flags;
    out[4] = payload_profile;
    out[5] = interleaver_id;
    out[6] = mask_count;
    out[7] = reserved;

    for (size_t i = 0; i < sizeof(active_mask); ++i) {
        out[8 + i] = static_cast<uint8_t>((active_mask >> (8 * i)) & 0xFF);
    }

    const uint16_t crc = ControlFrame::calculateCRC(out.data(), 16);
    const uint16_t inverted_crc = static_cast<uint16_t>(crc ^ 0xFFFF);
    out[16] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    out[17] = static_cast<uint8_t>(crc & 0xFF);
    out[18] = static_cast<uint8_t>((inverted_crc >> 8) & 0xFF);
    out[19] = static_cast<uint8_t>(inverted_crc & 0xFF);

    return out;
}

bool PHYMaskHeader::validateFields() const {
    if (version != VERSION_V1 || scheme != SCHEME_BITMAP_INTERLEAVER_V1) {
        return false;
    }
    if (flags != 0 || reserved != 0) {
        return false;
    }
    if (interleaver_id != INTERLEAVER_CARRIER_LDPC_V1) {
        return false;
    }
    if (mask_count < MIN_MASK_COUNT || mask_count > MAX_MASK_COUNT) {
        return false;
    }
    if ((active_mask & ~ACTIVE_CARRIER_MASK) != 0) {
        return false;
    }
    return std::popcount(active_mask) == DATA_CARRIER_COUNT - mask_count;
}

bool PHYMaskHeader::validate(ByteSpan data) {
    return deserialize(data).has_value();
}

std::optional<PHYMaskHeader> PHYMaskHeader::deserialize(ByteSpan data) {
    if (data.size() < SIZE) {
        return std::nullopt;
    }

    if (data[0] != MAGIC0 || data[1] != MAGIC1) {
        return std::nullopt;
    }

    const uint16_t received_crc = (static_cast<uint16_t>(data[16]) << 8) | data[17];
    const uint16_t calculated_crc = ControlFrame::calculateCRC(data.data(), 16);
    if (received_crc != calculated_crc) {
        return std::nullopt;
    }

    const uint16_t received_inverted_crc = (static_cast<uint16_t>(data[18]) << 8) | data[19];
    if (received_inverted_crc != static_cast<uint16_t>(received_crc ^ 0xFFFF)) {
        return std::nullopt;
    }

    PHYMaskHeader h;
    h.version = static_cast<uint8_t>((data[2] >> 4) & 0x0F);
    h.scheme = static_cast<uint8_t>(data[2] & 0x0F);
    h.flags = data[3];
    h.payload_profile = data[4];
    h.interleaver_id = data[5];
    h.mask_count = data[6];
    h.reserved = data[7];
    h.active_mask = 0;
    for (size_t i = 0; i < sizeof(h.active_mask); ++i) {
        h.active_mask |= static_cast<uint64_t>(data[8 + i]) << (8 * i);
    }
    h.crc16 = received_crc;
    h.inverted_crc16 = received_inverted_crc;

    if (!h.validateFields()) {
        return std::nullopt;
    }
    return h;
}

// ============================================================================
// ControlFrame implementation
// ============================================================================

ControlFrame ControlFrame::makeProbe(const std::string& src, const std::string& dst) {
    ControlFrame f;
    f.type = FrameType::PROBE;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    return f;
}

ControlFrame ControlFrame::makeProbeAck(const std::string& src, const std::string& dst,
                                         uint8_t snr_db, uint8_t recommended_rate) {
    ControlFrame f;
    f.type = FrameType::PROBE_ACK;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    f.payload[0] = snr_db;
    f.payload[1] = recommended_rate;
    return f;
}

ControlFrame ControlFrame::makeAck(const std::string& src, const std::string& dst, uint16_t seq) {
    ControlFrame f;
    f.type = FrameType::ACK;
    f.flags = Flags::VERSION_V2;
    f.seq = seq;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    return f;
}

ControlFrame ControlFrame::makeNack(const std::string& src, const std::string& dst,
                                     uint16_t seq, uint32_t cw_bitmap) {
    ControlFrame f;
    f.type = FrameType::NACK;
    f.flags = Flags::VERSION_V2;
    f.seq = seq;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);

    NackPayload np;
    np.frame_seq = seq;
    np.cw_bitmap = cw_bitmap;
    np.encode(f.payload);

    return f;
}

ControlFrame ControlFrame::makeTurnover(const std::string& src, const std::string& dst) {
    ControlFrame f;
    f.type = FrameType::TURNOVER;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    return f;
}

ControlFrame ControlFrame::makeTurnRequest(const std::string& src, const std::string& dst) {
    ControlFrame f;
    f.type = FrameType::TURN_REQUEST;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    return f;
}

ControlFrame ControlFrame::makeFileCancel(const std::string& src, const std::string& dst) {
    ControlFrame f;
    f.type = FrameType::FILE_CANCEL;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    return f;
}

ControlFrame ControlFrame::makeBurstHeader(const std::string& src, const std::string& dst,
                                           uint16_t seq, uint8_t group_size,
                                           uint8_t cw_per_frame, Modulation mod, CodeRate rate,
                                           uint8_t interleave_flags) {
    ControlFrame f;
    f.type = FrameType::BURST_HEADER;
    f.flags = Flags::VERSION_V2;
    f.seq = seq;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    f.payload[0] = group_size;
    f.payload[1] = cw_per_frame;
    f.payload[2] = static_cast<uint8_t>(mod);
    f.payload[3] = static_cast<uint8_t>(rate);
    f.payload[4] = interleave_flags;
    f.payload[5] = 0;  // reserved
    return f;
}

ControlFrame ControlFrame::makeGroupAck(const std::string& src, const std::string& dst,
                                        uint16_t group_seq) {
    ControlFrame f;
    f.type = FrameType::GROUP_ACK;
    f.flags = Flags::VERSION_V2;
    f.seq = group_seq;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    f.payload[0] = static_cast<uint8_t>(group_seq & 0xFF);
    f.payload[1] = static_cast<uint8_t>((group_seq >> 8) & 0xFF);
    return f;
}

ControlFrame ControlFrame::makeBeacon(const std::string& src) {
    ControlFrame f;
    f.type = FrameType::BEACON;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = 0xFFFFFF;  // Broadcast
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    return f;
}

ControlFrame ControlFrame::makeKeepalive(const std::string& src, const std::string& dst) {
    ControlFrame f;
    f.type = FrameType::KEEPALIVE;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    return f;
}

ControlFrame ControlFrame::makeDisconnect(const std::string& src, const std::string& dst) {
    ControlFrame f;
    f.type = FrameType::DISCONNECT;
    f.flags = Flags::VERSION_V2;
    f.seq = DISCONNECT_SEQ;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    return f;
}

ControlFrame ControlFrame::makeModeChange(const std::string& src, const std::string& dst,
                                           uint16_t seq, Modulation new_mod, CodeRate new_rate,
                                           float snr_db, float fading_index, uint8_t reason,
                                           uint8_t cw_count,
                                           LadderRungId rung_id) {
    ControlFrame f;
    f.type = FrameType::MODE_CHANGE;
    f.flags = Flags::VERSION_V2;
    f.seq = seq;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    f.payload[0] = static_cast<uint8_t>(new_mod);
    f.payload[1] = static_cast<uint8_t>(new_rate);
    f.payload[2] = encodeSNR(snr_db);
    f.payload[3] = reason;
    f.payload[4] = encodeFadingIndex(fading_index);
    f.payload[5] = packCWCountAndRung(cw_count, rung_id);
    return f;
}

ControlFrame ControlFrame::makeModeChangeByHash(const std::string& src, uint32_t dst_hash,
                                                 uint16_t seq, Modulation new_mod, CodeRate new_rate,
                                                 float snr_db, float fading_index, uint8_t reason,
                                                 uint8_t cw_count,
                                                 LadderRungId rung_id) {
    ControlFrame f;
    f.type = FrameType::MODE_CHANGE;
    f.flags = Flags::VERSION_V2;
    f.seq = seq;
    f.src_hash = hashCallsign(src);
    f.dst_hash = dst_hash & 0xFFFFFF;
    f.payload[0] = static_cast<uint8_t>(new_mod);
    f.payload[1] = static_cast<uint8_t>(new_rate);
    f.payload[2] = encodeSNR(snr_db);
    f.payload[3] = reason;
    f.payload[4] = encodeFadingIndex(fading_index);
    f.payload[5] = packCWCountAndRung(cw_count, rung_id);
    return f;
}

ControlFrame ControlFrame::makeConnect(const std::string& src, const std::string& dst,
                                        uint8_t mode_capabilities, uint8_t preferred_mode) {
    ControlFrame f;
    f.type = FrameType::CONNECT;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    f.payload[0] = mode_capabilities;  // Our supported modes
    f.payload[1] = preferred_mode;     // Our preferred mode
    return f;
}

ControlFrame ControlFrame::makeConnectAck(const std::string& src, const std::string& dst,
                                           uint8_t negotiated_mode) {
    ControlFrame f;
    f.type = FrameType::CONNECT_ACK;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    f.payload[0] = negotiated_mode;
    return f;
}

ControlFrame ControlFrame::makeConnectNak(const std::string& src, const std::string& dst) {
    ControlFrame f;
    f.type = FrameType::CONNECT_NAK;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    return f;
}

// Hash-based factory methods (for responding when callsign is unknown)
ControlFrame ControlFrame::makeProbeAckByHash(const std::string& src, uint32_t dst_hash,
                                               uint8_t snr_db, uint8_t recommended_rate) {
    ControlFrame f;
    f.type = FrameType::PROBE_ACK;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = dst_hash & 0xFFFFFF;  // Ensure 24-bit
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    f.payload[0] = snr_db;
    f.payload[1] = recommended_rate;
    return f;
}

ControlFrame ControlFrame::makeConnectAckByHash(const std::string& src, uint32_t dst_hash,
                                                 uint8_t negotiated_mode) {
    ControlFrame f;
    f.type = FrameType::CONNECT_ACK;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = dst_hash & 0xFFFFFF;
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    f.payload[0] = negotiated_mode;
    return f;
}

ControlFrame ControlFrame::makeConnectNakByHash(const std::string& src, uint32_t dst_hash) {
    ControlFrame f;
    f.type = FrameType::CONNECT_NAK;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = dst_hash & 0xFFFFFF;
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    return f;
}

ControlFrame ControlFrame::makeAckByHash(const std::string& src, uint32_t dst_hash, uint16_t seq) {
    ControlFrame f;
    f.type = FrameType::ACK;
    f.flags = Flags::VERSION_V2;
    f.seq = seq;
    f.src_hash = hashCallsign(src);
    f.dst_hash = dst_hash & 0xFFFFFF;
    std::memset(f.payload, 0, PAYLOAD_SIZE);
    return f;
}

ControlFrame ControlFrame::makeNackByHash(const std::string& src, uint32_t dst_hash,
                                           uint16_t seq, uint32_t cw_bitmap) {
    ControlFrame f;
    f.type = FrameType::NACK;
    f.flags = Flags::VERSION_V2;
    f.seq = seq;
    f.src_hash = hashCallsign(src);
    f.dst_hash = dst_hash & 0xFFFFFF;

    NackPayload np;
    np.frame_seq = seq;
    np.cw_bitmap = cw_bitmap;
    np.encode(f.payload);

    return f;
}

Bytes ControlFrame::serialize() const {
    Bytes out(SIZE);

    // Magic (2 bytes, big-endian)
    out[0] = (MAGIC_V2 >> 8) & 0xFF;
    out[1] = MAGIC_V2 & 0xFF;

    // Type (1 byte)
    out[2] = static_cast<uint8_t>(type);

    // Flags (1 byte)
    out[3] = flags;

    // Sequence (2 bytes, big-endian)
    out[4] = (seq >> 8) & 0xFF;
    out[5] = seq & 0xFF;

    // Source hash (3 bytes, big-endian)
    out[6] = (src_hash >> 16) & 0xFF;
    out[7] = (src_hash >> 8) & 0xFF;
    out[8] = src_hash & 0xFF;

    // Destination hash (3 bytes, big-endian)
    out[9] = (dst_hash >> 16) & 0xFF;
    out[10] = (dst_hash >> 8) & 0xFF;
    out[11] = dst_hash & 0xFF;

    // Payload (6 bytes)
    std::memcpy(out.data() + 12, payload, PAYLOAD_SIZE);

    // CRC16 (2 bytes, big-endian) - over bytes 0-17
    uint16_t crc = calculateCRC(out.data(), SIZE - 2);
    out[18] = (crc >> 8) & 0xFF;
    out[19] = crc & 0xFF;

    return out;
}

std::optional<ControlFrame> ControlFrame::deserialize(ByteSpan data) {
    if (data.size() < SIZE) {
        return std::nullopt;
    }

    // Check magic
    uint16_t magic = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    if (magic != MAGIC_V2) {
        return std::nullopt;
    }

    // Verify CRC
    uint16_t received_crc = (static_cast<uint16_t>(data[18]) << 8) | data[19];
    uint16_t calculated_crc = calculateCRC(data.data(), SIZE - 2);
    if (received_crc != calculated_crc) {
        return std::nullopt;
    }

    ControlFrame f;
    f.type = static_cast<FrameType>(data[2]);
    f.flags = data[3];
    f.seq = (static_cast<uint16_t>(data[4]) << 8) | data[5];
    f.src_hash = (static_cast<uint32_t>(data[6]) << 16) |
                 (static_cast<uint32_t>(data[7]) << 8) |
                 data[8];
    f.dst_hash = (static_cast<uint32_t>(data[9]) << 16) |
                 (static_cast<uint32_t>(data[10]) << 8) |
                 data[11];
    std::memcpy(f.payload, data.data() + 12, PAYLOAD_SIZE);

    return f;
}

// ============================================================================
// DataFrame implementation
// ============================================================================

uint8_t DataFrame::calculateCodewords(size_t payload_size) {
    return calculateCodewords(payload_size, CodeRate::R1_4);
}

uint8_t DataFrame::calculateCodewords(size_t payload_size, CodeRate rate) {
    // Total frame size = header (17) + payload + frame_CRC (2)
    size_t total = HEADER_SIZE + payload_size + CRC_SIZE;

    size_t bytes_per_cw = getBytesPerCodeword(rate);
    if (total <= bytes_per_cw) {
        return 1;
    }

    size_t data_payload_size = bytes_per_cw - DATA_CW_HEADER_SIZE;
    size_t continuation_bytes = total - bytes_per_cw;
    size_t continuation_cw = (continuation_bytes + data_payload_size - 1) / data_payload_size;

    return static_cast<uint8_t>(1 + continuation_cw);
}

DataFrame DataFrame::makeData(const std::string& src, const std::string& dst,
                               uint16_t seq, const Bytes& data) {
    DataFrame f;
    f.type = FrameType::DATA;
    f.flags = Flags::VERSION_V2;
    f.seq = seq;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    f.payload = data;
    f.payload_len = static_cast<uint16_t>(data.size());
    f.total_cw = calculateCodewords(data.size());
    return f;
}

DataFrame DataFrame::makeData(const std::string& src, const std::string& dst,
                               uint16_t seq, const std::string& text) {
    Bytes data(text.begin(), text.end());
    return makeData(src, dst, seq, data);
}

DataFrame DataFrame::makeData(const std::string& src, const std::string& dst,
                               uint16_t seq, const Bytes& data, CodeRate cw1_rate) {
    DataFrame f;
    f.type = FrameType::DATA;
    f.flags = Flags::VERSION_V2;
    f.seq = seq;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);
    f.payload = data;
    f.payload_len = static_cast<uint16_t>(data.size());
    f.total_cw = calculateCodewords(data.size(), cw1_rate);
    return f;
}

DataFrame DataFrame::makeData(const std::string& src, const std::string& dst,
                               uint16_t seq, const std::string& text, CodeRate cw1_rate) {
    Bytes data(text.begin(), text.end());
    return makeData(src, dst, seq, data, cw1_rate);
}

Bytes DataFrame::serialize() const {
    // Total size = header + payload + CRC
    size_t total_size = HEADER_SIZE + payload.size() + CRC_SIZE;
    Bytes out(total_size);

    // Magic (2 bytes, big-endian)
    out[0] = (MAGIC_V2 >> 8) & 0xFF;
    out[1] = MAGIC_V2 & 0xFF;

    // Type (1 byte)
    out[2] = static_cast<uint8_t>(type);

    // Flags (1 byte)
    out[3] = flags;

    // Sequence (2 bytes, big-endian)
    out[4] = (seq >> 8) & 0xFF;
    out[5] = seq & 0xFF;

    // Source hash (3 bytes, big-endian)
    out[6] = (src_hash >> 16) & 0xFF;
    out[7] = (src_hash >> 8) & 0xFF;
    out[8] = src_hash & 0xFF;

    // Destination hash (3 bytes, big-endian)
    out[9] = (dst_hash >> 16) & 0xFF;
    out[10] = (dst_hash >> 8) & 0xFF;
    out[11] = dst_hash & 0xFF;

    // Total codewords (1 byte)
    out[12] = total_cw;

    // Payload length (2 bytes, big-endian)
    out[13] = (payload_len >> 8) & 0xFF;
    out[14] = payload_len & 0xFF;

    // Header CRC (2 bytes) - CRC of bytes 0-14
    uint16_t hcrc = ControlFrame::calculateCRC(out.data(), 15);
    out[15] = (hcrc >> 8) & 0xFF;
    out[16] = hcrc & 0xFF;

    // Payload
    if (!payload.empty()) {
        std::memcpy(out.data() + HEADER_SIZE, payload.data(), payload.size());
    }

    // Frame CRC (2 bytes) - CRC of entire frame except last 2 bytes
    uint16_t fcrc = ControlFrame::calculateCRC(out.data(), total_size - 2);
    out[total_size - 2] = (fcrc >> 8) & 0xFF;
    out[total_size - 1] = fcrc & 0xFF;

    return out;
}

std::optional<DataFrame> DataFrame::deserialize(ByteSpan data) {
    if (data.size() < HEADER_SIZE + CRC_SIZE) {
        return std::nullopt;
    }

    // Check magic
    uint16_t magic = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    if (magic != MAGIC_V2) {
        return std::nullopt;
    }

    // Verify header CRC
    uint16_t received_hcrc = (static_cast<uint16_t>(data[15]) << 8) | data[16];
    uint16_t calculated_hcrc = ControlFrame::calculateCRC(data.data(), 15);
    if (received_hcrc != calculated_hcrc) {
        return std::nullopt;
    }

    // Parse header
    DataFrame f;
    f.type = static_cast<FrameType>(data[2]);
    f.flags = data[3];
    f.seq = (static_cast<uint16_t>(data[4]) << 8) | data[5];
    f.src_hash = (static_cast<uint32_t>(data[6]) << 16) |
                 (static_cast<uint32_t>(data[7]) << 8) |
                 data[8];
    f.dst_hash = (static_cast<uint32_t>(data[9]) << 16) |
                 (static_cast<uint32_t>(data[10]) << 8) |
                 data[11];
    f.total_cw = data[12];
    f.payload_len = (static_cast<uint16_t>(data[13]) << 8) | data[14];

    // Check we have enough data
    size_t expected_size = HEADER_SIZE + f.payload_len + CRC_SIZE;
    if (data.size() < expected_size) {
        return std::nullopt;
    }

    // Verify frame CRC
    uint16_t received_fcrc = (static_cast<uint16_t>(data[expected_size - 2]) << 8) |
                              data[expected_size - 1];
    uint16_t calculated_fcrc = ControlFrame::calculateCRC(data.data(), expected_size - 2);
    if (received_fcrc != calculated_fcrc) {
        return std::nullopt;
    }

    // Extract payload
    if (f.payload_len > 0) {
        f.payload.assign(data.begin() + HEADER_SIZE, data.begin() + HEADER_SIZE + f.payload_len);
    }

    return f;
}

std::string DataFrame::payloadAsText() const {
    return std::string(payload.begin(), payload.end());
}

// ============================================================================
// ConnectFrame implementation (ham-compliant with full callsigns)
// ============================================================================

ConnectFrame ConnectFrame::makeConnect(const std::string& src, const std::string& dst,
                                        uint8_t mode_caps, uint8_t forced_waveform,
                                        uint8_t forced_modulation, uint8_t forced_code_rate,
                                        uint8_t forced_cw_count) {
    ConnectFrame f;
    f.type = FrameType::CONNECT;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);

    // Copy callsigns (null-terminated, max 9 chars)
    std::strncpy(f.src_callsign, src.c_str(), MAX_CALLSIGN_LEN - 1);
    f.src_callsign[MAX_CALLSIGN_LEN - 1] = '\0';
    std::strncpy(f.dst_callsign, dst.c_str(), MAX_CALLSIGN_LEN - 1);
    f.dst_callsign[MAX_CALLSIGN_LEN - 1] = '\0';

    f.mode_capabilities = mode_caps;
    f.negotiated_mode = forced_waveform;        // 0xFF = AUTO, else forced
    f.initial_modulation = forced_modulation;   // 0xFF = AUTO, else forced
    f.initial_code_rate = forced_code_rate;     // 0xFF = AUTO, else forced
    f.measured_snr = 0;                         // Not used in CONNECT
    f.data_frame_cw_count = forced_cw_count;    // 0=AUTO, else 1..8 forced
    f.ladder_rung_id = LadderRungId::UNKNOWN;
    f.phy_mask_v1_capability = ultra::protocol::hasPhyMaskV1Capability(f.mode_capabilities);
    return f;
}

ConnectFrame ConnectFrame::makeConnectAck(const std::string& src, const std::string& dst,
                                           uint8_t neg_mode, Modulation init_mod, CodeRate init_rate,
                                           float snr_db, float fading_index,
                                           uint8_t cw_count,
                                           LadderRungId rung_id) {
    ConnectFrame f;
    f.type = FrameType::CONNECT_ACK;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);

    std::strncpy(f.src_callsign, src.c_str(), MAX_CALLSIGN_LEN - 1);
    f.src_callsign[MAX_CALLSIGN_LEN - 1] = '\0';
    std::strncpy(f.dst_callsign, dst.c_str(), MAX_CALLSIGN_LEN - 1);
    f.dst_callsign[MAX_CALLSIGN_LEN - 1] = '\0';

    // CONNECT_ACK reuses this byte to carry responder fading index.
    f.mode_capabilities = encodeFadingIndex(fading_index);
    f.negotiated_mode = neg_mode;

    // Initial data mode - eliminates separate MODE_CHANGE after connect
    f.initial_modulation = static_cast<uint8_t>(init_mod);
    f.initial_code_rate = static_cast<uint8_t>(init_rate);
    f.measured_snr = encodeSNR(snr_db);
    f.data_frame_cw_count = cw_count;  // Final negotiated CW count (1..8)
    f.ladder_rung_id = rung_id;
    f.phy_mask_v1_capability = false;
    return f;
}

ConnectFrame ConnectFrame::makeConnectNak(const std::string& src, const std::string& dst) {
    ConnectFrame f;
    f.type = FrameType::CONNECT_NAK;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);

    std::strncpy(f.src_callsign, src.c_str(), MAX_CALLSIGN_LEN - 1);
    f.src_callsign[MAX_CALLSIGN_LEN - 1] = '\0';
    std::strncpy(f.dst_callsign, dst.c_str(), MAX_CALLSIGN_LEN - 1);
    f.dst_callsign[MAX_CALLSIGN_LEN - 1] = '\0';

    f.mode_capabilities = 0;
    f.negotiated_mode = 0;
    f.ladder_rung_id = LadderRungId::UNKNOWN;
    f.phy_mask_v1_capability = false;
    return f;
}

ConnectFrame ConnectFrame::makeDisconnect(const std::string& src, const std::string& dst) {
    ConnectFrame f;
    f.type = FrameType::DISCONNECT;
    f.flags = Flags::VERSION_V2;
    f.seq = DISCONNECT_SEQ;  // Unique seq to distinguish from data ACKs
    f.src_hash = hashCallsign(src);
    f.dst_hash = hashCallsign(dst);

    std::strncpy(f.src_callsign, src.c_str(), MAX_CALLSIGN_LEN - 1);
    f.src_callsign[MAX_CALLSIGN_LEN - 1] = '\0';
    std::strncpy(f.dst_callsign, dst.c_str(), MAX_CALLSIGN_LEN - 1);
    f.dst_callsign[MAX_CALLSIGN_LEN - 1] = '\0';

    f.mode_capabilities = 0;
    f.negotiated_mode = 0;
    f.phy_mask_v1_capability = false;
    return f;
}

ConnectFrame ConnectFrame::makeConnectAckByHash(const std::string& src, uint32_t dst_hash,
                                                 uint8_t neg_mode, Modulation init_mod, CodeRate init_rate,
                                                 float snr_db, float fading_index,
                                                 uint8_t cw_count,
                                                 LadderRungId rung_id) {
    ConnectFrame f;
    f.type = FrameType::CONNECT_ACK;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = dst_hash & 0xFFFFFF;

    std::strncpy(f.src_callsign, src.c_str(), MAX_CALLSIGN_LEN - 1);
    f.src_callsign[MAX_CALLSIGN_LEN - 1] = '\0';
    // dst_callsign unknown - leave empty (will be filled from received CONNECT)
    f.dst_callsign[0] = '\0';

    f.mode_capabilities = encodeFadingIndex(fading_index);
    f.negotiated_mode = neg_mode;

    // Initial data mode - eliminates separate MODE_CHANGE after connect
    f.initial_modulation = static_cast<uint8_t>(init_mod);
    f.initial_code_rate = static_cast<uint8_t>(init_rate);
    f.measured_snr = encodeSNR(snr_db);
    f.data_frame_cw_count = cw_count;  // Final negotiated CW count (1..8)
    f.ladder_rung_id = rung_id;
    f.phy_mask_v1_capability = false;
    return f;
}

ConnectFrame ConnectFrame::makeConnectNakByHash(const std::string& src, uint32_t dst_hash) {
    ConnectFrame f;
    f.type = FrameType::CONNECT_NAK;
    f.flags = Flags::VERSION_V2;
    f.seq = 0;
    f.src_hash = hashCallsign(src);
    f.dst_hash = dst_hash & 0xFFFFFF;

    std::strncpy(f.src_callsign, src.c_str(), MAX_CALLSIGN_LEN - 1);
    f.src_callsign[MAX_CALLSIGN_LEN - 1] = '\0';
    f.dst_callsign[0] = '\0';

    f.mode_capabilities = 0;
    f.negotiated_mode = 0;
    f.phy_mask_v1_capability = false;
    return f;
}

Bytes ConnectFrame::serialize() const {
    // Use DATA frame format: header (17B) + payload (25B) + CRC (2B) = 44 bytes
    // Always uses 4 codewords with frame-level interleaving for fading resistance
    Bytes out;
    out.reserve(DataFrame::HEADER_SIZE + PAYLOAD_SIZE + DataFrame::CRC_SIZE);

    // Magic (2 bytes)
    out.push_back((MAGIC_V2 >> 8) & 0xFF);
    out.push_back(MAGIC_V2 & 0xFF);

    // Type, flags, seq (4 bytes)
    out.push_back(static_cast<uint8_t>(type));
    out.push_back(flags);
    out.push_back((seq >> 8) & 0xFF);
    out.push_back(seq & 0xFF);

    // Hashes (6 bytes)
    out.push_back((src_hash >> 16) & 0xFF);
    out.push_back((src_hash >> 8) & 0xFF);
    out.push_back(src_hash & 0xFF);
    out.push_back((dst_hash >> 16) & 0xFF);
    out.push_back((dst_hash >> 8) & 0xFF);
    out.push_back(dst_hash & 0xFF);

    // Total codewords (1 byte) - default fixed-frame count for compatibility
    out.push_back(kDefaultFixedFrameCodewords);

    // Payload length (2 bytes)
    out.push_back((PAYLOAD_SIZE >> 8) & 0xFF);
    out.push_back(PAYLOAD_SIZE & 0xFF);

    // Header CRC (2 bytes)
    uint16_t hcrc = ControlFrame::calculateCRC(out.data(), out.size());
    out.push_back((hcrc >> 8) & 0xFF);
    out.push_back(hcrc & 0xFF);

    // Payload: src_callsign (10B) + dst_callsign (10B) + caps (1B) + wfmode (1B)
    //          + mod (1B) + rate (1B) + snr (1B) + cw_count (1B) = 26B
    for (int i = 0; i < MAX_CALLSIGN_LEN; i++) {
        out.push_back(static_cast<uint8_t>(src_callsign[i]));
    }
    for (int i = 0; i < MAX_CALLSIGN_LEN; i++) {
        out.push_back(static_cast<uint8_t>(dst_callsign[i]));
    }
    out.push_back(mode_capabilities);
    out.push_back(negotiated_mode);
    out.push_back(initial_modulation);
    out.push_back(initial_code_rate);
    out.push_back(measured_snr);
    const uint8_t cw_count_wire = packCWCountAndRung(
        data_frame_cw_count,
        type == FrameType::CONNECT_ACK ? ladder_rung_id : LadderRungId::UNKNOWN,
        type == FrameType::CONNECT_ACK && phy_mask_v1_capability);
    out.push_back(cw_count_wire);

    // Frame CRC (2 bytes)
    uint16_t fcrc = ControlFrame::calculateCRC(out.data(), out.size());
    out.push_back((fcrc >> 8) & 0xFF);
    out.push_back(fcrc & 0xFF);

    return out;
}

std::optional<ConnectFrame> ConnectFrame::deserialize(ByteSpan data) {
    constexpr size_t MIN_SIZE = DataFrame::HEADER_SIZE + PAYLOAD_SIZE + DataFrame::CRC_SIZE;
    if (data.size() < MIN_SIZE) {
        return std::nullopt;
    }

    // Verify magic
    uint16_t magic = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    if (magic != MAGIC_V2) {
        return std::nullopt;
    }

    // Check type is CONNECT, CONNECT_ACK, CONNECT_NAK, or DISCONNECT
    FrameType ftype = static_cast<FrameType>(data[2]);
    if (!isConnectFrame(ftype)) {
        return std::nullopt;
    }

    // Verify header CRC
    uint16_t stored_hcrc = (static_cast<uint16_t>(data[15]) << 8) | data[16];
    uint16_t calc_hcrc = ControlFrame::calculateCRC(data.data(), 15);
    if (stored_hcrc != calc_hcrc) {
        return std::nullopt;
    }

    // Verify frame CRC
    size_t fcrc_offset = DataFrame::HEADER_SIZE + PAYLOAD_SIZE;
    uint16_t stored_fcrc = (static_cast<uint16_t>(data[fcrc_offset]) << 8) | data[fcrc_offset + 1];
    uint16_t calc_fcrc = ControlFrame::calculateCRC(data.data(), fcrc_offset);
    if (stored_fcrc != calc_fcrc) {
        return std::nullopt;
    }

    ConnectFrame f;
    f.type = ftype;
    f.flags = data[3];
    f.seq = (static_cast<uint16_t>(data[4]) << 8) | data[5];
    f.src_hash = (static_cast<uint32_t>(data[6]) << 16) |
                 (static_cast<uint32_t>(data[7]) << 8) |
                 data[8];
    f.dst_hash = (static_cast<uint32_t>(data[9]) << 16) |
                 (static_cast<uint32_t>(data[10]) << 8) |
                 data[11];

    // Parse payload (starts at offset 17)
    size_t payload_offset = DataFrame::HEADER_SIZE;
    for (int i = 0; i < MAX_CALLSIGN_LEN; i++) {
        f.src_callsign[i] = static_cast<char>(data[payload_offset + i]);
    }
    f.src_callsign[MAX_CALLSIGN_LEN - 1] = '\0';  // Ensure null-terminated

    for (int i = 0; i < MAX_CALLSIGN_LEN; i++) {
        f.dst_callsign[i] = static_cast<char>(data[payload_offset + MAX_CALLSIGN_LEN + i]);
    }
    f.dst_callsign[MAX_CALLSIGN_LEN - 1] = '\0';

    size_t field_offset = payload_offset + 2 * MAX_CALLSIGN_LEN;
    f.mode_capabilities = data[field_offset];
    f.negotiated_mode = data[field_offset + 1];
    f.initial_modulation = data[field_offset + 2];
    f.initial_code_rate = data[field_offset + 3];
    f.measured_snr = data[field_offset + 4];
    const uint8_t cw_count_wire = data[field_offset + 5];
    if (f.type == FrameType::CONNECT_ACK) {
        f.phy_mask_v1_capability = (cw_count_wire & ModeCapabilities::PHY_MASK_V1) != 0;
        f.data_frame_cw_count = unpackCWCount(cw_count_wire);
        f.ladder_rung_id = unpackLadderRungId(cw_count_wire);
    } else {
        f.data_frame_cw_count = unpackCWCount(cw_count_wire);
        f.ladder_rung_id = LadderRungId::UNKNOWN;
        f.phy_mask_v1_capability = ultra::protocol::hasPhyMaskV1Capability(f.mode_capabilities);
    }

    return f;
}

std::string ConnectFrame::getSrcCallsign() const {
    return std::string(src_callsign);
}

std::string ConnectFrame::getDstCallsign() const {
    return std::string(dst_callsign);
}

bool hasPhyMaskV1Capability(const ConnectFrame& frame) {
    if (frame.type == FrameType::CONNECT_ACK) {
        return frame.phy_mask_v1_capability;
    }
    return ultra::protocol::hasPhyMaskV1Capability(frame.mode_capabilities);
}

void setPhyMaskV1Capability(ConnectFrame& frame) {
    if (frame.type == FrameType::CONNECT_ACK) {
        frame.phy_mask_v1_capability = true;
        return;
    }
    frame.mode_capabilities = ultra::protocol::setPhyMaskV1Capability(frame.mode_capabilities);
    frame.phy_mask_v1_capability = true;
}

// ============================================================================
// NackPayload implementation
// ============================================================================

void NackPayload::encode(uint8_t* out) const {
    // Frame sequence (2 bytes)
    out[0] = (frame_seq >> 8) & 0xFF;
    out[1] = frame_seq & 0xFF;

    // Codeword bitmap (4 bytes)
    out[2] = (cw_bitmap >> 24) & 0xFF;
    out[3] = (cw_bitmap >> 16) & 0xFF;
    out[4] = (cw_bitmap >> 8) & 0xFF;
    out[5] = cw_bitmap & 0xFF;
}

NackPayload NackPayload::decode(const uint8_t* in) {
    NackPayload np;
    np.frame_seq = (static_cast<uint16_t>(in[0]) << 8) | in[1];
    np.cw_bitmap = (static_cast<uint32_t>(in[2]) << 24) |
                   (static_cast<uint32_t>(in[3]) << 16) |
                   (static_cast<uint32_t>(in[4]) << 8) |
                   in[5];
    return np;
}

int NackPayload::countFailed() const {
    int count = 0;
    uint32_t b = cw_bitmap;
    while (b) {
        count += b & 1;
        b >>= 1;
    }
    return count;
}

// ============================================================================
// DataRepairFrame implementation
// ============================================================================

namespace {

uint8_t countBits16(uint16_t value) {
    uint8_t count = 0;
    while (value != 0) {
        count += static_cast<uint8_t>(value & 1u);
        value >>= 1;
    }
    return count;
}

} // namespace

DataRepairFrame DataRepairFrame::make(const std::string& src, const std::string& dst,
                                      uint16_t target_seq, uint8_t original_total_cw,
                                      uint32_t repair_bitmap, CodeRate rate,
                                      const std::vector<Bytes>& repair_codewords) {
    DataRepairFrame frame;
    frame.flags = Flags::VERSION_V2;
    frame.target_seq = target_seq;
    frame.src_hash = hashCallsign(src);
    frame.dst_hash = hashCallsign(dst);
    frame.original_total_cw = original_total_cw;
    frame.repair_bitmap = static_cast<uint16_t>(repair_bitmap & 0xFFFFu);
    frame.repair_count = static_cast<uint8_t>(repair_codewords.size());
    frame.rate = rate;
    frame.repair_codewords = repair_codewords;
    return frame;
}

bool DataRepairFrame::valid() const {
    if (original_total_cw == 0 || original_total_cw > MAX_REPAIR_CW) {
        return false;
    }
    const uint16_t expected_mask = original_total_cw >= 16
        ? 0xFFFFu
        : static_cast<uint16_t>((1u << original_total_cw) - 1u);
    if (repair_bitmap == 0 || (repair_bitmap & ~expected_mask) != 0) {
        return false;
    }
    if (repair_count == 0 || repair_count != countBits16(repair_bitmap)) {
        return false;
    }
    if (repair_codewords.size() != repair_count) {
        return false;
    }
    const size_t bytes_per_cw = getBytesPerCodeword(rate);
    if (bytes_per_cw < HEADER_BYTES) {
        return false;
    }
    for (const auto& cw : repair_codewords) {
        if (cw.empty() || cw.size() > bytes_per_cw) {
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> DataRepairFrame::repairIndices() const {
    std::vector<uint8_t> indices;
    for (uint8_t i = 0; i < original_total_cw && i < MAX_REPAIR_CW; ++i) {
        if ((repair_bitmap & (1u << i)) != 0) {
            indices.push_back(i);
        }
    }
    return indices;
}

Bytes DataRepairFrame::headerCodeword() const {
    const size_t bytes_per_cw = std::max(getBytesPerCodeword(rate), HEADER_BYTES);
    Bytes out(bytes_per_cw, 0);

    out[0] = (MAGIC_V2 >> 8) & 0xFF;
    out[1] = MAGIC_V2 & 0xFF;
    out[2] = static_cast<uint8_t>(FrameType::DATA_REPAIR);
    out[3] = flags;
    out[4] = (target_seq >> 8) & 0xFF;
    out[5] = target_seq & 0xFF;
    out[6] = (src_hash >> 16) & 0xFF;
    out[7] = (src_hash >> 8) & 0xFF;
    out[8] = src_hash & 0xFF;
    out[9] = (dst_hash >> 16) & 0xFF;
    out[10] = (dst_hash >> 8) & 0xFF;
    out[11] = dst_hash & 0xFF;
    out[12] = original_total_cw;
    out[13] = (repair_bitmap >> 8) & 0xFF;
    out[14] = repair_bitmap & 0xFF;
    out[15] = repair_count;
    out[16] = static_cast<uint8_t>(rate);
    out[17] = 0;  // reserved

    const uint16_t crc = ControlFrame::calculateCRC(out.data(), 18);
    out[18] = (crc >> 8) & 0xFF;
    out[19] = crc & 0xFF;
    return out;
}

std::vector<Bytes> DataRepairFrame::infoCodewords() const {
    std::vector<Bytes> codewords;
    if (!valid()) {
        return codewords;
    }
    const size_t bytes_per_cw = getBytesPerCodeword(rate);
    codewords.push_back(headerCodeword());
    for (auto cw : repair_codewords) {
        cw.resize(bytes_per_cw, 0);
        codewords.push_back(std::move(cw));
    }
    return codewords;
}

Bytes DataRepairFrame::serialize() const {
    Bytes out;
    auto codewords = infoCodewords();
    const size_t bytes_per_cw = getBytesPerCodeword(rate);
    out.reserve(codewords.size() * bytes_per_cw);
    for (const auto& cw : codewords) {
        out.insert(out.end(), cw.begin(), cw.end());
    }
    return out;
}

std::optional<DataRepairFrame> DataRepairFrame::parseHeader(ByteSpan first_codeword) {
    if (first_codeword.size() < HEADER_BYTES) {
        return std::nullopt;
    }
    const uint16_t magic = (static_cast<uint16_t>(first_codeword[0]) << 8) | first_codeword[1];
    if (magic != MAGIC_V2 || first_codeword[2] != static_cast<uint8_t>(FrameType::DATA_REPAIR)) {
        return std::nullopt;
    }
    const uint16_t received_crc =
        (static_cast<uint16_t>(first_codeword[18]) << 8) | first_codeword[19];
    const uint16_t calculated_crc = ControlFrame::calculateCRC(first_codeword.data(), 18);
    if (received_crc != calculated_crc) {
        return std::nullopt;
    }

    DataRepairFrame frame;
    frame.flags = first_codeword[3];
    frame.target_seq = (static_cast<uint16_t>(first_codeword[4]) << 8) | first_codeword[5];
    frame.src_hash = (static_cast<uint32_t>(first_codeword[6]) << 16) |
                     (static_cast<uint32_t>(first_codeword[7]) << 8) |
                     first_codeword[8];
    frame.dst_hash = (static_cast<uint32_t>(first_codeword[9]) << 16) |
                     (static_cast<uint32_t>(first_codeword[10]) << 8) |
                     first_codeword[11];
    frame.original_total_cw = first_codeword[12];
    frame.repair_bitmap = (static_cast<uint16_t>(first_codeword[13]) << 8) |
                          first_codeword[14];
    frame.repair_count = first_codeword[15];
    frame.rate = static_cast<CodeRate>(first_codeword[16]);
    return frame;
}

std::optional<DataRepairFrame> DataRepairFrame::deserialize(ByteSpan data) {
    auto frame = parseHeader(data);
    if (!frame) {
        return std::nullopt;
    }
    const size_t bytes_per_cw = getBytesPerCodeword(frame->rate);
    if (bytes_per_cw < HEADER_BYTES) {
        return std::nullopt;
    }
    const size_t expected_size =
        static_cast<size_t>(frame->repair_count + 1) * bytes_per_cw;
    if (data.size() < expected_size) {
        return std::nullopt;
    }

    frame->repair_codewords.clear();
    frame->repair_codewords.reserve(frame->repair_count);
    for (uint8_t i = 0; i < frame->repair_count; ++i) {
        const size_t offset = static_cast<size_t>(i + 1) * bytes_per_cw;
        frame->repair_codewords.emplace_back(data.begin() + offset,
                                             data.begin() + offset + bytes_per_cw);
    }
    if (!frame->valid()) {
        return std::nullopt;
    }
    return frame;
}

// ============================================================================
// Codeword helpers
// ============================================================================

std::vector<Bytes> splitIntoCodewords(const Bytes& frame_data) {
    return splitIntoCodewords(frame_data, CodeRate::R1_4);
}

std::vector<Bytes> splitIntoCodewords(const Bytes& frame_data, CodeRate rate) {
    std::vector<Bytes> codewords;
    const size_t bytes_per_cw = getBytesPerCodeword(rate);
    const size_t data_payload_size = bytes_per_cw - DATA_CW_HEADER_SIZE;

    // CW0: First 20 bytes of frame data (contains header with 0x554C magic)
    // No modification needed - the magic already identifies it
    {
        Bytes cw0(bytes_per_cw, 0);  // Zero-pad if needed
        size_t cw0_data = std::min(bytes_per_cw, frame_data.size());
        std::memcpy(cw0.data(), frame_data.data(), cw0_data);
        codewords.push_back(std::move(cw0));
    }

    // CW1+: Add marker + index header before payload data
    size_t offset = bytes_per_cw;  // Start after CW0's data
    uint8_t cw_index = 1;

    while (offset < frame_data.size()) {
        Bytes cw(bytes_per_cw, 0);  // Zero-pad if needed

        // Add marker and index
        cw[0] = DATA_CW_MARKER;
        cw[1] = cw_index;

        // Copy payload data (up to 18 bytes)
        size_t remaining = frame_data.size() - offset;
        size_t chunk_size = std::min(data_payload_size, remaining);
        std::memcpy(cw.data() + DATA_CW_HEADER_SIZE, frame_data.data() + offset, chunk_size);

        codewords.push_back(std::move(cw));
        offset += data_payload_size;  // Each CW1+ consumes payload bytes after marker
        cw_index++;
    }

    return codewords;
}

Bytes reassembleCodewords(const std::vector<Bytes>& codewords, size_t expected_size) {
    Bytes result;
    result.reserve(expected_size);

    for (size_t i = 0; i < codewords.size(); i++) {
        size_t remaining = expected_size - result.size();
        if (remaining == 0) break;

        if (i == 0) {
            // CW0: All 20 bytes are frame data (header + payload start)
            size_t to_copy = std::min(remaining, codewords[i].size());
            result.insert(result.end(), codewords[i].begin(), codewords[i].begin() + to_copy);
        } else {
            // CW1+: Skip marker (0xD5) and index, copy payload portion
            // Verify marker byte (optional - for robustness)
            if (codewords[i].size() >= DATA_CW_HEADER_SIZE && codewords[i][0] == DATA_CW_MARKER) {
                size_t payload_size = codewords[i].size() - DATA_CW_HEADER_SIZE;
                size_t to_copy = std::min(remaining, payload_size);
                result.insert(result.end(),
                              codewords[i].begin() + DATA_CW_HEADER_SIZE,
                              codewords[i].begin() + DATA_CW_HEADER_SIZE + to_copy);
            } else {
                // Fallback: old format without marker (backward compatibility during transition)
                size_t to_copy = std::min(remaining, codewords[i].size());
                result.insert(result.end(), codewords[i].begin(), codewords[i].begin() + to_copy);
            }
        }
    }

    return result;
}

Bytes reassembleFixedCodewords(const std::vector<Bytes>& codewords, size_t expected_size) {
    Bytes result;
    result.reserve(expected_size);

    for (const auto& cw : codewords) {
        size_t remaining = expected_size - result.size();
        if (remaining == 0) break;

        size_t to_copy = std::min(remaining, cw.size());
        result.insert(result.end(), cw.begin(), cw.begin() + to_copy);
    }

    return result;
}

uint32_t CodewordStatus::getNackBitmap() const {
    uint32_t bitmap = 0;
    for (size_t i = 0; i < decoded.size() && i < 32; i++) {
        if (!decoded[i]) {
            bitmap |= (1u << i);
        }
    }
    return bitmap;
}

bool CodewordStatus::allSuccess() const {
    for (bool d : decoded) {
        if (!d) return false;
    }
    return true;
}

int CodewordStatus::countFailures() const {
    int count = 0;
    for (bool d : decoded) {
        if (!d) count++;
    }
    return count;
}

uint8_t CodewordStatus::getExpectedCodewords() const {
    if (decoded.empty() || !decoded[0] || data.empty() || data[0].size() < 20) {
        return 0;
    }

    // Parse header from first codeword
    auto info = parseHeader(data[0]);
    if (!info.valid) {
        return 0;
    }

    return info.total_cw;
}

Bytes CodewordStatus::reassemble() const {
    if (decoded.empty() || !decoded[0] || data.empty()) {
        LOG_MODEM(WARN, "reassemble: early return (decoded=%zu, data=%zu)",
                  decoded.size(), data.size());
        return {};
    }

    // Parse header to know total size
    auto info = parseHeader(data[0]);
    if (!info.valid) {
        // Log all 20 bytes to diagnose LDPC false positives vs bit errors
        if (data[0].size() >= 17) {
            LOG_MODEM(WARN, "reassemble: header invalid, CW0[0..16]: "
                      "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X (size=%zu)",
                      data[0][0], data[0][1], data[0][2], data[0][3],
                      data[0][4], data[0][5], data[0][6], data[0][7],
                      data[0][8], data[0][9], data[0][10], data[0][11],
                      data[0][12], data[0][13], data[0][14], data[0][15],
                      data[0][16], data[0].size());
        }
        return {};
    }

    // Calculate expected frame size
    size_t expected_size;
    if (info.is_control) {
        expected_size = ControlFrame::SIZE;  // 20 bytes
    } else {
        expected_size = DataFrame::HEADER_SIZE + info.payload_len + DataFrame::CRC_SIZE;
    }

    // Fixed OFDM frames pack raw info bytes contiguously across all fixed CWs.
    // Do not run them through the marker-aware variable-CW reassembler: a real
    // payload byte of 0xD5 at a CW boundary would be mistaken for a marker.
    if (!info.is_control && fixed_frame &&
        info.total_cw >= kMinFixedFrameCodewords &&
        info.total_cw <= kMaxFixedFrameCodewords) {
        return reassembleFixedCodewords(data, expected_size);
    }

    return reassembleCodewords(data, expected_size);
}

bool CodewordStatus::mergeCodeword(size_t index, const Bytes& cw_data) {
    if (index >= decoded.size()) {
        return false;  // Index out of range
    }

    if (decoded[index]) {
        return false;  // Already decoded, no need to merge
    }

    // Merge the retransmitted codeword
    decoded[index] = true;
    data[index] = cw_data;
    return true;
}

void CodewordStatus::initForFrame(uint8_t total_cw) {
    // Must clear first! resize() preserves existing elements when shrinking,
    // which would leave stale 'true' values from previous frame
    fixed_frame = false;
    decoded.clear();
    decoded.resize(total_cw, false);
    data.clear();
    data.resize(total_cw);
    iterations.assign(total_cw, 0);
    unsatisfied_checks.assign(total_cw, -1);
    llr_abs_mean.assign(total_cw, 0.0f);
    llr_abs_min.assign(total_cw, 0.0f);
    llr_abs_p10.assign(total_cw, 0.0f);
    llr_abs_p50.assign(total_cw, 0.0f);
    llr_abs_p90.assign(total_cw, 0.0f);
    used_perturbation.assign(total_cw, 0);
    harq_attempts.assign(total_cw, 0);
}

// ============================================================================
// LDPC Integration Implementation
// ============================================================================

std::vector<Bytes> encodeFrameWithLDPC(const Bytes& frame_data) {
    // Default to R1/4 for control frames
    return encodeFrameWithLDPC(frame_data, CodeRate::R1_4);
}

std::vector<Bytes> encodeFrameWithLDPC(const Bytes& frame_data, CodeRate rate) {
    std::vector<Bytes> chunks = splitIntoCodewords(frame_data, rate);

    // Create LDPC encoder with specified rate
    LDPCEncoder encoder(rate);

    std::vector<Bytes> encoded_codewords;
    encoded_codewords.reserve(chunks.size());

    for (const auto& chunk : chunks) {
        auto encoded = encoder.encode(chunk);
        encoded_codewords.push_back(std::move(encoded));
    }

    return encoded_codewords;
}

std::vector<Bytes> encodeInfoCodewordsWithLDPC(const std::vector<Bytes>& info_codewords,
                                               CodeRate rate) {
    const size_t bytes_per_cw = getBytesPerCodeword(rate);
    LDPCEncoder encoder(rate);

    std::vector<Bytes> encoded_codewords;
    encoded_codewords.reserve(info_codewords.size());
    for (auto cw : info_codewords) {
        cw.resize(bytes_per_cw, 0);
        auto encoded = encoder.encode(cw);
        encoded_codewords.push_back(std::move(encoded));
    }
    return encoded_codewords;
}

std::pair<bool, Bytes> decodeSingleCodeword(const std::vector<float>& soft_bits) {
    // Default to R1/4 for control frames
    return decodeSingleCodeword(soft_bits, CodeRate::R1_4);
}

std::pair<bool, Bytes> decodeSingleCodeword(const std::vector<float>& soft_bits, CodeRate rate) {
    if (soft_bits.size() < LDPC_CODEWORD_BITS) {
        return {false, {}};
    }

    // Get bytes per codeword for this rate
    size_t bytes_per_cw = getBytesPerCodeword(rate);

    // Create LDPC decoder with specified rate and recommended iterations
    LDPCDecoder decoder(rate);
    decoder.setMaxIterations(fec::LDPCCodec::getRecommendedIterations(rate));

    // Decode - returns k/8 bytes
    auto decoded = decoder.decodeSoft(soft_bits);
    bool success = decoder.lastDecodeSuccess();

    if (!success || decoded.size() < bytes_per_cw) {
        return {false, {}};
    }

    // Return exactly bytes_per_cw bytes (the info portion)
    Bytes result(decoded.begin(), decoded.begin() + bytes_per_cw);
    return {true, result};
}

CodewordStatus decodeCodewordsWithLDPC(const std::vector<std::vector<float>>& soft_bits) {
    CodewordStatus status;
    status.decoded.resize(soft_bits.size(), false);
    status.data.resize(soft_bits.size());
    status.iterations.assign(soft_bits.size(), 0);
    status.unsatisfied_checks.assign(soft_bits.size(), -1);
    status.llr_abs_mean.assign(soft_bits.size(), 0.0f);
    status.llr_abs_min.assign(soft_bits.size(), 0.0f);
    status.llr_abs_p10.assign(soft_bits.size(), 0.0f);
    status.llr_abs_p50.assign(soft_bits.size(), 0.0f);
    status.llr_abs_p90.assign(soft_bits.size(), 0.0f);
    status.used_perturbation.assign(soft_bits.size(), 0);
    status.harq_attempts.assign(soft_bits.size(), 1);

    for (size_t i = 0; i < soft_bits.size(); i++) {
        const auto llr_summary = summarizeAbsLlrs(soft_bits[i]);
        auto [success, data] = decodeSingleCodeword(soft_bits[i]);
        status.decoded[i] = success;
        status.llr_abs_mean[i] = llr_summary.mean;
        status.llr_abs_min[i] = llr_summary.min;
        status.llr_abs_p10[i] = llr_summary.p10;
        status.llr_abs_p50[i] = llr_summary.p50;
        status.llr_abs_p90[i] = llr_summary.p90;
        if (success) {
            status.data[i] = std::move(data);
        }
    }

    return status;
}

HeaderInfo parseHeader(const Bytes& first_codeword_data) {
    HeaderInfo info;

    if (first_codeword_data.size() < BYTES_PER_CODEWORD) {
        return info;  // invalid
    }

    // Check magic
    uint16_t magic = (static_cast<uint16_t>(first_codeword_data[0]) << 8) |
                      first_codeword_data[1];
    if (magic != MAGIC_V2) {
        return info;  // invalid magic
    }

    // Parse type
    info.type = static_cast<FrameType>(first_codeword_data[2]);
    info.is_control = isControlFrame(info.type);

    // Parse sequence
    info.seq = (static_cast<uint16_t>(first_codeword_data[4]) << 8) |
                first_codeword_data[5];

    // Parse hashes
    info.src_hash = (static_cast<uint32_t>(first_codeword_data[6]) << 16) |
                    (static_cast<uint32_t>(first_codeword_data[7]) << 8) |
                    first_codeword_data[8];
    info.dst_hash = (static_cast<uint32_t>(first_codeword_data[9]) << 16) |
                    (static_cast<uint32_t>(first_codeword_data[10]) << 8) |
                    first_codeword_data[11];

    if (info.is_control) {
        // Control frame: always 1 codeword, verify CRC
        uint16_t received_crc = (static_cast<uint16_t>(first_codeword_data[18]) << 8) |
                                 first_codeword_data[19];
        uint16_t calculated_crc = ControlFrame::calculateCRC(first_codeword_data.data(), 18);
        if (received_crc != calculated_crc) {
            return info;  // CRC failed
        }
        info.total_cw = 1;
        info.payload_len = 0;
    } else if (info.type == FrameType::DATA_REPAIR) {
        auto repair = DataRepairFrame::parseHeader(first_codeword_data);
        if (!repair) {
            return info;
        }
        info.total_cw = static_cast<uint8_t>(repair->repair_count + 1);
        info.payload_len = 0;
    } else {
        // Data frame: read TOTAL_CW and LEN, verify header CRC
        info.total_cw = first_codeword_data[12];
        info.payload_len = (static_cast<uint16_t>(first_codeword_data[13]) << 8) |
                            first_codeword_data[14];

        uint16_t received_hcrc = (static_cast<uint16_t>(first_codeword_data[15]) << 8) |
                                  first_codeword_data[16];
        uint16_t calculated_hcrc = ControlFrame::calculateCRC(first_codeword_data.data(), 15);
        if (received_hcrc != calculated_hcrc) {
            LOG_MODEM(WARN, "parseHeader: data frame header CRC mismatch: rx=%04X calc=%04X (total_cw=%d, payload_len=%d)",
                      received_hcrc, calculated_hcrc, info.total_cw, info.payload_len);
            return info;  // Header CRC failed
        }
    }

    info.valid = true;
    return info;
}

CodewordInfo identifyCodeword(const Bytes& cw_data) {
    CodewordInfo info;

    if (cw_data.size() < 2) {
        return info;  // Too short to identify
    }

    // Check for header magic (0x554C = "UL")
    uint16_t first_two = (static_cast<uint16_t>(cw_data[0]) << 8) | cw_data[1];
    if (first_two == MAGIC_V2) {
        info.type = CodewordType::HEADER;
        info.index = 0;
        return info;
    }

    // Check for data codeword marker (0xD5)
    if (cw_data[0] == DATA_CW_MARKER) {
        info.type = CodewordType::DATA;
        info.index = cw_data[1];
        return info;
    }

    // Unknown codeword type
    return info;
}

// ============================================================================
// Fixed-Codeword Frame Implementation
// ============================================================================

namespace {
// Info-block counts of the 24-column 802.11n base matrix (k = info_blocks * Z).
// Matches LDPCEncoder/Decoder getCodeParams. Used to size the file-class long
// codeword (Z=81) without touching the rate-only getBytesPerCodeword() used by
// the default (Z=27) path and its many capacity-math callers.
inline int infoBlocksForRate(CodeRate r) {
    switch (r) {
        case CodeRate::R1_4: return 6;
        case CodeRate::R1_2: return 12;
        case CodeRate::R2_3: return 16;
        case CodeRate::R3_4: return 18;
        case CodeRate::R5_6: return 20;
        default:             return 12;
    }
}
// Info bytes per codeword at lifting size Z. Z=27 must equal getBytesPerCodeword().
inline size_t infoBytesPerCodewordZ(CodeRate r, int lifting_z) {
    return static_cast<size_t>(infoBlocksForRate(r)) * static_cast<size_t>(lifting_z) / 8;
}
constexpr int kLdpcBlockCols = 24;  // n = kLdpcBlockCols * Z (648 @ Z27, 1944 @ Z81)
}  // namespace

Bytes encodeFixedFrame(const Bytes& frame_data, CodeRate rate, int cw_count,
                       bool use_channel_interleave, size_t bits_per_symbol,
                       int lifting_z) {
    using namespace fec;

    cw_count = sanitizeFixedFrameCodewords(cw_count);
    const int codeword_bits = kLdpcBlockCols * lifting_z;  // 648 or 1944
    size_t bytes_per_cw = (lifting_z == 27) ? getBytesPerCodeword(rate)
                                            : infoBytesPerCodewordZ(rate, lifting_z);
    size_t total_info_bytes = static_cast<size_t>(cw_count) * bytes_per_cw;

    // Pad frame data to exactly N CWs worth of info bytes
    Bytes padded = frame_data;
    if (padded.size() < total_info_bytes) {
        padded.resize(total_info_bytes, 0);
    } else if (padded.size() > total_info_bytes) {
        padded.resize(total_info_bytes);  // Truncate (caller should have chunked)
    }

    // Split into fixed-size info chunks and LDPC encode each
    LDPCEncoder encoder(rate, lifting_z);
    std::vector<std::vector<uint8_t>> coded_codewords;
    coded_codewords.reserve(static_cast<size_t>(cw_count));

    // Create channel interleaver if enabled
    std::unique_ptr<ChannelInterleaver> interleaver;
    if (use_channel_interleave) {
        interleaver = std::make_unique<ChannelInterleaver>(bits_per_symbol, codeword_bits);
    }

    for (int cw = 0; cw < cw_count; ++cw) {
        // Extract info bytes for this CW
        Bytes info_chunk(padded.begin() + cw * bytes_per_cw,
                         padded.begin() + (cw + 1) * bytes_per_cw);

        // LDPC encode → 81 bytes (648 bits)
        auto coded = encoder.encode(info_chunk);

        // Apply channel interleaving if enabled
        if (use_channel_interleave && interleaver) {
            coded = interleaver->interleave(coded);
        }

        coded_codewords.push_back(std::move(coded));
    }

    // Apply frame-level interleaving
    return FrameInterleaver::interleave(coded_codewords, cw_count, codeword_bits);
}

Bytes encodeFixedFrame(const Bytes& frame_data, CodeRate rate, bool use_channel_interleave, size_t bits_per_symbol) {
    return encodeFixedFrame(frame_data, rate, kDefaultFixedFrameCodewords,
                            use_channel_interleave, bits_per_symbol);
}

Bytes encodeFixedFrame(const Bytes& frame_data, CodeRate rate, int cw_count) {
    return encodeFixedFrame(frame_data, rate, cw_count, false);
}

// Default: no channel interleaving (backward compatible)
Bytes encodeFixedFrame(const Bytes& frame_data, CodeRate rate) {
    return encodeFixedFrame(frame_data, rate, false);
}

CodewordStatus decodeFixedFrame(const std::vector<float>& interleaved_soft, CodeRate rate, int cw_count,
                                bool use_channel_deinterleave, size_t bits_per_symbol,
                                fec::SoftCombineBuffer* harq_buffer,
                                const fec::SoftCombineBuffer::Key* harq_key,
                                int lifting_z) {
    using namespace fec;
    ultra::timing::ScopedTimer _profile_(
        ultra::timing::globalDecoderProfile().decode_fixed_frame_total);

    cw_count = sanitizeFixedFrameCodewords(cw_count);
    const int codeword_bits = kLdpcBlockCols * lifting_z;  // 648 @ Z27, 1944 @ Z81
    const size_t total_frame_bits =
        static_cast<size_t>(FrameInterleaver::totalFrameBits(cw_count, codeword_bits));

    CodewordStatus status;
    status.fixed_frame = true;
    status.decoded.resize(static_cast<size_t>(cw_count), false);
    status.data.resize(static_cast<size_t>(cw_count));
    status.iterations.assign(static_cast<size_t>(cw_count), 0);
    status.unsatisfied_checks.assign(static_cast<size_t>(cw_count), -1);
    status.llr_abs_mean.assign(static_cast<size_t>(cw_count), 0.0f);
    status.llr_abs_min.assign(static_cast<size_t>(cw_count), 0.0f);
    status.llr_abs_p10.assign(static_cast<size_t>(cw_count), 0.0f);
    status.llr_abs_p50.assign(static_cast<size_t>(cw_count), 0.0f);
    status.llr_abs_p90.assign(static_cast<size_t>(cw_count), 0.0f);
    status.used_perturbation.assign(static_cast<size_t>(cw_count), 0);
    status.harq_attempts.assign(static_cast<size_t>(cw_count), 0);

    // Check we have enough soft bits
    if (interleaved_soft.size() < total_frame_bits) {
        return status;  // All failed - not enough data
    }

    const bool harq_has_key =
        harq_buffer && harq_key && harq_key->sender_hash != 0;
    const bool harq_active = harq_has_key && harq_buffer->enabled();

    // Deinterleave to restore original CW order (frame-level)
    auto cw_soft_bits = FrameInterleaver::deinterleave(interleaved_soft, cw_count, codeword_bits);

    // Create channel interleaver for deinterleaving if enabled
    std::unique_ptr<ChannelInterleaver> interleaver;
    if (use_channel_deinterleave) {
        interleaver = std::make_unique<ChannelInterleaver>(bits_per_symbol, codeword_bits);
    }

    // Decode each codeword
    // Use min-sum factor 0.9375 (closer to BP) as default — empirically best
    // for DQPSK differential LLRs on fading channels. The default Z=27 path uses
    // the cached per-rate decoder; the file-class long code (Z=81) uses a local
    // decoder so the cache + every existing caller stay untouched.
    std::unique_ptr<LDPCDecoder> long_decoder;
    LDPCDecoder* decoder_ptr;
    if (lifting_z == 27) {
        decoder_ptr = &fixedFrameDecoderForRate(rate);
    } else {
        long_decoder = std::make_unique<LDPCDecoder>(rate, lifting_z);
        decoder_ptr = long_decoder.get();
    }
    LDPCDecoder& decoder = *decoder_ptr;
    size_t bytes_per_cw = (lifting_z == 27) ? getBytesPerCodeword(rate)
                                            : infoBytesPerCodewordZ(rate, lifting_z);

    int perturbation_cw_count = 0;  // How many CWs needed perturbation retry
    std::vector<std::vector<float>> decoder_soft_bits(static_cast<size_t>(cw_count));
    std::vector<int> harq_attempts(static_cast<size_t>(cw_count), 1);

    auto keyForCodeword = [&](int cw) {
        fec::SoftCombineBuffer::Key cw_key = *harq_key;
        cw_key.cw_index = static_cast<uint8_t>(std::clamp(cw, 0, 255));
        return cw_key;
    };

    auto finalize_harq = [&](const CodewordStatus& final_status) {
        if (!harq_has_key) {
            return;
        }
        for (int cw = 0; cw < cw_count; ++cw) {
            const auto cw_key = keyForCodeword(cw);
            if (cw < static_cast<int>(final_status.decoded.size()) &&
                final_status.decoded[static_cast<size_t>(cw)]) {
                if (harqDebugKeySelected(cw_key)) {
                    LOG_MODEM(WARN,
                              "HARQ_DEBUG finalize action=drop seq=%u cw=%d/%u frame_all_success=%d decoded=1",
                              cw_key.seq, cw, cw_key.cw_count, final_status.allSuccess() ? 1 : 0);
                }
                harq_buffer->drop(cw_key);
            } else if (harq_active &&
                       cw < static_cast<int>(decoder_soft_bits.size()) &&
                       !decoder_soft_bits[static_cast<size_t>(cw)].empty()) {
                if (harqDebugKeySelected(cw_key)) {
                    LOG_MODEM(WARN,
                              "HARQ_DEBUG finalize action=retain seq=%u cw=%d/%u frame_all_success=%d decoded=0 mean_abs=%.3f",
                              cw_key.seq, cw, cw_key.cw_count,
                              final_status.allSuccess() ? 1 : 0,
                              meanAbsLlr(decoder_soft_bits[static_cast<size_t>(cw)]));
                }
                harq_buffer->retain(cw_key, decoder_soft_bits[static_cast<size_t>(cw)]);
            }
        }
    };

    for (int cw = 0; cw < cw_count; ++cw) {
        auto cw_bits = cw_soft_bits[cw];

        // Apply channel deinterleaving if enabled
        if (use_channel_deinterleave && interleaver) {
            cw_bits = interleaver->deinterleave(cw_bits);
        }

        if (harq_active) {
            std::vector<float> combined_cw_bits;
            const auto cw_key = keyForCodeword(cw);
            const int attempts = harq_buffer->combine(cw_key, cw_bits, combined_cw_bits);
            harq_attempts[static_cast<size_t>(cw)] = attempts;
            if (!combined_cw_bits.empty()) {
                cw_bits = std::move(combined_cw_bits);
            }
            if (attempts > 1) {
                LOG_MODEM(INFO,
                          "HARQ: combining attempt %d for seq=%u cw=%d/%u (sender_hash=0x%06X)",
                          attempts, harq_key->seq, cw, harq_key->cw_count,
                          harq_key->sender_hash);
            }
        }
        decoder_soft_bits[static_cast<size_t>(cw)] = cw_bits;

        // The decoder is cached per thread/rate and retry paths mutate this
        // factor. Reset before every CW so one marginal CW cannot bias the
        // next CW in the same fixed frame.
        decoder.setMinSumFactor(kFixedFrameDefaultMinSumFactor);

        // Debug: check LLR statistics for this CW
        float llr_sum = 0.0f;
        for (float llr : cw_bits) {
            llr_sum += llr;
        }
        const float llr_avg = cw_bits.empty() ? 0.0f : llr_sum / cw_bits.size();
        const auto llr_summary = summarizeAbsLlrs(cw_bits);

        std::vector<uint8_t> decoded;
        {
            ultra::timing::ScopedTimer _ldpc_(
                ultra::timing::globalDecoderProfile().ldpc_cw_total);
            decoded = decoder.decodeSoft(cw_bits);
        }
        bool success = decoder.lastDecodeSuccess();
        int iterations = decoder.lastIterations();
        bool used_perturbation = false;  // Track if this CW needed perturbation retry

        // Multi-strategy LDPC retry when decode fails:
        // Uses decoder diversity (varying min-sum factor) + LLR perturbation
        // to break trapping sets from multiple angles.
        if (!success) {
            // Use data-dependent seed for unique perturbation per CW
            uint32_t data_hash = 0;
            for (size_t j = 0; j < std::min(cw_bits.size(), size_t(16)); j++) {
                union { float f; uint32_t u; } conv;
                conv.f = cw_bits[j];
                data_hash ^= conv.u + 0x9e3779b9 + (data_hash << 6) + (data_hash >> 2);
            }

            // Phase 0: Pure decoder diversity (4 attempts)
            // Try different min-sum normalization factors on UNMODIFIED LLRs.
            // Different factors change message-passing dynamics fundamentally,
            // breaking trapping sets that 0.875 gets stuck in.
            // Initial decode uses 0.9375, so try 0.875, 0.75, 0.625, 0.5 here.
            {
                static constexpr float factors[] = {0.875f, 0.75f, 0.625f, 0.5f};
                for (int retry = 0; retry < 4 && !success; retry++) {
                    decoder.setMinSumFactor(factors[retry]);
                    {
                        ultra::timing::ScopedTimer _ldpc_(
                            ultra::timing::globalDecoderProfile().ldpc_cw_total);
                        decoded = decoder.decodeSoft(cw_bits);
                    }
                    if (decoder.lastDecodeSuccess()) {
                        success = true;
                        iterations = decoder.lastIterations();
                        LOG_MODEM(INFO, "CW[%d]: RETRY OK (factor=%.4f, iters=%d)", cw, factors[retry], iterations);
                    }
                }
                decoder.setMinSumFactor(kFixedFrameDefaultMinSumFactor);  // restore default
            }

            // Phase 1: Perturbation with decoder diversity (5 attempts)
            // With ARQ, fast failure + retransmit beats slow recovery.
            // Excessive perturbation (was 44 attempts across 6 phases) caused:
            //   - 200ms per failed frame → decoder falls behind real-time
            //   - High LDPC false positive rate (random noise → wrong codewords)
            //   - 5-10s audio backlog → sync detection degradation
            // Reduced to 5 perturbation attempts (was 34) + 4 factor retries = 9 total.
            if (!success) {
                static constexpr float sigmas1[] = {0.3f, 0.7f, 1.0f, 1.5f, 2.0f};
                static constexpr float factors1[] = {0.75f, 0.625f, 0.875f, 0.75f, 0.625f};
                for (int retry = 0; retry < 5 && !success; retry++) {
                    decoder.setMinSumFactor(factors1[retry]);
                    std::mt19937 rng(data_hash + retry * 997 + retry * 31);
                    std::normal_distribution<float> noise(0.0f, sigmas1[retry]);
                    auto perturbed = cw_bits;
                    for (float& llr : perturbed) {
                        llr += noise(rng);
                    }
                    {
                        ultra::timing::ScopedTimer _ldpc_(
                            ultra::timing::globalDecoderProfile().ldpc_cw_total);
                        decoded = decoder.decodeSoft(perturbed);
                    }
                    if (decoder.lastDecodeSuccess()) {
                        success = true;
                        iterations = decoder.lastIterations();
                        used_perturbation = true;
                        LOG_MODEM(INFO, "CW[%d]: RETRY OK (perturb σ=%.1f f=%.3f, iters=%d)", cw, sigmas1[retry], factors1[retry], iterations);
                    }
                }
                decoder.setMinSumFactor(kFixedFrameDefaultMinSumFactor);
            }
            // Phases 2-6 REMOVED (2026-03-15): excessive perturbation caused false
            // positives and decoder backlog. ARQ handles frame loss more efficiently.
        }

        if (used_perturbation && success) perturbation_cw_count++;

        const int final_iterations = decoder.lastIterations();
        const int final_unsatisfied = decoder.lastUnsatisfiedChecks();

        LOG_MODEM(INFO, "CW[%d]: %s (iters=%d, unsat=%d, llr_avg=%.2f, |llr|=mean %.2f p10 %.2f p50 %.2f p90 %.2f)",
                  cw, success ? "OK" : "FAIL", final_iterations, final_unsatisfied,
                  llr_avg, llr_summary.mean, llr_summary.p10,
                  llr_summary.p50, llr_summary.p90);
        if (harq_active) {
            const auto cw_key = keyForCodeword(cw);
            if (harq_attempts[static_cast<size_t>(cw)] > 1 && harqDebugKeySelected(cw_key)) {
                LOG_MODEM(WARN,
                          "HARQ_DEBUG decode_after_combine seq=%u cw=%d/%u attempts=%d success=%d iters=%d mean_abs=%.3f perturbation=%d",
                          cw_key.seq, cw, cw_key.cw_count,
                          harq_attempts[static_cast<size_t>(cw)], success ? 1 : 0,
                          iterations, meanAbsLlr(cw_bits), used_perturbation ? 1 : 0);
            }
        }

        status.decoded[cw] = success;
        status.iterations[static_cast<size_t>(cw)] = final_iterations;
        status.unsatisfied_checks[static_cast<size_t>(cw)] = final_unsatisfied;
        status.llr_abs_mean[static_cast<size_t>(cw)] = llr_summary.mean;
        status.llr_abs_min[static_cast<size_t>(cw)] = llr_summary.min;
        status.llr_abs_p10[static_cast<size_t>(cw)] = llr_summary.p10;
        status.llr_abs_p50[static_cast<size_t>(cw)] = llr_summary.p50;
        status.llr_abs_p90[static_cast<size_t>(cw)] = llr_summary.p90;
        status.used_perturbation[static_cast<size_t>(cw)] =
            used_perturbation ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
        status.harq_attempts[static_cast<size_t>(cw)] =
            harq_attempts[static_cast<size_t>(cw)];
        if (success && decoded.size() >= bytes_per_cw) {
            // Take exactly bytes_per_cw bytes
            status.data[cw].assign(decoded.begin(), decoded.begin() + bytes_per_cw);
        }
    }

    // ========================================================================
    // LDPC FALSE POSITIVE RECOVERY
    // ========================================================================
    // LDPC min-sum can rarely converge to a wrong-but-valid codeword (syndrome
    // passes but information bits are wrong). Detect via frame CRC and attempt
    // recovery using CRC-guided bit-flip search and LDPC re-decode.
    if (status.allSuccess()) {
        auto frame_data = status.reassemble();
        bool frame_valid = false;
        if (!frame_data.empty()) {
            auto hdr = parseHeader(frame_data);
            if (hdr.valid) {
                if (hdr.is_control) {
                    frame_valid = ControlFrame::deserialize(frame_data).has_value();
                } else {
                    frame_valid = DataFrame::deserialize(frame_data).has_value();
                }
            }
        }

        if (!frame_valid) {
            LOG_MODEM(WARN, "LDPC false positive detected: all CWs decoded but frame invalid (perturbed_cws=%d)",
                      perturbation_cw_count);
            bool recovered = false;

            // If any CW used perturbation retry, the false positive is almost certainly
            // from the random noise injection finding a wrong-but-valid LDPC codeword.
            // Skip expensive bit-flip recovery — it can't fix random garbage and risks
            // producing wrong "recovered" data that passes CRC by coincidence.
            if (perturbation_cw_count > 0) {
                LOG_MODEM(WARN, "LDPC false positive: %d CWs used perturbation, skipping recovery",
                          perturbation_cw_count);
                for (int cw = 0; cw < cw_count; ++cw) {
                    status.decoded[cw] = false;
                }
                finalize_harq(status);
                return status;
            }

            // Helper: verify assembled frame without logging
            auto verifyFrame = [](const Bytes& assembled) -> bool {
                if (assembled.empty()) return false;
                auto h = parseHeader(assembled);
                if (!h.valid) return false;
                if (h.is_control) return ControlFrame::deserialize(assembled).has_value();
                return DataFrame::deserialize(assembled).has_value();
            };

            if (frame_data.empty()) {
                // ===========================================================
                // Case 1: Header CRC error in CW0
                // ===========================================================
                // Use direct magic + header CRC check (avoids parseHeader logging)
                for (size_t byte_idx = 0; byte_idx < bytes_per_cw && !recovered; ++byte_idx) {
                    for (int bit = 0; bit < 8 && !recovered; ++bit) {
                        status.data[0][byte_idx] ^= (1 << bit);
                        // Quick header validation without parseHeader
                        uint16_t magic = (uint16_t(status.data[0][0]) << 8) | status.data[0][1];
                        if (magic == MAGIC_V2) {
                            uint16_t stored_hcrc = (uint16_t(status.data[0][15]) << 8) | status.data[0][16];
                            uint16_t calc_hcrc = ControlFrame::calculateCRC(status.data[0].data(), 15);
                            if (stored_hcrc == calc_hcrc) {
                                auto trial = status.reassemble();
                                if (verifyFrame(trial)) {
                                    LOG_MODEM(INFO, "CW[0]: FALSE POSITIVE RECOVERED (1-bit flip byte %zu bit %d)", byte_idx, bit);
                                    recovered = true;
                                }
                            }
                        }
                        if (!recovered) status.data[0][byte_idx] ^= (1 << bit);
                    }
                }

                // Two-bit flip in CW0 (header + payload start)
                if (!recovered) {
                    size_t total_bits_cw0 = bytes_per_cw * 8;
                    for (size_t b1 = 0; b1 < total_bits_cw0 && !recovered; ++b1) {
                        status.data[0][b1/8] ^= (1 << (b1%8));
                        for (size_t b2 = b1 + 1; b2 < total_bits_cw0 && !recovered; ++b2) {
                            status.data[0][b2/8] ^= (1 << (b2%8));
                            uint16_t magic = (uint16_t(status.data[0][0]) << 8) | status.data[0][1];
                            if (magic == MAGIC_V2) {
                                uint16_t stored_hcrc = (uint16_t(status.data[0][15]) << 8) | status.data[0][16];
                                uint16_t calc_hcrc = ControlFrame::calculateCRC(status.data[0].data(), 15);
                                if (stored_hcrc == calc_hcrc) {
                                    auto trial = status.reassemble();
                                    if (verifyFrame(trial)) {
                                        LOG_MODEM(INFO, "CW[0]: FALSE POSITIVE RECOVERED (2-bit flip CW0 bits %zu,%zu)", b1, b2);
                                        recovered = true;
                                    }
                                }
                            }
                            if (!recovered) status.data[0][b2/8] ^= (1 << (b2%8));
                        }
                        if (!recovered) status.data[0][b1/8] ^= (1 << (b1%8));
                    }
                }
            } else {
                // ===========================================================
                // Case 2: Frame CRC error (header OK, payload corrupted)
                // ===========================================================
                // Work directly on assembled frame_data with CRC delta table
                // for efficient 1-bit and cross-CW 2-bit search.
                auto hdr = parseHeader(frame_data);
                if (hdr.valid && !hdr.is_control) {
                    size_t expected_size = DataFrame::HEADER_SIZE + hdr.payload_len + DataFrame::CRC_SIZE;
                    if (frame_data.size() >= expected_size) {
                        uint16_t stored_fcrc = (uint16_t(frame_data[expected_size-2]) << 8) |
                                                frame_data[expected_size-1];
                        size_t data_bytes = expected_size - 2;
                        uint16_t orig_crc = ControlFrame::calculateCRC(frame_data.data(), data_bytes);
                        uint16_t syndrome = stored_fcrc ^ orig_crc;
                        LOG_MODEM(WARN, "Frame CRC syndrome=%04X (rx=%04X calc=%04X, %zu data bytes)",
                                  syndrome, stored_fcrc, orig_crc, data_bytes);

                        // Precompute CRC delta for each data bit position
                        size_t data_bits = data_bytes * 8;
                        std::vector<uint16_t> deltas(data_bits);
                        for (size_t p = 0; p < data_bits; ++p) {
                            frame_data[p/8] ^= (1 << (p%8));
                            deltas[p] = orig_crc ^ ControlFrame::calculateCRC(frame_data.data(), data_bytes);
                            frame_data[p/8] ^= (1 << (p%8));
                        }

                        // Single-bit search in data bytes
                        for (size_t p = 0; p < data_bits && !recovered; ++p) {
                            if (deltas[p] == syndrome) {
                                size_t frame_byte = p / 8;
                                int bit = p % 8;
                                int cw_idx = static_cast<int>(frame_byte / bytes_per_cw);
                                size_t cw_byte = frame_byte % bytes_per_cw;
                                if (cw_idx < cw_count) {
                                    status.data[cw_idx][cw_byte] ^= (1 << bit);
                                    LOG_MODEM(INFO, "CW[%d]: FALSE POSITIVE RECOVERED (1-bit flip frame byte %zu bit %d)",
                                              cw_idx, frame_byte, bit);
                                    recovered = true;
                                }
                            }
                        }

                        // Single-bit in CRC bytes (syndrome is a power of 2)
                        if (!recovered) {
                            for (int bit = 0; bit < 16 && !recovered; ++bit) {
                                if (syndrome == (1u << bit)) {
                                    // Error in stored CRC itself — data is correct!
                                    size_t frame_byte = (bit >= 8) ? (expected_size - 2) : (expected_size - 1);
                                    int actual_bit = bit % 8;
                                    int cw_idx = static_cast<int>(frame_byte / bytes_per_cw);
                                    size_t cw_byte = frame_byte % bytes_per_cw;
                                    if (cw_idx < cw_count) {
                                        status.data[cw_idx][cw_byte] ^= (1 << actual_bit);
                                        LOG_MODEM(INFO, "CW[%d]: FALSE POSITIVE RECOVERED (CRC bit %d)", cw_idx, bit);
                                        recovered = true;
                                    }
                                }
                            }
                        }

                        // Helper to apply/undo a byte-order bit fix in status.data.
                        // The CRC delta table indexes bits as byte bit positions
                        // (bit 0 = LSB), while LDPC soft bits are MSB-first.
                        auto fixBit = [&](size_t p) {
                            size_t fb = p / 8;
                            int cw = static_cast<int>(fb / bytes_per_cw);
                            size_t cb = fb % bytes_per_cw;
                            if (cw < cw_count)
                                status.data[cw][cb] ^= (1 << (p % 8));
                        };

                        // -------------------------------------------------------
                        // Build suspect set: LDPC-flipped info bits
                        // Multi-bit searches MUST be restricted to suspects to
                        // prevent false CRC matches. With 16-bit CRC, arbitrary
                        // n-bit search has too many candidates:
                        //   C(1280,2)/65536 ≈ 12.5 expected false matches
                        //   C(1280,3)/65536 ≈ 5.3M expected false matches
                        // Restricting to 30 suspects:
                        //   C(30,2)/65536 ≈ 0.007 — safe
                        //   C(30,3)/65536 ≈ 0.062 — acceptable
                        //   C(15,4)/65536 ≈ 0.021 — safe (with LLR threshold)
                        // -------------------------------------------------------
                        struct SuspectBit { size_t crc_bit; float abs_llr; };
                        std::vector<SuspectBit> suspects;

                        for (int c = 0; c < cw_count; ++c) {
                            const auto& soft = decoder_soft_bits[static_cast<size_t>(c)];
                            for (size_t i = 0; i < bytes_per_cw * 8 && i < soft.size(); ++i) {
                                size_t cw_byte = i / 8;
                                int byte_bit = 7 - static_cast<int>(i % 8);
                                size_t frame_byte = c * bytes_per_cw + cw_byte;
                                if (frame_byte >= data_bytes) continue;
                                size_t crc_bit = frame_byte * 8 + static_cast<size_t>(byte_bit);
                                int ch_bit = (soft[i] < 0) ? 1 : 0;
                                int dec_bit = (status.data[c][cw_byte] >> byte_bit) & 1;
                                if (ch_bit != dec_bit) {
                                    suspects.push_back({crc_bit, std::abs(soft[i])});
                                }
                            }
                        }
                        std::sort(suspects.begin(), suspects.end(),
                                  [](const SuspectBit& a, const SuspectBit& b) { return a.abs_llr < b.abs_llr; });

                        constexpr int MAX_S = 30;
                        int ns = std::min(MAX_S, static_cast<int>(suspects.size()));

                        std::vector<uint16_t> sd(ns);
                        for (int i = 0; i < ns; ++i)
                            sd[i] = deltas[suspects[i].crc_bit];

                        // 2-bit among suspects: C(30,2) = 435 — safe
                        if (!recovered) {
                            for (int a = 0; a < ns && !recovered; ++a) {
                                for (int b = a + 1; b < ns && !recovered; ++b) {
                                    if ((sd[a] ^ sd[b]) == syndrome) {
                                        fixBit(suspects[a].crc_bit);
                                        fixBit(suspects[b].crc_bit);
                                        auto trial = status.reassemble();
                                        if (verifyFrame(trial)) {
                                            LOG_MODEM(INFO, "FALSE POSITIVE RECOVERED (2-bit suspects, bits %zu,%zu, |LLR|=%.2f,%.2f)",
                                                      suspects[a].crc_bit, suspects[b].crc_bit,
                                                      suspects[a].abs_llr, suspects[b].abs_llr);
                                            recovered = true;
                                        } else {
                                            fixBit(suspects[a].crc_bit);
                                            fixBit(suspects[b].crc_bit);
                                        }
                                    }
                                }
                            }
                        }

                        // 3-bit and 4-bit suspect searches REMOVED (2026-03-15).
                        // With 16-bit CRC, higher-order searches have unacceptable
                        // false match rates that produce corrupted "recovered" frames:
                        //   C(30,3)/65536 ≈ 6.2%  — caused file transfer corruption
                        //   C(15,4)/65536 ≈ 2.1%  — also unsafe
                        // Only 1-bit (exact) and 2-bit (C(30,2)/65536 ≈ 0.7%) are safe.
                    }
                }
            }

            // ===========================================================
            // Fallback: LDPC re-decode with different min-sum factors
            // ===========================================================
            if (!recovered) {
                static constexpr float recovery_factors[] = {0.75f, 0.625f, 0.5f, 0.875f};
                for (int attempt = 0; attempt < 4 && !recovered; ++attempt) {
                    for (int cw = 0; cw < cw_count && !recovered; ++cw) {
                        const auto& cw_bits = decoder_soft_bits[static_cast<size_t>(cw)];
                        auto original_data = status.data[cw];

                        decoder.setMinSumFactor(recovery_factors[attempt]);
                        std::vector<uint8_t> re_decoded;
                        {
                            ultra::timing::ScopedTimer _ldpc_(
                                ultra::timing::globalDecoderProfile().ldpc_cw_total);
                            re_decoded = decoder.decodeSoft(cw_bits);
                        }
                        if (decoder.lastDecodeSuccess() && re_decoded.size() >= bytes_per_cw) {
                            Bytes new_cw_data(re_decoded.begin(), re_decoded.begin() + bytes_per_cw);
                            if (new_cw_data != original_data) {
                                status.data[cw] = new_cw_data;
                                auto trial = status.reassemble();
                                if (verifyFrame(trial)) {
                                    LOG_MODEM(INFO, "CW[%d]: FALSE POSITIVE RECOVERED (re-decode factor=%.3f)",
                                              cw, recovery_factors[attempt]);
                                    recovered = true;
                                } else {
                                    status.data[cw] = original_data;
                                }
                            }
                        }
                    }
                }
                decoder.setMinSumFactor(kFixedFrameDefaultMinSumFactor);
            }

            if (!recovered) {
                LOG_MODEM(WARN, "LDPC false positive: recovery FAILED, marking as decode failure");
                for (int cw = 0; cw < cw_count; ++cw) {
                    status.decoded[cw] = false;
                }
            }
        }
    }

    finalize_harq(status);
    return status;
}

CodewordStatus decodeFixedFrame(const std::vector<float>& interleaved_soft, CodeRate rate,
                                bool use_channel_deinterleave, size_t bits_per_symbol) {
    return decodeFixedFrame(interleaved_soft, rate, kDefaultFixedFrameCodewords,
                            use_channel_deinterleave, bits_per_symbol);
}

CodewordStatus decodeFixedFrame(const std::vector<float>& interleaved_soft, CodeRate rate, int cw_count) {
    return decodeFixedFrame(interleaved_soft, rate, cw_count, false);
}

// Default: no channel deinterleaving (backward compatible)
CodewordStatus decodeFixedFrame(const std::vector<float>& interleaved_soft, CodeRate rate) {
    return decodeFixedFrame(interleaved_soft, rate, false);
}

DataFrame makeFixedDataFrame(const std::string& src, const std::string& dst,
                              uint16_t seq, const Bytes& payload, CodeRate rate,
                              int cw_count) {
    cw_count = sanitizeFixedFrameCodewords(cw_count);
    size_t capacity = getFixedFramePayloadCapacity(rate, cw_count);

    // Truncate or use as-is
    Bytes actual_payload = payload;
    if (actual_payload.size() > capacity) {
        actual_payload.resize(capacity);
    }

    // Create frame with explicit fixed-frame total_cw
    DataFrame frame;
    frame.type = FrameType::DATA;
    frame.flags = Flags::VERSION_V2;
    frame.seq = seq;
    frame.src_hash = hashCallsign(src);
    frame.dst_hash = hashCallsign(dst);
    frame.payload = actual_payload;
    frame.payload_len = static_cast<uint16_t>(actual_payload.size());
    frame.total_cw = static_cast<uint8_t>(cw_count);

    return frame;
}

} // namespace v2
} // namespace protocol
} // namespace ultra
