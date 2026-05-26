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

inline OFDMControlProfile profileForDataMode(Modulation data_mod,
                                             bool coherent_control_enabled = true) {
    // Control frames keep the robust R1/4 LDPC payload. The modulation follows
    // the data regime only after the OFDM handshake is confirmed. Until then,
    // preserve the legacy DQPSK control profile so CONNECT/CONNECT_ACK and the
    // responder's unconfirmed handoff cannot inherit coherent-data sizing.
    if (!coherent_control_enabled) {
        return {Modulation::DQPSK, CodeRate::R1_4};
    }

    // Coherent links use coherent QPSK control so warm sync, equalization, and
    // pilot tracking stay in the same receiver family; differential links keep
    // the existing DQPSK control profile.
    return {
        ofdm_link_adaptation::isCoherentModulation(data_mod)
            ? Modulation::QPSK
            : Modulation::DQPSK,
        CodeRate::R1_4
    };
}

inline int pilotSpacingForProfile(const OFDMControlProfile& profile) {
    return ofdm_link_adaptation::recommendedPilotSpacing(profile.modulation,
                                                         profile.rate);
}

}  // namespace streaming_control_profile
}  // namespace gui
}  // namespace ultra
