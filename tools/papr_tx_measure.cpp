// papr_tx_measure.cpp — Throwaway empirical TX-level measurement (V1).
//
// Produces a REAL OFDM_CHIRP coherent QPSK R2/3 burst GROUP through the
// PRODUCTION StreamingEncoder path (the exact transmitBurst config: z=27, cw=4,
// BURST_HEADER descriptor on, burst-interleave on), then measures, per frame in
// the group:
//   - in-band RMS  (ultra::sim::measureTxBurstInBandRms / referenceBand FIR)
//   - broadband RMS
//   - peak
// under BOTH normalizeTxBurstForHardware(samples, 0.5) and
// normalizeTxBurstToReference(samples).
//
// PHY note: both norms apply ONE scalar gain to the WHOLE block, so per-frame
// RELATIVE levels are identical across norms; only the absolute scalar differs.
// We therefore measure the raw group once, then apply each norm's scalar and the
// RX erasure-window math (BURST_ERASURE_RMS_THRESHOLD = 0.015, window
// [1024 : 1024 + min(block-1024, 5000)]) exactly as streaming_burst_interleave.cpp.

#include "gui/modem/streaming_encoder.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/tx_burst_normalization.hpp"
#include "ultra/types.hpp"
#include "ota_channel_core/ota_channel_core/models.hpp"
#include "ultra/ofdm_link_adaptation.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <string>
#include <vector>

using ultra::Bytes;
namespace gui = ultra::gui;
namespace protocol = ultra::protocol;
namespace v2 = ultra::protocol::v2;
namespace sim = ultra::sim;

namespace {

constexpr float kErasureThreshold = 0.015f;  // streaming_burst_interleave.cpp:315

ultra::ModemConfig makeOFDMConfig(ultra::Modulation mod, ultra::CodeRate rate) {
    ultra::ModemConfig cfg;
    cfg.fft_size = 1024;
    cfg.num_carriers = 59;
    cfg.sample_rate = 48000;
    cfg.center_freq = 1500.0f;
    cfg.cp_mode = ultra::CyclicPrefixMode::LONG;
    cfg.modulation = mod;
    cfg.code_rate = rate;
    cfg.use_pilots = true;
    cfg.pilot_spacing = ultra::ofdm_link_adaptation::recommendedPilotSpacing(mod, rate);
    return cfg;
}

// Plain RMS over [begin, end).
double rmsRange(const std::vector<float>& x, size_t begin, size_t end) {
    if (end <= begin) return 0.0;
    double s = 0.0;
    for (size_t i = begin; i < end; ++i) s += double(x[i]) * double(x[i]);
    return std::sqrt(s / double(end - begin));
}

// Exact RX erasure-gate RMS: window [1024 : 1024 + min(block-1024, 5000)]
// relative to the start of a frame block. (streaming_burst_interleave.cpp:306-314)
double rxErasureRms(const std::vector<float>& x, size_t blk_begin, size_t blk_len) {
    size_t check_start = std::min<size_t>(1024, blk_len);
    size_t check_len = std::min<size_t>(blk_len - check_start, 5000);
    if (check_len == 0) return 0.0;
    double s = 0.0;
    for (size_t i = 0; i < check_len; ++i) {
        float v = x[blk_begin + check_start + i];
        s += double(v) * double(v);
    }
    return std::sqrt(s / double(check_len));
}

double peakRange(const std::vector<float>& x, size_t begin, size_t end) {
    double p = 0.0;
    for (size_t i = begin; i < end; ++i) p = std::max(p, double(std::fabs(x[i])));
    return p;
}

double db20(double a, double b) { return 20.0 * std::log10((a + 1e-30) / (b + 1e-30)); }

}  // namespace

