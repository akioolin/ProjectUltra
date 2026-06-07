#pragma once

#include "gui/modem/modem_engine.hpp"
#include "protocol/protocol_engine.hpp"

#include <functional>
#include <utility>
#include <vector>

namespace ultra {
namespace gui {

// wireModemToProtocol — the SINGLE place the modem (ModemEngine) is bound to the protocol
// (ProtocolEngine). BOTH frontends (the GUI app and headless ultra_tnc) call this, so the
// modem->protocol forwarding lives in exactly ONE spot: RX-data, burst-group delivery,
// data-sync acceptance, tone-burst GROUP_ACK, and HARQ provisional context.
//
// Why this exists: previously this glue lived inline in app.cpp ONLY. ultra_tnc owns a
// ModemEngine too but re-wired a *subset* of the callbacks by hand, silently MISSING
// setBurstGroupCallback + setToneBurstAckCallback -> it decoded burst frames it never
// assembled/delivered/ACKed, so file transfer timed out. Centralizing the binding makes
// that whole divergence class structurally impossible: add a forwarding here once, both
// frontends get it.
//
// Frontend-specific REACTIONS are deliberately NOT in this binding — ping/status callbacks
// stay frontend-owned (GUI: UI log lines; TNC: operator log), and the optional
// `after_rx_data` hook lets a frontend observe each decoded frame AFTER the core protocol
// forwarding (GUI: monitor-mode log + adaptive advisory) without re-implementing the core.
struct ModemProtocolFrontendHooks {
    // Fires after the core RX-data -> protocol forwarding (SNR measurement + onRxData).
    // GUI uses it for monitor-mode logging + adaptive advisory; TNC leaves it empty.
    std::function<void(const Bytes& data, float snr_db, float fading,
                       SNRSource snr_source, bool used_for_quality)>
        after_rx_data;
};

inline void wireModemToProtocol(ModemEngine& modem,
                                protocol::ProtocolEngine& protocol,
                                ModemProtocolFrontendHooks hooks = {}) {
    // HARQ provisional context: the decoder pulls it from the protocol's soft-combine
    // buffer when assembling provisional codewords.
    modem.setHarqProvisionalContextCallback(
        [&protocol]() { return protocol.harqProvisionalContext(); });

    // Core RX-data path: every decoded frame -> SNR/channel-quality measurement + onRxData.
    // The frontend hook observes afterwards (no duplication of the core forwarding).
    modem.setRawDataCallback(
        [&modem, &protocol, hooks = std::move(hooks)](const Bytes& data) {
            const auto stats = modem.getStats();
            const float snr_db = stats.snr_db;
            const SNRSource snr_source = stats.snr_source;
            const float fading = modem.getFadingIndex();
            const bool use_quality = protocol.shouldUseRxFrameForChannelQuality(data);
            protocol.setMeasuredSNR(snr_db, snr_source);
            if (use_quality) {
                protocol.setChannelQuality(snr_db, fading, snr_source);
            }
            protocol.onRxData(data);
            if (hooks.after_rx_data) {
                hooks.after_rx_data(data, snr_db, fading, snr_source, use_quality);
            }
        });

    // §14.27: a decoded interleaved burst delivered as a unit -> protocol burst transport
    // (deliver-once + single GROUP_ACK). THIS is the forwarding ultra_tnc was missing.
    modem.setBurstGroupCallback(
        [&protocol](uint16_t group_seq, const std::vector<Bytes>& frames, bool all_ok,
                    float quality, uint8_t frame_mask, bool interleaved, uint8_t group_size) {
            protocol.onBurstGroupReceived(group_seq, frames, all_ok, quality, frame_mask,
                                          interleaved, group_size);
        });

    // Accepted OFDM data-sync -> protocol (warm-sync / burst-cadence bookkeeping).
    modem.setDataSyncAcceptedCallback(
        [&protocol](float sync_correlation) {
            protocol.onAcceptedOFDMDataSync(sync_correlation);
        });

    // §15: tone-burst ACK detections from the always-on monitor -> protocol. Connection
    // matches against the in-flight burst group and either advances (ACK) or triggers a
    // fast resend (NACK). THIS is the other half ultra_tnc was missing (the GROUP_ACK
    // reverse path), so its sender never learned a group landed and retransmitted to the
    // retry cap.
    modem.setToneBurstAckCallback(
        [&protocol](const ultra::waveform::tone_burst_ack::ToneBurstAckDetection& d) {
            protocol.onToneBurstAck(d);
        });
}

}  // namespace gui
}  // namespace ultra
