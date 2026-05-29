// test_streaming_decoder_tone_burst_monitor.cpp — verifies that the
// tone-burst ACK monitor installed inside StreamingDecoder fires when
// audio passes through feedAudio(). PHY_ADAPTATION_DESIGN §15 step 4b.
//
// This is the integration validation: the unit-level monitor tests prove
// the detector works; this test proves the detector ALSO works when
// embedded in the StreamingDecoder's real feedAudio() path.

#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "waveform/tone_burst_ack/tone_burst_constants.hpp"
#include "waveform/tone_burst_ack/tone_burst_encoder.hpp"
#include "waveform/tone_burst_ack/tone_burst_payload.hpp"

#include <cstdio>
#include <mutex>
#include <vector>

namespace tba = ultra::waveform::tone_burst_ack;

namespace {

int g_failures = 0;

#define EXPECT(cond)                                                            \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                       \
    } while (0)

#define EXPECT_EQ(a, b)                                                         \
    do {                                                                        \
        const auto _va = (a);                                                   \
        const auto _vb = (b);                                                   \
        if (!(_va == _vb)) {                                                    \
            ++g_failures;                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s (%lld vs %lld)\n",       \
                         __FILE__, __LINE__, #a, #b,                            \
                         static_cast<long long>(_va),                           \
                         static_cast<long long>(_vb));                          \
        }                                                                       \
    } while (0)

tba::ToneBurstAckPayload makePayload() {
    tba::ToneBurstAckPayload p;
    p.group_seq = 42;
    p.frame_mask = 0b111100;
    p.rate_hint = 4;
    p.type = tba::AckType::Ack;
    return p;
}

void test_streaming_decoder_fires_tone_burst_callback_on_baseline_burst() {
    std::printf("[test] streaming_decoder_fires_tone_burst_callback_on_baseline_burst\n");

    ultra::gui::StreamingDecoder dec;

    // Install a test callback that captures the detection event.
    std::mutex mtx;
    std::vector<tba::ToneBurstAckDetection> events;
    dec.setToneBurstAckCallback([&](const tba::ToneBurstAckDetection& d) {
        std::lock_guard<std::mutex> lk(mtx);
        events.push_back(d);
    });

    // Encode a clean 25 ms/sym tone-burst.
    tba::ToneBurstEncoder enc;
    const auto orig = makePayload();
    const auto burst = enc.encode(orig, tba::kBaselineSymbolMs);

    // Feed: [200 ms silence][burst][200 ms silence] in 1024-sample chunks,
    // matching how the audio thread feeds StreamingDecoder.
    const size_t chunk_samples = 1024;
    std::vector<float> silence(chunk_samples, 0.0f);
    const size_t pad_chunks =
        (static_cast<size_t>(tba::kSampleRate) * 200u / 1000u) / chunk_samples;

    for (size_t i = 0; i < pad_chunks; ++i) {
        dec.feedAudio(silence.data(), silence.size());
    }
    size_t off = 0;
    while (off < burst.size()) {
        const size_t n = std::min(chunk_samples, burst.size() - off);
        dec.feedAudio(burst.data() + off, n);
        off += n;
    }
    // Trailing silence + a bit more to make sure the cadence-gated
    // detection actually triggers AFTER the burst is fully buffered.
    const size_t tail_chunks = pad_chunks + 24;  // ~720 ms total tail
    for (size_t i = 0; i < tail_chunks; ++i) {
        dec.feedAudio(silence.data(), silence.size());
    }

    // Verify the callback fired exactly once and decoded the right payload.
    std::lock_guard<std::mutex> lk(mtx);
    EXPECT_EQ(events.size(), static_cast<size_t>(1));
    EXPECT_EQ(dec.toneBurstAcksDetected(), static_cast<uint64_t>(1));
    if (events.size() >= 1) {
        const auto& d = events[0];
        EXPECT_EQ(d.payload.group_seq, orig.group_seq);
        EXPECT_EQ(d.payload.frame_mask, orig.frame_mask);
        EXPECT_EQ(d.payload.rate_hint, orig.rate_hint);
        EXPECT_EQ(static_cast<int>(d.payload.type), static_cast<int>(orig.type));
        EXPECT_EQ(d.symbol_ms_used, tba::kBaselineSymbolMs);
        EXPECT(d.correlation_peak > 500.0f);  // real burst should be ~1080
        std::printf("  symbol_ms=%u peak=%.1f stream_off=%llu\n",
                    static_cast<unsigned>(d.symbol_ms_used), d.correlation_peak,
                    static_cast<unsigned long long>(d.detected_stream_offset));
    }
}

void test_streaming_encoder_to_decoder_loopback() {
    std::printf("[test] streaming_encoder_to_decoder_loopback\n");
    // §15 step 4c: prove the sender API can produce tone-burst audio AND the
    // receiver's monitor decodes it correctly. Real ACK loopback in miniature.

    ultra::gui::StreamingEncoder enc;
    ultra::gui::StreamingDecoder dec;

    std::mutex mtx;
    std::vector<tba::ToneBurstAckDetection> events;
    dec.setToneBurstAckCallback([&](const tba::ToneBurstAckDetection& d) {
        std::lock_guard<std::mutex> lk(mtx);
        events.push_back(d);
    });

    const auto orig = makePayload();
    const auto tx = enc.encodeToneBurstAck(orig, tba::kBaselineSymbolMs);
    EXPECT(tx.size() > 0);
    // 27 symbols × 1200 samples/symbol = 32400 samples at baseline.
    EXPECT_EQ(tx.size(),
              static_cast<size_t>(tba::kTotalSymbols) *
                  (tba::kSampleRate * tba::kBaselineSymbolMs / 1000u));

    // Feed: silence, then the encoded burst, then more silence (so the
    // monitor's cadence-gated detection definitely triggers after the burst
    // is fully buffered).
    const size_t chunk_samples = 1024;
    std::vector<float> silence(chunk_samples, 0.0f);
    const size_t pad_chunks =
        (static_cast<size_t>(tba::kSampleRate) * 200u / 1000u) / chunk_samples;
    for (size_t i = 0; i < pad_chunks; ++i) {
        dec.feedAudio(silence.data(), silence.size());
    }
    size_t off = 0;
    while (off < tx.size()) {
        const size_t n = std::min(chunk_samples, tx.size() - off);
        dec.feedAudio(tx.data() + off, n);
        off += n;
    }
    const size_t tail_chunks = pad_chunks + 24;
    for (size_t i = 0; i < tail_chunks; ++i) {
        dec.feedAudio(silence.data(), silence.size());
    }

    std::lock_guard<std::mutex> lk(mtx);
    EXPECT_EQ(events.size(), static_cast<size_t>(1));
    if (events.size() >= 1) {
        const auto& d = events[0];
        EXPECT_EQ(d.payload.group_seq, orig.group_seq);
        EXPECT_EQ(d.payload.frame_mask, orig.frame_mask);
        EXPECT_EQ(d.payload.rate_hint, orig.rate_hint);
        EXPECT_EQ(static_cast<int>(d.payload.type), static_cast<int>(orig.type));
        std::printf("  encoded->decoded round-trip OK: peak=%.1f\n",
                    d.correlation_peak);
    }
}

void test_streaming_decoder_pure_silence_does_not_fire_monitor() {
    std::printf("[test] streaming_decoder_pure_silence_does_not_fire_monitor\n");

    ultra::gui::StreamingDecoder dec;
    std::mutex mtx;
    std::vector<tba::ToneBurstAckDetection> events;
    dec.setToneBurstAckCallback([&](const tba::ToneBurstAckDetection& d) {
        std::lock_guard<std::mutex> lk(mtx);
        events.push_back(d);
    });

    // Feed ~3 seconds of pure silence.
    std::vector<float> silence(1024, 0.0f);
    for (int i = 0; i < 150; ++i) {
        dec.feedAudio(silence.data(), silence.size());
    }
    std::lock_guard<std::mutex> lk(mtx);
    EXPECT_EQ(events.size(), static_cast<size_t>(0));
    EXPECT_EQ(dec.toneBurstAcksDetected(), static_cast<uint64_t>(0));
}

}  // namespace

int main() {
    test_streaming_decoder_fires_tone_burst_callback_on_baseline_burst();
    test_streaming_encoder_to_decoder_loopback();
    test_streaming_decoder_pure_silence_does_not_fire_monitor();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d test assertion(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll StreamingDecoder tone-burst monitor integration tests PASSED\n");
    return 0;
}