int main() {
    const ultra::Modulation mod = ultra::Modulation::QPSK;
    const ultra::CodeRate rate = ultra::CodeRate::R2_3;
    const int frame_cw = 4;          // production z=27 OFDM data cw
    const uint8_t z = 27;
    const int data_frames = 5;       // "anchor + ~5 data frames"

    // ---- Configure encoder EXACTLY like ModemEngine::transmitBurst ----
    gui::StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(makeOFDMConfig(mod, rate));
    encoder.setDataMode(mod, rate);
    encoder.setFixedFrameCodewords(frame_cw);
    encoder.setLDPCLiftingZ(z);
    encoder.setBurstInterleave(true);
    encoder.setBurstGroupSeq(0);
    encoder.setBurstInterleaveGroupSize(data_frames);  // transmitBurst sizes group = frame count
    encoder.setBurstDescriptorEnabled(true);            // production default ON
    encoder.setBurstDescriptorIdentity("", "");

    // Build the data frames (20-byte payloads, like the real file chunker).
    std::mt19937_64 rng(0xC0FFEEull);
    std::vector<Bytes> frames;
    for (int f = 0; f < data_frames; ++f) {
        Bytes payload(20);
        for (auto& b : payload) b = uint8_t(rng() & 0xFF);
        auto frame = v2::makeFixedDataFrame("ALPHA", "BRAVO",
                                            uint16_t(f), payload, rate, frame_cw);
        frames.push_back(frame.serialize());
    }

    // ---- Encode the REAL burst group (one contiguous waveform) ----
    std::vector<float> group = encoder.encodeBurstLight(frames);
    if (group.empty()) {
        std::fprintf(stderr, "encodeBurstLight produced no samples\n");
        return 1;
    }

    // ---- Walk frame block boundaries the way the RX does ----
    // The descriptor (BURST_HEADER) is a full-anchor 1-CW control frame; the group
    // -start data frame carries a full chirp+LTS anchor (force_first_full_preamble),
    // then data members ride light-LTS at a fixed stride getMinSamplesForCWCount(cw).
    // We reconstruct boundaries by regenerating each segment's size from the waveform.
    ultra::IWaveform* wf = encoder.getWaveform();
    const int data_block = wf->getMinSamplesForCWCount(frame_cw);  // light-LTS data stride
    const int ctrl_block = wf->getMinSamplesForCWCount(1);         // 1-CW control stride

    // The first segment is the full-anchor descriptor (control, full chirp) and the
    // first data frame is full-anchor too. To avoid fragile boundary guessing, we
    // segment by the KNOWN construction: [descriptor: full-anchor ctrl][data0:
    // full-anchor][data1..N-1: light-LTS data]. We measure on the light-LTS data
    // stride for members and report the leading anchor region separately.
    // Robust approach: measure the WHOLE group + per-light-data-block from the tail.
    //
    // Tail-anchored walk: the last (data_frames-1) blocks are light-LTS data blocks
    // of size data_block; everything before is the anchor region (descriptor + first
    // full-anchor data frame). This needs no chirp-size constant.
    size_t total = group.size();
    int light_members = data_frames - 1;  // group-start frame is full-anchor
    size_t tail = size_t(light_members) * size_t(data_block);
    size_t anchor_region_end = (tail <= total) ? (total - tail) : 0;

    struct FrameSeg { std::string label; size_t begin; size_t len; };
    std::vector<FrameSeg> segs;
    segs.push_back({"anchor+desc+dataframe0 (full chirp)", 0, anchor_region_end});
    for (int m = 0; m < light_members; ++m) {
        segs.push_back({"data frame " + std::to_string(m + 1) + " (light-LTS)",
                        anchor_region_end + size_t(m) * size_t(data_block),
                        size_t(data_block)});
    }

    // ---- Compute the two whole-block normalization SCALARS ----
    // normalizeTxBurstForHardware(0.5): gain = 0.5 / global_peak  (single scalar)
    std::vector<float> hw_copy = group;
    sim::TxBurstHardwareMeasurement hwm = sim::normalizeTxBurstForHardware(hw_copy, 0.5f);
    double hw_gain = hwm.gain_to_target;

    // normalizeTxBurstToReference: gain = kModemReferenceInBandRms / group_in_band_rms
    std::vector<float> ref_copy = group;
    sim::TxBurstRmsMeasurement refm = sim::normalizeTxBurstToReference(ref_copy);
    double ref_gain = refm.gain_to_reference;

    std::printf("=== RAW GROUP (un-normalized encodeBurstLight output) ===\n");
    std::printf("samples=%zu  global_peak=%.5f\n", total, peakRange(group, 0, total));
    sim::TxBurstRmsMeasurement raw = sim::measureTxBurstInBandRms(group);
    std::printf("whole-group in-band RMS=%.6f  broadband RMS=%.6f  active=%zu\n\n",
                raw.in_band_rms, raw.broadband_rms, raw.active_samples);

    std::printf("=== NORMALIZATION SCALARS (single gain over whole block) ===\n");
    std::printf("HW peak-norm(0.5):  gain=%.5f  (peak_before=%.5f peak_after=%.5f)\n",
                hw_gain, hwm.peak_before_gain, hwm.peak_after_gain);
    std::printf("SIM ref RMS-norm:   gain=%.5f  (target in-band RMS=%.6f)\n",
                ref_gain, ultra::ota_channel_core::kModemReferenceInBandRms);
    std::printf("ratio ref/hw gain = %.4f  (%+.2f dB sim-RMS advantage on EVERY frame)\n\n",
                ref_gain / hw_gain, db20(ref_gain, hw_gain));

    // ---- Per-frame measurement under BOTH norms ----
    std::printf("=== PER-FRAME (raw values × each norm's scalar) ===\n");
    std::printf("%-38s | %-30s | %-30s | %-30s\n",
                "frame", "HW peak-norm(0.5)", "SIM RMS-norm", "RX-erasure-window RMS");
    std::printf("%-38s | inbandRMS broadband  peak  | inbandRMS broadband  peak  | hw-norm   sim-norm  vs0.015\n", "");

    double anchor_inband_hw = 0.0, anchor_inband_ref = 0.0;
    for (size_t si = 0; si < segs.size(); ++si) {
        const auto& s = segs[si];
        if (s.len == 0) continue;
        size_t end = s.begin + s.len;
        double ib  = rmsRange(group, s.begin, end);
        double bb  = ib; // placeholder; recompute broadband below
        // broadband = plain RMS; in-band needs FIR. measureTxBurstInBandRms wants a
        // contiguous active region. We slice the segment and measure it standalone.
        std::vector<float> slice(group.begin() + s.begin, group.begin() + end);
        sim::TxBurstRmsMeasurement sm = sim::measureTxBurstInBandRms(slice);
        double seg_inband = sm.in_band_rms;
        double seg_broad  = sm.broadband_rms;
        double seg_peak   = peakRange(group, s.begin, end);
        (void)ib; (void)bb;

        // RX erasure-window RMS (broadband, plain) per the gate code, on the RAW slice.
        double erx_raw = rxErasureRms(group, s.begin, s.len);

        // Under each norm (single scalar applied to whole block):
        double hw_inband = seg_inband * hw_gain;
        double hw_broad  = seg_broad  * hw_gain;
        double hw_peak   = seg_peak   * hw_gain;
        double ref_inband = seg_inband * ref_gain;
        double ref_broad  = seg_broad  * ref_gain;
        double ref_peak   = seg_peak   * ref_gain;
        double erx_hw  = erx_raw * hw_gain;
        double erx_ref = erx_raw * ref_gain;

        const char* tag_hw  = (erx_hw  < kErasureThreshold) ? "ERASE" : "ok";
        const char* tag_ref = (erx_ref < kErasureThreshold) ? "ERASE" : "ok";

        std::printf("%-38s | %.5f %.5f %.5f | %.5f %.5f %.5f | hw=%.5f(%s) sim=%.5f(%s)\n",
                    s.label.c_str(),
                    hw_inband, hw_broad, hw_peak,
                    ref_inband, ref_broad, ref_peak,
                    erx_hw, tag_hw, erx_ref, tag_ref);

        if (si == 0) { anchor_inband_hw = hw_inband; anchor_inband_ref = ref_inband; }
        else if (si == 1) {
            // first light data frame vs anchor delta
        }
    }

    // ---- Group PAPR (in-band) and data-vs-anchor delta ----
    // In-band group PAPR = peak / in-band RMS of the whole group (dB).
    double group_peak = peakRange(group, 0, total);
    double group_papr_db = db20(group_peak, raw.in_band_rms);
    // Broadband PAPR for reference too.
    double group_papr_bb_db = db20(group_peak, raw.broadband_rms);

    std::printf("\n=== GROUP-LEVEL ===\n");
    std::printf("group in-band PAPR = %.2f dB (peak %.5f / in-band RMS %.6f)\n",
                group_papr_db, group_peak, raw.in_band_rms);
    std::printf("group broadband PAPR = %.2f dB (peak %.5f / broadband RMS %.6f)\n",
                group_papr_bb_db, group_peak, raw.broadband_rms);

    // data-frame RMS minus anchor-frame RMS (single scalar => identical under both norms)
    // Recompute a representative data-frame and the anchor-region in-band RMS.
    {
        // anchor region
        std::vector<float> aslice(group.begin(), group.begin() + anchor_region_end);
        sim::TxBurstRmsMeasurement am = sim::measureTxBurstInBandRms(aslice);
        // representative data frame (last light member)
        size_t db_begin = anchor_region_end + size_t(light_members - 1) * size_t(data_block);
        std::vector<float> dslice(group.begin() + db_begin, group.begin() + db_begin + data_block);
        sim::TxBurstRmsMeasurement dm = sim::measureTxBurstInBandRms(dslice);
        double delta_db = db20(dm.in_band_rms, am.in_band_rms);
        std::printf("data-frame in-band RMS=%.6f  anchor-region in-band RMS=%.6f  data-anchor=%+.2f dB\n",
                    dm.in_band_rms, am.in_band_rms, delta_db);
        std::printf("(same under both norms: single scalar gain) data-anchor(broadband)=%+.2f dB\n",
                    db20(dm.broadband_rms, am.broadband_rms));

        // Data-frame absolute RMS under each norm vs the 0.015 gate:
        double d_ib_hw  = dm.in_band_rms * hw_gain;
        double d_ib_ref = dm.in_band_rms * ref_gain;
        double d_bb_hw  = dm.broadband_rms * hw_gain;
        double d_bb_ref = dm.broadband_rms * ref_gain;
        std::printf("\n=== DATA FRAME vs 0.015 GATE ===\n");
        std::printf("HW peak-norm(0.5): data in-band=%.5f broadband=%.5f  (gate 0.015, margin x%.1f bb)\n",
                    d_ib_hw, d_bb_hw, d_bb_hw / kErasureThreshold);
        std::printf("SIM RMS-norm:      data in-band=%.5f broadband=%.5f  (gate 0.015, margin x%.1f bb)\n",
                    d_ib_ref, d_bb_ref, d_bb_ref / kErasureThreshold);
        std::printf("peak-norm loses %+.2f dB (broadband) vs RMS-norm on the data frames\n",
                    db20(d_bb_hw, d_bb_ref));
    }

    // ---- Where does the global peak (which SETS the hardware gain) live? ----
    {
        size_t peak_idx = 0; double pk = 0.0;
        for (size_t i = 0; i < total; ++i) {
            double a = std::fabs(group[i]);
            if (a > pk) { pk = a; peak_idx = i; }
        }
        const char* where = (peak_idx < anchor_region_end) ? "ANCHOR region (chirp+LTS+data0)"
                                                           : "a light-LTS DATA frame";
        std::printf("\n=== PEAK LOCATION (sets the HW 0.5/peak scalar) ===\n");
        std::printf("global peak %.5f at sample %zu of %zu -> %s\n",
                    pk, peak_idx, total, where);
        // Per-segment standalone PAPR (peak/in-band-RMS), i.e. what each frame would
        // self-normalize to. Shows the data frames are NOT individually high-PAPR
        // enough to explain a 10 dB hit — the group peak (chirp anchor) is.
        for (const auto& s : segs) {
            if (s.len == 0) continue;
            std::vector<float> slice(group.begin() + s.begin, group.begin() + s.begin + s.len);
            sim::TxBurstRmsMeasurement m = sim::measureTxBurstInBandRms(slice);
            double pseg = peakRange(group, s.begin, s.begin + s.len);
            std::printf("  %-38s standalone PAPR(in-band)=%.2f dB (peak %.4f / RMS %.5f)\n",
                        s.label.c_str(), db20(pseg, m.in_band_rms), pseg, m.in_band_rms);
        }
    }

    return 0;
}

// ── appended probe: where is the global peak, and per-segment PAPR ──────────
// (compiled out unless PAPR_PROBE defined)
