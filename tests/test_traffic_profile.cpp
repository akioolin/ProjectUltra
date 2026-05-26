// Unit test for the traffic-class PHY profile selector (src/protocol/traffic_profile.hpp).
// Locks the gating rule: ACK/control -> Control, DATA(chat) -> Chat, file
// segments / DATA-during-transfer -> File; and that only File carries the
// aggressive long-LDPC + deep-interleave + thin-pilot knobs.

#include "protocol/traffic_profile.hpp"
#include "fec/frame_interleaver.hpp"
#include <cstdio>
#include <vector>
#include <random>

using ultra::protocol::TrafficClass;
using ultra::protocol::classifyTraffic;
using ultra::protocol::profileFor;
using ultra::fec::FrameInterleaver;
using FT = ultra::protocol::v2::FrameType;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++g_fail; } } while (0)

namespace v2 = ultra::protocol::v2;

static int infoBlocks(ultra::CodeRate r) {
    switch (r) {
        case ultra::CodeRate::R1_4: return 6;
        case ultra::CodeRate::R1_2: return 12;
        case ultra::CodeRate::R2_3: return 16;
        case ultra::CodeRate::R3_4: return 18;
        default:                    return 12;
    }
}

// Full frame-level round-trip through encodeFixedFrame -> clean-channel soft LLRs
// -> decodeFixedFrame at lifting size Z. Proves the n=1944 file codeword flows
// end-to-end through the frame codec, and that Z=27 stays correct.
static bool frameRoundTrip(ultra::CodeRate rate, int cw_count, int lifting_z, size_t bps) {
    const size_t bytes_per_cw = (size_t)infoBlocks(rate) * (size_t)lifting_z / 8;
    const int codeword_bits = 24 * lifting_z;
    std::vector<uint8_t> data(bytes_per_cw * cw_count);
    std::mt19937 rng(2024);
    std::uniform_int_distribution<int> bytev(0, 255);
    for (auto& b : data) b = (uint8_t)bytev(rng);

    auto coded = v2::encodeFixedFrame(data, rate, cw_count, /*channel_interleave=*/true, bps, lifting_z);
    // Clean channel: byte bits -> strong LLR (bit1 -> -, bit0 -> +).
    std::vector<float> soft;
    soft.reserve(coded.size() * 8);
    for (uint8_t b : coded)
        for (int i = 7; i >= 0; --i) soft.push_back(((b >> i) & 1) ? -8.0f : +8.0f);
    soft.resize((size_t)cw_count * codeword_bits);

    auto st = v2::decodeFixedFrame(soft, rate, cw_count, /*channel_deinterleave=*/true, bps,
                                   nullptr, nullptr, lifting_z);
    // Verify the LDPC+interleaver codec recovered the exact info bytes at this
    // codeword length. We check st.data (the recovered payload), NOT st.decoded:
    // decodeFixedFrame also runs a frame-HEADER validity check that (correctly)
    // rejects our random non-frame test bytes as a false positive — that is a
    // higher-layer concern, not a codeword-length-correctness concern.
    for (int cw = 0; cw < cw_count; ++cw) {
        if (cw >= (int)st.data.size() || st.data[cw].size() < bytes_per_cw) return false;
        for (size_t i = 0; i < bytes_per_cw; ++i)
            if (st.data[cw][i] != data[cw * bytes_per_cw + i]) return false;
    }
    return true;
}

// FrameInterleaver round-trip at a given codeword length: interleaveSoft ->
// deinterleave must recover every soft bit exactly (length-aware correctness).
static bool frameInterleaverRoundTrip(int cw_count, int bits_per_cw) {
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> u(-9.0f, 9.0f);
    std::vector<std::vector<float>> cws(cw_count, std::vector<float>(bits_per_cw));
    for (auto& cw : cws) for (auto& v : cw) v = u(rng);

    auto inter = FrameInterleaver::interleaveSoft(cws, cw_count, bits_per_cw);
    if ((int)inter.size() != cw_count * bits_per_cw) return false;
    auto out = FrameInterleaver::deinterleave(inter, cw_count, bits_per_cw);
    if ((int)out.size() != cw_count) return false;
    for (int c = 0; c < cw_count; ++c) {
        if ((int)out[c].size() != bits_per_cw) return false;
        for (int b = 0; b < bits_per_cw; ++b) if (out[c][b] != cws[c][b]) return false;
    }
    return true;
}

