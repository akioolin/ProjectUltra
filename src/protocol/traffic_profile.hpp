#pragma once

// Traffic-class PHY profiles — the per-frame structural overrides that let a
// FILE transfer run an aggressive high-throughput PHY (long LDPC + deep time
// interleave + thin pilots) while chat messages and ACK/control frames stay on
// the responsive default. The discriminator is the FRAME TYPE plus, for a plain
// DATA frame, whether a file transfer is active:
//
//   ACK / control                       -> Control profile  (lean, instant)
//   DATA (chat text message)            -> Chat profile     (low latency)
//   DATA_START/CONT/END/REPAIR (file)   -> File profile     (throughput + fade
//                                                            survival, latency OK)
//   DATA + file_transfer_active         -> File profile
//
// WHY ACK stays lean: an ACK gates the half-duplex ARQ loop — the sender sits in
// RX waiting for it before the next burst. Deep-interleaving an ACK would add the
// whole interleave span (~seconds) to every turnaround and destroy the throughput
// the file profile gained. A 1-CW ACK gets its diversity from repetition + a
// robust rate + dense pilots, not from interleaving.
//
// This header only declares the CLASSIFICATION + the target structural knobs;
// modulation and code rate remain driven by SNR negotiation (the file class may
// later bias the rate higher because the deep interleave buys back the margin).
// Wiring these overrides into the encoder/decoder/frame path is done in later,
// individually-measured steps.

#include "frame_v2.hpp"

namespace ultra {
namespace protocol {

enum class TrafficClass { Control, Chat, File };

// Map a frame type (+ active-file-transfer hint for a plain DATA frame) to its
// traffic class. Stateless except for the file-transfer hint.
inline TrafficClass classifyTraffic(v2::FrameType type, bool file_transfer_active) {
    using FT = v2::FrameType;
    switch (type) {
        case FT::DATA_START:
        case FT::DATA_CONT:
        case FT::DATA_END:
        case FT::DATA_REPAIR:
            return TrafficClass::File;          // bulk file segments
        case FT::DATA:
            // A plain DATA frame is an interactive text message UNLESS it is
            // riding inside an active file transfer.
            return file_transfer_active ? TrafficClass::File : TrafficClass::Chat;
        default:
            return TrafficClass::Control;       // PING/CONNECT/ACK/NACK/...
    }
}

// Structural PHY knobs that the traffic class overrides. Modulation/code-rate
// stay with SNR negotiation and are intentionally NOT here.
struct TrafficProfile {
    int  ldpc_lifting_z;    // 27 -> n=648 codeword, 81 -> n=1944 long codeword
    int  pilot_spacing;     // smaller = denser pilots (better CSI, fewer data carriers)
    bool deep_interleave;   // cross-frame time interleave (fade diversity)
    int  interleave_depth;  // # frames spanned when deep_interleave (else 1)
};

// Target profiles. Control/Chat are today's proven defaults (n=648, dense
// pilots, no cross-frame interleave). File is the coupled high-throughput
// config; its knobs are activated + measured incrementally in later steps.
inline TrafficProfile profileFor(TrafficClass cls) {
    switch (cls) {
        case TrafficClass::File:
            return TrafficProfile{ /*z=*/81, /*pilot_spacing=*/10,
                                   /*deep_interleave=*/true, /*interleave_depth=*/16 };
        case TrafficClass::Control:
        case TrafficClass::Chat:
        default:
            return TrafficProfile{ /*z=*/27, /*pilot_spacing=*/5,
                                   /*deep_interleave=*/false, /*interleave_depth=*/1 };
    }
}

}  // namespace protocol
}  // namespace ultra
