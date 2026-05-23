#include "gui/modem/modem_types.hpp"
#include "gui/modem/streaming_decoder.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using ultra::SNRSource;
using ultra::gui::DecodeResult;
using ultra::gui::LoopbackStats;
using ultra::gui::selectOperatorSNRDisplay;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

LoopbackStats statsFromDecode(const DecodeResult& result) {
    LoopbackStats stats;
    stats.snr_db = result.snr_db;
    stats.snr_source = result.snr_source;
    stats.has_idle_in_band_snr_db = result.has_idle_in_band_snr_db;
    stats.idle_in_band_snr_db = result.idle_in_band_snr_db;
    stats.has_ofdm_broadband_snr_db = result.has_ofdm_broadband_snr_db;
    stats.ofdm_broadband_snr_db = result.ofdm_broadband_snr_db;
    stats.ofdm_internal_snr_db = result.ofdm_internal_snr_db;
    stats.sync_quality_db = result.sync_quality_db;
    return stats;
}

void connectedOFDMKeepsBroadbandSourceForConsumers() {
    DecodeResult result;
    result.snr_db = 15.4f;
    result.snr_source = SNRSource::OFDM_BROADBAND;
    result.has_idle_in_band_snr_db = true;
    result.idle_in_band_snr_db = -0.7f;
    result.has_ofdm_broadband_snr_db = true;
    result.ofdm_broadband_snr_db = 15.4f;
    result.ofdm_internal_snr_db = 24.3f;
    result.sync_quality_db = 27.4f;

    const LoopbackStats stats = statsFromDecode(result);
    check(stats.snr_db == 15.4f, "consumer SNR value changed");
    check(stats.snr_source == SNRSource::OFDM_BROADBAND,
          "connected OFDM data must route OFDM broadband source");

    const auto display = selectOperatorSNRDisplay(stats);
    check(display.valid, "OFDM broadband should be displayable when idle is present");
    check(display.snr_db == 15.4f,
          "connected OFDM display must not prefer stale idle in-band value");
    check(display.source == SNRSource::OFDM_BROADBAND,
          "OFDM-only display should be labeled broadband");
}

void idleMeterPrefersInBandSource() {
    LoopbackStats stats;
    stats.snr_db = 24.6f;
    stats.snr_source = SNRSource::IDLE_IN_BAND;
    stats.has_idle_in_band_snr_db = true;
    stats.idle_in_band_snr_db = 24.6f;
    stats.has_ofdm_broadband_snr_db = true;
    stats.ofdm_broadband_snr_db = 15.4f;

    const auto display = selectOperatorSNRDisplay(stats);
    check(display.valid, "idle in-band SNR should be displayable");
    check(display.snr_db == 24.6f, "operator display should use idle in-band value");
    check(display.source == SNRSource::IDLE_IN_BAND,
          "operator display should label idle in-band source");
}

void connectedMCDPSKKeepsConnectedSourceForConsumers() {
    LoopbackStats stats;
    stats.snr_db = 8.5f;
    stats.snr_source = SNRSource::MCDPSK_IN_BAND;
    stats.has_idle_in_band_snr_db = true;
    stats.idle_in_band_snr_db = -1.0f;

    const auto display = selectOperatorSNRDisplay(stats);
    check(display.valid, "MC-DPSK connected SNR should be displayable");
    check(display.snr_db == 8.5f,
          "connected MC-DPSK display must not prefer stale idle in-band value");
    check(display.source == SNRSource::MCDPSK_IN_BAND,
          "connected MC-DPSK display should be labeled MC-DPSK in-band");
}

void chirpOnlyDoesNotLookLikeAnSNRMeter() {
    LoopbackStats stats;
    stats.snr_db = 27.4f;
    stats.snr_source = SNRSource::SYNC_QUALITY;
    stats.sync_quality_db = 27.4f;

    const auto display = selectOperatorSNRDisplay(stats);
    check(!display.valid, "chirp-only sync quality must not drive the SNR meter");
    check(display.source != SNRSource::SYNC_QUALITY,
          "operator display must not label sync quality as SNR");
}

} // namespace

int main() {
    try {
        connectedOFDMKeepsBroadbandSourceForConsumers();
        idleMeterPrefersInBandSource();
        connectedMCDPSKKeepsConnectedSourceForConsumers();
        chirpOnlyDoesNotLookLikeAnSNRMeter();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }

    std::cout << "SNR source routing tests passed\n";
    return 0;
}
