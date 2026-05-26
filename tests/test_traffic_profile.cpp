// Unit test for the traffic-class PHY profile selector (src/protocol/traffic_profile.hpp).
// Locks the gating rule: ACK/control -> Control, DATA(chat) -> Chat, file
// segments / DATA-during-transfer -> File; and that only File carries the
// aggressive long-LDPC + deep-interleave + thin-pilot knobs.

#include "protocol/traffic_profile.hpp"
#include <cstdio>

using ultra::protocol::TrafficClass;
using ultra::protocol::classifyTraffic;
using ultra::protocol::profileFor;
using FT = ultra::protocol::v2::FrameType;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++g_fail; } } while (0)

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

    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