int main() {
    // --- classification ---
    CHECK(classifyTraffic(FT::ACK, false)         == TrafficClass::Control, "ACK -> Control");
    CHECK(classifyTraffic(FT::NACK, true)         == TrafficClass::Control, "NACK -> Control even during file xfer");
    CHECK(classifyTraffic(FT::CONNECT, false)     == TrafficClass::Control, "CONNECT -> Control");
    CHECK(classifyTraffic(FT::KEEPALIVE, false)   == TrafficClass::Control, "KEEPALIVE -> Control");

    CHECK(classifyTraffic(FT::DATA, false)        == TrafficClass::Chat,    "DATA (no xfer) -> Chat");
    CHECK(classifyTraffic(FT::DATA, true)         == TrafficClass::File,    "DATA during xfer -> File");

    CHECK(classifyTraffic(FT::DATA_START, false)  == TrafficClass::File,    "DATA_START -> File");
    CHECK(classifyTraffic(FT::DATA_CONT, false)   == TrafficClass::File,    "DATA_CONT -> File");
    CHECK(classifyTraffic(FT::DATA_END, false)    == TrafficClass::File,    "DATA_END -> File");

    // --- profiles: only File is aggressive; Control/Chat stay on the default ---
    auto ctrl = profileFor(TrafficClass::Control);
    auto chat = profileFor(TrafficClass::Chat);
    auto file = profileFor(TrafficClass::File);

    CHECK(ctrl.ldpc_lifting_z == 27 && !ctrl.deep_interleave && ctrl.pilot_spacing == 5,
          "Control = default n=648, dense pilots, no deep interleave");
    CHECK(chat.ldpc_lifting_z == 27 && !chat.deep_interleave && chat.pilot_spacing == 5,
          "Chat = default n=648, dense pilots, no deep interleave");
    CHECK(file.ldpc_lifting_z == 81, "File uses n=1944 long LDPC (Z=81)");
    CHECK(file.deep_interleave && file.interleave_depth >= 8, "File uses deep cross-frame interleave");
    CHECK(file.pilot_spacing > chat.pilot_spacing, "File thins pilots vs chat (more data carriers)");

    // --- FrameInterleaver is length-aware: round-trips at 648 (default) AND 1944 (long) ---
    CHECK(frameInterleaverRoundTrip(4, 648),  "FrameInterleaver round-trip 4x648 (default, behavior-preserving)");
    CHECK(frameInterleaverRoundTrip(4, 1944), "FrameInterleaver round-trip 4x1944 (file-class long code)");
    CHECK(frameInterleaverRoundTrip(2, 1944), "FrameInterleaver round-trip 2x1944");
    CHECK(frameInterleaverRoundTrip(1, 1944), "FrameInterleaver round-trip 1x1944");

    // --- full frame codec round-trip: default n=648 AND file-class n=1944 ---
    CHECK(frameRoundTrip(ultra::CodeRate::R2_3, 4, 27, 94), "encode/decodeFixedFrame R2/3 4x648 (default)");
    CHECK(frameRoundTrip(ultra::CodeRate::R2_3, 4, 81, 94), "encode/decodeFixedFrame R2/3 4x1944 (file long code)");
    CHECK(frameRoundTrip(ultra::CodeRate::R1_2, 4, 81, 94), "encode/decodeFixedFrame R1/2 4x1944 (file long code)");
    CHECK(frameRoundTrip(ultra::CodeRate::R3_4, 4, 81, 188), "encode/decodeFixedFrame R3/4 4x1944 (file long code)");

    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
