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

inline int pilotSpacingForProfile(const OFDMControlProfile& profile) {
    return ofdm_link_adaptation::recommendedPilotSpacing(profile.modulation,
                                                         profile.rate);
}

}  // namespace streaming_control_profile
}  // namespace gui
}  // namespace ultra
