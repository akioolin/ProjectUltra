#pragma once

#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/types.hpp"

namespace ultra {
namespace gui {
namespace streaming_control_profile {

struct OFDMControlProfile {
    Modulation modulation = Modulation::DQPSK;
    CodeRate rate = CodeRate::R1_4;
};

inline OFDMControlProfile profileForDataMode(Modulation /*data_mod*/) {
    // Coherent-only OFDM (thread A 2026-05-31): OFDM control frames always ride
    // coherent QPSK R1/4, keeping warm sync, equalization, and pilot tracking in
    // the same receiver family as the (coherent) data. The robust R1/4 LDPC
    // payload is preserved. The legacy DQPSK control profile is gone — differential
    // lives in MC-DPSK, which has its own control path and does not use this
    // profile (CONNECT/CONNECT_ACK ride MC-DPSK control, a separate decoder).
    return {Modulation::QPSK, CodeRate::R1_4};
}

// #72 (2026-06-28): MC-DPSK control/handshake frames (CONNECT/CONNECT_ACK) ride a
// FIXED DBPSK R1/4 profile — the analog of OFDM's QPSK-R1/4 control profile. MC-DPSK
// baud is standardized at sps=1024 (connection_policy ladder), so the encoder reaches
// this profile with a plain configure(DBPSK, R1/4): the constellation swaps, the baud
// is preserved, and the peer — whose control decoder is the default DBPSK/1024 — can
// always decode the handshake regardless of the negotiated DATA constellation (DQPSK
// rungs). Without it the CONNECT_ACK shipped at the data mod and the initiator stranded.
inline OFDMControlProfile profileForMCDPSK() {
    return {Modulation::DBPSK, CodeRate::R1_4};
}

inline int pilotSpacingForProfile(const OFDMControlProfile& profile) {
    return ofdm_link_adaptation::recommendedPilotSpacing(profile.modulation,
                                                         profile.rate);
}

}  // namespace streaming_control_profile
}  // namespace gui
}  // namespace ultra
