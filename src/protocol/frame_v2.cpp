#include "frame_v2.hpp"
#include "ultra/fec.hpp"  // LDPC encoder/decoder + ChannelInterleaver
#include "../fec/frame_interleaver.hpp"  // Frame-level interleaving
#include "../fec/ldpc_codec.hpp"  // For getRecommendedIterations
#include "ultra/logging.hpp"  // LOG_MODEM
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <random>

namespace ultra {
namespace protocol {

// ============================================================================
// Shared Protocol Types Implementation
// ============================================================================

const char* waveformModeToString(WaveformMode mode) {
    switch (mode) {
        case WaveformMode::OFDM_COX:  return "OFDM-COX";
        case WaveformMode::OTFS_EQ:    return "OTFS-EQ";
        case WaveformMode::OTFS_RAW:   return "OTFS-RAW";
        case WaveformMode::MFSK:       return "MFSK";
        case WaveformMode::MC_DPSK:    return "MC-DPSK";
        case WaveformMode::OFDM_CHIRP: return "OFDM-CHIRP";
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
    if (snr_db >= 25.0f && delay_spread_ms < 1.0f && doppler_spread_hz < 1.0f) {
        return "Excellent";
    } else if (snr_db >= 18.0f && delay_spread_ms < 2.0f && doppler_spread_hz < 2.0f) {
        return "Good";
    } else if (snr_db >= 10.0f) {
        return "Moderate";
    } else if (snr_db >= 3.0f) {
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
        case FrameType::BEACON:      return "BEACON";
        case FrameType::DATA:        return "DATA";
        case FrameType::DATA_START:  return "DATA_START";
        case FrameType::DATA_CONT:   return "DATA_CONT";
        case FrameType::DATA_END:    return "DATA_END";
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

ControlFrame ControlFrame::makeModeChange(const std::string& src, const std::string& dst,
                                           uint16_t seq, Modulation new_mod, CodeRate new_rate,
                                           float snr_db, uint8_t reason) {
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
    f.payload[4] = 0;  // Reserved
    f.payload[5] = 0;  // Reserved
    return f;
}

ControlFrame ControlFrame::makeModeChangeByHash(const std::string& src, uint32_t dst_hash,
                                                 uint16_t seq, Modulation new_mod, CodeRate new_rate,
                                                 float snr_db, uint8_t reason) {
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
    f.payload[4] = 0;  // Reserved
    f.payload[5] = 0;  // Reserved
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
    // Total frame size = header (17) + payload + frame_CRC (2)
    size_t total = HEADER_SIZE + payload_size + CRC_SIZE;

    // Calculate based on bit-level encoding (matches ldpc_encoder.cpp)
    // For R1/4: 162 info bits per codeword
    size_t total_bits = total * 8;
    constexpr size_t info_bits = 162;  // R1/4

    return static_cast<uint8_t>((total_bits + info_bits - 1) / info_bits);
}

uint8_t DataFrame::calculateCodewords(size_t payload_size, CodeRate rate) {
    // Total frame size = header (17) + payload + frame_CRC (2)
    size_t total = HEADER_SIZE + payload_size + CRC_SIZE;

    // Calculate based on bit-level encoding (matches ldpc_encoder.cpp)
    // Encoder packs total*8 bits into codewords of info_bits each
    size_t total_bits = total * 8;
    size_t info_bits = getInfoBitsForRate(rate);

    return static_cast<uint8_t>((total_bits + info_bits - 1) / info_bits);
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
                                        uint8_t forced_modulation, uint8_t forced_code_rate) {
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
    return f;
}

ConnectFrame ConnectFrame::makeConnectAck(const std::string& src, const std::string& dst,
                                           uint8_t neg_mode, Modulation init_mod, CodeRate init_rate,
                                           float snr_db) {
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

    f.mode_capabilities = 0;  // Not used in ACK
    f.negotiated_mode = neg_mode;

    // Initial data mode - eliminates separate MODE_CHANGE after connect
    f.initial_modulation = static_cast<uint8_t>(init_mod);
    f.initial_code_rate = static_cast<uint8_t>(init_rate);
    f.measured_snr = encodeSNR(snr_db);
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
    return f;
}

ConnectFrame ConnectFrame::makeDisconnect(const std::string& src, const std::string& dst) {
    ConnectFrame f;
    f.type = FrameType::DISCONNECT;
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
    return f;
}

ConnectFrame ConnectFrame::makeConnectAckByHash(const std::string& src, uint32_t dst_hash,
                                                 uint8_t neg_mode, Modulation init_mod, CodeRate init_rate,
                                                 float snr_db) {
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

    f.mode_capabilities = 0;
    f.negotiated_mode = neg_mode;

    // Initial data mode - eliminates separate MODE_CHANGE after connect
    f.initial_modulation = static_cast<uint8_t>(init_mod);
    f.initial_code_rate = static_cast<uint8_t>(init_rate);
    f.measured_snr = encodeSNR(snr_db);
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

    // Total codewords (1 byte) - Always 4 for frame interleaving
    out.push_back(FIXED_FRAME_CODEWORDS);

    // Payload length (2 bytes)
    out.push_back((PAYLOAD_SIZE >> 8) & 0xFF);
    out.push_back(PAYLOAD_SIZE & 0xFF);

    // Header CRC (2 bytes)
    uint16_t hcrc = ControlFrame::calculateCRC(out.data(), out.size());
    out.push_back((hcrc >> 8) & 0xFF);
    out.push_back(hcrc & 0xFF);

    // Payload: src_callsign (10B) + dst_callsign (10B) + caps (1B) + wfmode (1B) + mod (1B) + rate (1B) + snr (1B)
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

    return f;
}

std::string ConnectFrame::getSrcCallsign() const {
    return std::string(src_callsign);
}

std::string ConnectFrame::getDstCallsign() const {
    return std::string(dst_callsign);
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
// Codeword helpers
// ============================================================================

std::vector<Bytes> splitIntoCodewords(const Bytes& frame_data) {
    std::vector<Bytes> codewords;

    // CW0: First 20 bytes of frame data (contains header with 0x554C magic)
    // No modification needed - the magic already identifies it
    {
        Bytes cw0(BYTES_PER_CODEWORD, 0);  // Zero-pad if needed
        size_t cw0_data = std::min(BYTES_PER_CODEWORD, frame_data.size());
        std::memcpy(cw0.data(), frame_data.data(), cw0_data);
        codewords.push_back(std::move(cw0));
    }

    // CW1+: Add marker + index header before payload data
    size_t offset = BYTES_PER_CODEWORD;  // Start after CW0's data
    uint8_t cw_index = 1;

    while (offset < frame_data.size()) {
        Bytes cw(BYTES_PER_CODEWORD, 0);  // Zero-pad if needed

        // Add marker and index
        cw[0] = DATA_CW_MARKER;
        cw[1] = cw_index;

        // Copy payload data (up to 18 bytes)
        size_t remaining = frame_data.size() - offset;
        size_t chunk_size = std::min(DATA_CW_PAYLOAD_SIZE, remaining);
        std::memcpy(cw.data() + DATA_CW_HEADER_SIZE, frame_data.data() + offset, chunk_size);

        codewords.push_back(std::move(cw));
        offset += DATA_CW_PAYLOAD_SIZE;  // Each CW1+ consumes 18 bytes of frame data
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

    // Reassemble from decoded codewords
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
    decoded.clear();
    decoded.resize(total_cw, false);
    data.clear();
    data.resize(total_cw);
}

// ============================================================================
// LDPC Integration Implementation
// ============================================================================

std::vector<Bytes> encodeFrameWithLDPC(const Bytes& frame_data) {
    // Default to R1/4 for control frames
    return encodeFrameWithLDPC(frame_data, CodeRate::R1_4);
}

std::vector<Bytes> encodeFrameWithLDPC(const Bytes& frame_data, CodeRate rate) {
    // Get bytes per codeword for this rate
    size_t bytes_per_cw = getBytesPerCodeword(rate);
    size_t data_payload_size = bytes_per_cw - DATA_CW_HEADER_SIZE;  // Payload bytes in CW1+

    // Split frame into chunks based on rate
    std::vector<Bytes> chunks;

    // CW0: First bytes_per_cw bytes of frame data (contains header with 0x554C magic)
    {
        Bytes cw0(bytes_per_cw, 0);  // Zero-pad if needed
        size_t cw0_data = std::min(bytes_per_cw, frame_data.size());
        std::memcpy(cw0.data(), frame_data.data(), cw0_data);
        chunks.push_back(std::move(cw0));
    }

    // CW1+: Add marker + index header before payload data
    size_t offset = bytes_per_cw;  // Start after CW0's data
    uint8_t cw_index = 1;

    while (offset < frame_data.size()) {
        Bytes cw(bytes_per_cw, 0);  // Zero-pad if needed

        // Add marker and index
        cw[0] = DATA_CW_MARKER;
        cw[1] = cw_index;

        // Copy payload data
        size_t remaining = frame_data.size() - offset;
        size_t chunk_size = std::min(data_payload_size, remaining);
        std::memcpy(cw.data() + DATA_CW_HEADER_SIZE, frame_data.data() + offset, chunk_size);

        chunks.push_back(std::move(cw));
        offset += data_payload_size;
        cw_index++;
    }

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

    for (size_t i = 0; i < soft_bits.size(); i++) {
        auto [success, data] = decodeSingleCodeword(soft_bits[i]);
        status.decoded[i] = success;
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
// Fixed 4-Codeword Frame Implementation
// ============================================================================

Bytes encodeFixedFrame(const Bytes& frame_data, CodeRate rate, bool use_channel_interleave) {
    using namespace fec;

    // Channel interleaving spreads consecutive bits across OFDM symbols for fading resistance.
    // Uses 106 bits/symbol (53 data carriers * 2 bits for DQPSK) as the standard config.
    static constexpr size_t BITS_PER_SYMBOL = 106;

    size_t bytes_per_cw = getBytesPerCodeword(rate);
    size_t total_info_bytes = FIXED_FRAME_CODEWORDS * bytes_per_cw;

    // Pad frame data to exactly 4 CWs worth of info bytes
    Bytes padded = frame_data;
    if (padded.size() < total_info_bytes) {
        padded.resize(total_info_bytes, 0);
    } else if (padded.size() > total_info_bytes) {
        padded.resize(total_info_bytes);  // Truncate (caller should have chunked)
    }

    // Split into 4 info chunks and LDPC encode each
    LDPCEncoder encoder(rate);
    std::vector<std::vector<uint8_t>> coded_codewords;
    coded_codewords.reserve(FIXED_FRAME_CODEWORDS);

    // Create channel interleaver if enabled
    std::unique_ptr<ChannelInterleaver> interleaver;
    if (use_channel_interleave) {
        interleaver = std::make_unique<ChannelInterleaver>(BITS_PER_SYMBOL, LDPC_CODEWORD_BITS);
    }

    for (int cw = 0; cw < FIXED_FRAME_CODEWORDS; ++cw) {
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
    return FrameInterleaver::interleave(coded_codewords);
}

// Default: no channel interleaving (backward compatible)
Bytes encodeFixedFrame(const Bytes& frame_data, CodeRate rate) {
    return encodeFixedFrame(frame_data, rate, false);
}

CodewordStatus decodeFixedFrame(const std::vector<float>& interleaved_soft, CodeRate rate, bool use_channel_deinterleave) {
    using namespace fec;

    // Channel deinterleaving must match TX - uses same 106 bits/symbol
    static constexpr size_t BITS_PER_SYMBOL = 106;

    CodewordStatus status;
    status.decoded.resize(FIXED_FRAME_CODEWORDS, false);
    status.data.resize(FIXED_FRAME_CODEWORDS);

    // Check we have enough soft bits
    if (interleaved_soft.size() < FrameInterleaver::TOTAL_FRAME_BITS) {
        return status;  // All failed - not enough data
    }

    // Deinterleave to restore original CW order (frame-level)
    auto cw_soft_bits = FrameInterleaver::deinterleave(interleaved_soft);

    // Create channel interleaver for deinterleaving if enabled
    std::unique_ptr<ChannelInterleaver> interleaver;
    if (use_channel_deinterleave) {
        interleaver = std::make_unique<ChannelInterleaver>(BITS_PER_SYMBOL, LDPC_CODEWORD_BITS);
    }

    // Decode each codeword
    // Use min-sum factor 0.9375 (closer to BP) as default — empirically best
    // for DQPSK differential LLRs on fading channels.
    LDPCDecoder decoder(rate);
    decoder.setMaxIterations(fec::LDPCCodec::getRecommendedIterations(rate));
    decoder.setMinSumFactor(0.9375f);
    size_t bytes_per_cw = getBytesPerCodeword(rate);

    for (int cw = 0; cw < FIXED_FRAME_CODEWORDS; ++cw) {
        auto cw_bits = cw_soft_bits[cw];

        // Apply channel deinterleaving if enabled
        if (use_channel_deinterleave && interleaver) {
            cw_bits = interleaver->deinterleave(cw_bits);
        }

        // Debug: check LLR statistics for this CW
        float llr_sum = 0, llr_abs_sum = 0;
        for (float llr : cw_bits) {
            llr_sum += llr;
            llr_abs_sum += std::abs(llr);
        }
        float llr_avg = llr_sum / cw_bits.size();
        float llr_abs_avg = llr_abs_sum / cw_bits.size();

        auto decoded = decoder.decodeSoft(cw_bits);
        bool success = decoder.lastDecodeSuccess();
        int iterations = decoder.lastIterations();

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
                    decoded = decoder.decodeSoft(cw_bits);
                    if (decoder.lastDecodeSuccess()) {
                        success = true;
                        iterations = decoder.lastIterations();
                        LOG_MODEM(INFO, "CW[%d]: RETRY OK (factor=%.4f, iters=%d)", cw, factors[retry], iterations);
                    }
                }
                decoder.setMinSumFactor(0.9375f);  // restore default
            }

            // Phase 1: Perturbation with decoder diversity (15 attempts)
            // Alternate min-sum factor between attempts for maximum diversity.
            // Different seeds × different factors explore the solution space broadly.
            if (!success) {
                static constexpr float sigmas1[] = {
                    0.3f, 0.7f, 0.3f, 1.0f, 0.5f, 1.5f, 0.3f, 2.0f,
                    0.5f, 0.7f, 1.0f, 2.5f, 0.3f, 1.5f, 0.5f
                };
                static constexpr float factors1[] = {
                    0.75f, 0.625f, 0.875f, 0.75f, 0.625f, 0.75f, 0.5f, 0.625f,
                    0.875f, 0.75f, 0.625f, 0.875f, 0.75f, 0.5f, 0.625f
                };
                for (int retry = 0; retry < 15 && !success; retry++) {
                    decoder.setMinSumFactor(factors1[retry]);
                    std::mt19937 rng(data_hash + retry * 997 + retry * 31);
                    std::normal_distribution<float> noise(0.0f, sigmas1[retry]);
                    auto perturbed = cw_bits;
                    for (float& llr : perturbed) {
                        llr += noise(rng);
                    }
                    decoded = decoder.decodeSoft(perturbed);
                    if (decoder.lastDecodeSuccess()) {
                        success = true;
                        iterations = decoder.lastIterations();
                        LOG_MODEM(INFO, "CW[%d]: RETRY OK (perturb σ=%.1f f=%.3f, iters=%d)", cw, sigmas1[retry], factors1[retry], iterations);
                    }
                }
                decoder.setMinSumFactor(0.875f);
            }

            // Phase 2: Clip ±10 + perturbation (5 attempts)
            if (!success) {
                static constexpr float sigmas2[] = {0.3f, 0.8f, 1.5f, 2.5f, 4.0f};
                for (int retry = 0; retry < 5 && !success; retry++) {
                    decoder.setMinSumFactor(retry % 2 == 0 ? 0.625f : 0.875f);
                    std::mt19937 rng(data_hash + (retry + 15) * 997 + 12345);
                    std::normal_distribution<float> noise(0.0f, sigmas2[retry]);
                    auto clipped = cw_bits;
                    for (float& llr : clipped) {
                        llr = std::max(-10.0f, std::min(10.0f, llr));
                        llr += noise(rng);
                    }
                    decoded = decoder.decodeSoft(clipped);
                    if (decoder.lastDecodeSuccess()) {
                        success = true;
                        iterations = decoder.lastIterations();
                        LOG_MODEM(INFO, "CW[%d]: RETRY OK (clip10+perturb σ=%.1f, iters=%d)", cw, sigmas2[retry], iterations);
                    }
                }
                decoder.setMinSumFactor(0.875f);
            }

            // Phase 3: Scale 0.5× + perturbation (3 attempts)
            if (!success) {
                static constexpr float sigmas3[] = {0.5f, 1.5f, 3.0f};
                for (int retry = 0; retry < 3 && !success; retry++) {
                    std::mt19937 rng(data_hash + (retry + 20) * 997 + 54321);
                    std::normal_distribution<float> noise(0.0f, sigmas3[retry]);
                    auto scaled = cw_bits;
                    for (float& llr : scaled) {
                        llr = llr * 0.5f + noise(rng);
                    }
                    decoded = decoder.decodeSoft(scaled);
                    if (decoder.lastDecodeSuccess()) {
                        success = true;
                        iterations = decoder.lastIterations();
                        LOG_MODEM(INFO, "CW[%d]: RETRY OK (scale50+perturb σ=%.1f, iters=%d)", cw, sigmas3[retry], iterations);
                    }
                }
            }

            // Phase 4: Clip ±6 + perturbation (3 attempts)
            if (!success) {
                static constexpr float sigmas4[] = {0.5f, 1.5f, 3.0f};
                for (int retry = 0; retry < 3 && !success; retry++) {
                    std::mt19937 rng(data_hash + (retry + 23) * 997 + 99999);
                    std::normal_distribution<float> noise(0.0f, sigmas4[retry]);
                    auto clipped = cw_bits;
                    for (float& llr : clipped) {
                        llr = std::max(-6.0f, std::min(6.0f, llr));
                        llr += noise(rng);
                    }
                    decoded = decoder.decodeSoft(clipped);
                    if (decoder.lastDecodeSuccess()) {
                        success = true;
                        iterations = decoder.lastIterations();
                        LOG_MODEM(INFO, "CW[%d]: RETRY OK (clip6+perturb σ=%.1f, iters=%d)", cw, sigmas4[retry], iterations);
                    }
                }
            }

            // Phase 5: Hard decision + perturbation (5 attempts)
            if (!success) {
                static constexpr float sigmas5[] = {0.0f, 0.2f, 0.5f, 1.0f, 1.5f};
                for (int retry = 0; retry < 5 && !success; retry++) {
                    std::mt19937 rng(data_hash + (retry + 26) * 997 + 33333);
                    std::normal_distribution<float> noise(0.0f, sigmas5[retry]);
                    auto hard = cw_bits;
                    for (float& llr : hard) {
                        llr = (llr >= 0) ? 1.0f : -1.0f;
                        llr += noise(rng);
                    }
                    decoded = decoder.decodeSoft(hard);
                    if (decoder.lastDecodeSuccess()) {
                        success = true;
                        iterations = decoder.lastIterations();
                        LOG_MODEM(INFO, "CW[%d]: RETRY OK (hard+perturb σ=%.1f, iters=%d)", cw, sigmas5[retry], iterations);
                    }
                }
            }

            // Phase 6: Scale 0.25× + perturbation (3 attempts)
            if (!success) {
                static constexpr float sigmas6[] = {0.3f, 1.0f, 2.0f};
                for (int retry = 0; retry < 3 && !success; retry++) {
                    std::mt19937 rng(data_hash + (retry + 31) * 997 + 77777);
                    std::normal_distribution<float> noise(0.0f, sigmas6[retry]);
                    auto scaled = cw_bits;
                    for (float& llr : scaled) {
                        llr = llr * 0.25f + noise(rng);
                    }
                    decoded = decoder.decodeSoft(scaled);
                    if (decoder.lastDecodeSuccess()) {
                        success = true;
                        iterations = decoder.lastIterations();
                        LOG_MODEM(INFO, "CW[%d]: RETRY OK (scale25+perturb σ=%.1f, iters=%d)", cw, sigmas6[retry], iterations);
                    }
                }
            }
        }

        LOG_MODEM(INFO, "CW[%d]: %s (iters=%d, llr_avg=%.2f, |llr|_avg=%.2f)",
                  cw, success ? "OK" : "FAIL", iterations, llr_avg, llr_abs_avg);

        status.decoded[cw] = success;
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
            LOG_MODEM(WARN, "LDPC false positive detected: all CWs decoded but frame invalid");
            bool recovered = false;

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
                                if (cw_idx < FIXED_FRAME_CODEWORDS) {
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
                                    if (cw_idx < FIXED_FRAME_CODEWORDS) {
                                        status.data[cw_idx][cw_byte] ^= (1 << actual_bit);
                                        LOG_MODEM(INFO, "CW[%d]: FALSE POSITIVE RECOVERED (CRC bit %d)", cw_idx, bit);
                                        recovered = true;
                                    }
                                }
                            }
                        }

                        // Helper to apply/undo bit fix in status.data
                        auto fixBit = [&](size_t p) {
                            size_t fb = p / 8;
                            int cw = static_cast<int>(fb / bytes_per_cw);
                            size_t cb = fb % bytes_per_cw;
                            if (cw < FIXED_FRAME_CODEWORDS)
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
                        struct SuspectBit { size_t frame_bit; float abs_llr; };
                        std::vector<SuspectBit> suspects;

                        for (int c = 0; c < FIXED_FRAME_CODEWORDS; ++c) {
                            auto soft = cw_soft_bits[c];
                            if (use_channel_deinterleave && interleaver) {
                                soft = interleaver->deinterleave(soft);
                            }
                            for (size_t i = 0; i < bytes_per_cw * 8 && i < soft.size(); ++i) {
                                size_t frame_bit = c * bytes_per_cw * 8 + i;
                                if (frame_bit / 8 >= data_bytes) continue;
                                int ch_bit = (soft[i] < 0) ? 1 : 0;
                                int dec_bit = (status.data[c][i / 8] >> (i % 8)) & 1;
                                if (ch_bit != dec_bit) {
                                    suspects.push_back({frame_bit, std::abs(soft[i])});
                                }
                            }
                        }
                        std::sort(suspects.begin(), suspects.end(),
                                  [](const SuspectBit& a, const SuspectBit& b) { return a.abs_llr < b.abs_llr; });

                        constexpr int MAX_S = 30;
                        int ns = std::min(MAX_S, static_cast<int>(suspects.size()));

                        std::vector<uint16_t> sd(ns);
                        for (int i = 0; i < ns; ++i)
                            sd[i] = deltas[suspects[i].frame_bit];

                        // 2-bit among suspects: C(30,2) = 435 — safe
                        if (!recovered) {
                            for (int a = 0; a < ns && !recovered; ++a) {
                                for (int b = a + 1; b < ns && !recovered; ++b) {
                                    if ((sd[a] ^ sd[b]) == syndrome) {
                                        fixBit(suspects[a].frame_bit);
                                        fixBit(suspects[b].frame_bit);
                                        auto trial = status.reassemble();
                                        if (verifyFrame(trial)) {
                                            LOG_MODEM(INFO, "FALSE POSITIVE RECOVERED (2-bit suspects, bits %zu,%zu, |LLR|=%.2f,%.2f)",
                                                      suspects[a].frame_bit, suspects[b].frame_bit,
                                                      suspects[a].abs_llr, suspects[b].abs_llr);
                                            recovered = true;
                                        } else {
                                            fixBit(suspects[a].frame_bit);
                                            fixBit(suspects[b].frame_bit);
                                        }
                                    }
                                }
                            }
                        }

                        // 3-bit among suspects: C(30,3) = 4060 — acceptable
                        if (!recovered) {
                            for (int a = 0; a < ns && !recovered; ++a) {
                                uint16_t da = sd[a];
                                for (int b = a + 1; b < ns && !recovered; ++b) {
                                    uint16_t dab = da ^ sd[b];
                                    for (int c2 = b + 1; c2 < ns && !recovered; ++c2) {
                                        if ((dab ^ sd[c2]) == syndrome) {
                                            fixBit(suspects[a].frame_bit);
                                            fixBit(suspects[b].frame_bit);
                                            fixBit(suspects[c2].frame_bit);
                                            auto trial = status.reassemble();
                                            if (verifyFrame(trial)) {
                                                LOG_MODEM(INFO, "FALSE POSITIVE RECOVERED (3-bit suspects, bits %zu,%zu,%zu)",
                                                          suspects[a].frame_bit, suspects[b].frame_bit, suspects[c2].frame_bit);
                                                recovered = true;
                                            } else {
                                                fixBit(suspects[a].frame_bit);
                                                fixBit(suspects[b].frame_bit);
                                                fixBit(suspects[c2].frame_bit);
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // 4-bit among weak suspects: limit |LLR| < 3.0 to reduce false matches
                        // With ~15 weak suspects: C(15,4) ≈ 1365 — safe (0.021 expected false CRC matches)
                        // NOTE: 5-bit (C(30,5)/65536≈2.17), 6-bit (≈9.1), and hybrid (≈8.5)
                        // searches were REMOVED — too many false CRC matches with 16-bit CRC.
                        if (!recovered) {
                            constexpr float MAX_SUSPECT_LLR = 3.0f;
                            for (int a = 0; a < ns && !recovered; ++a) {
                                if (suspects[a].abs_llr > MAX_SUSPECT_LLR) break;
                                uint16_t da = sd[a];
                                for (int b = a + 1; b < ns && !recovered; ++b) {
                                    if (suspects[b].abs_llr > MAX_SUSPECT_LLR) break;
                                    uint16_t dab = da ^ sd[b];
                                    for (int c2 = b + 1; c2 < ns && !recovered; ++c2) {
                                        if (suspects[c2].abs_llr > MAX_SUSPECT_LLR) break;
                                        uint16_t dabc = dab ^ sd[c2];
                                        for (int d = c2 + 1; d < ns && !recovered; ++d) {
                                            if (suspects[d].abs_llr > MAX_SUSPECT_LLR) break;
                                            if ((dabc ^ sd[d]) == syndrome) {
                                                fixBit(suspects[a].frame_bit);
                                                fixBit(suspects[b].frame_bit);
                                                fixBit(suspects[c2].frame_bit);
                                                fixBit(suspects[d].frame_bit);
                                                auto trial = status.reassemble();
                                                if (verifyFrame(trial)) {
                                                    LOG_MODEM(INFO, "FALSE POSITIVE RECOVERED (4-bit suspects, |LLR|=%.2f,%.2f,%.2f,%.2f)",
                                                              suspects[a].abs_llr, suspects[b].abs_llr, suspects[c2].abs_llr, suspects[d].abs_llr);
                                                    recovered = true;
                                                } else {
                                                    fixBit(suspects[a].frame_bit);
                                                    fixBit(suspects[b].frame_bit);
                                                    fixBit(suspects[c2].frame_bit);
                                                    fixBit(suspects[d].frame_bit);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ===========================================================
            // Fallback: LDPC re-decode with different min-sum factors
            // ===========================================================
            if (!recovered) {
                static constexpr float recovery_factors[] = {0.75f, 0.625f, 0.5f, 0.875f};
                for (int attempt = 0; attempt < 4 && !recovered; ++attempt) {
                    for (int cw = 0; cw < FIXED_FRAME_CODEWORDS && !recovered; ++cw) {
                        auto cw_bits = cw_soft_bits[cw];
                        if (use_channel_deinterleave && interleaver) {
                            cw_bits = interleaver->deinterleave(cw_bits);
                        }
                        auto original_data = status.data[cw];

                        decoder.setMinSumFactor(recovery_factors[attempt]);
                        auto re_decoded = decoder.decodeSoft(cw_bits);
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
                decoder.setMinSumFactor(0.9375f);
            }

            if (!recovered) {
                LOG_MODEM(WARN, "LDPC false positive: recovery FAILED, marking as decode failure");
                for (int cw = 0; cw < FIXED_FRAME_CODEWORDS; ++cw) {
                    status.decoded[cw] = false;
                }
            }
        }
    }

    return status;
}

// Default: no channel deinterleaving (backward compatible)
CodewordStatus decodeFixedFrame(const std::vector<float>& interleaved_soft, CodeRate rate) {
    return decodeFixedFrame(interleaved_soft, rate, false);
}

DataFrame makeFixedDataFrame(const std::string& src, const std::string& dst,
                              uint16_t seq, const Bytes& payload, CodeRate rate) {
    size_t capacity = getFixedFramePayloadCapacity(rate);

    // Truncate or use as-is
    Bytes actual_payload = payload;
    if (actual_payload.size() > capacity) {
        actual_payload.resize(capacity);
    }

    // Create frame with explicit total_cw = 4
    DataFrame frame;
    frame.type = FrameType::DATA;
    frame.flags = Flags::VERSION_V2;
    frame.seq = seq;
    frame.src_hash = hashCallsign(src);
    frame.dst_hash = hashCallsign(dst);
    frame.payload = actual_payload;
    frame.payload_len = static_cast<uint16_t>(actual_payload.size());
    frame.total_cw = FIXED_FRAME_CODEWORDS;  // Always 4

    return frame;
}

} // namespace v2
} // namespace protocol
} // namespace ultra
