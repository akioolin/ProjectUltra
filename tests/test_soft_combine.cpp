#include "fec/soft_combine.hpp"

#include <cmath>
#include <iostream>
#include <vector>

using ultra::CodeRate;
using ultra::fec::SoftCombineBuffer;

namespace {

int tests_passed = 0;
int tests_failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cout << "FAIL: " << msg << "\n"; \
            ++tests_failed; \
            return; \
        } \
    } while (0)

bool near(float a, float b, float eps = 1e-6f) {
    return std::fabs(a - b) <= eps;
}

bool vecNear(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!near(a[i], b[i])) return false;
    }
    return true;
}

SoftCombineBuffer::Key key(uint32_t sender, uint16_t seq,
                           CodeRate rate = CodeRate::R1_2,
                           uint8_t cw_count = 6) {
    return SoftCombineBuffer::Key{sender, seq, rate, cw_count};
}

void pass(const char* name) {
    ++tests_passed;
    std::cout << "PASS: " << name << "\n";
}

void test_disabled_noop() {
    SoftCombineBuffer buffer;
    std::vector<float> in{1.0f, -2.0f, 3.5f};
    std::vector<float> out;

    int attempts = buffer.combine(key(0x010203, 7), in, out);
    buffer.retain(key(0x010203, 7), out);

    CHECK(attempts == 1, "disabled combine should report one attempt");
    CHECK(vecNear(out, in), "disabled combine should copy input unchanged");
    CHECK(buffer.size() == 0, "disabled retain should not store entries");
    pass("disabled no-op");
}

void test_first_attempt_identity() {
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    std::vector<float> in{2.0f, -4.0f, 8.0f};
    std::vector<float> out;

    int attempts = buffer.combine(key(0x010203, 8), in, out);
    buffer.retain(key(0x010203, 8), out);

    CHECK(attempts == 1, "first attempt should report one attempt");
    CHECK(vecNear(out, in), "first attempt should be identity");
    CHECK(buffer.size() == 1, "first failed attempt should be retained");
    pass("first attempt identity");
}

void test_sum_across_attempts() {
    // Chase combining stores accumulated LLRs and sums new ones in.
    // Joint LLR for independent observations is the sum, NOT the
    // average — magnitude must grow with attempts so the LDPC decoder
    // gets stronger evidence on each retransmission.
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    const auto k = key(0x010203, 9);

    std::vector<float> out;
    buffer.combine(k, {1.0f, 3.0f, -2.0f}, out);
    buffer.retain(k, out);

    int attempts = buffer.combine(k, {3.0f, 5.0f, 2.0f}, out);
    CHECK(attempts == 2, "second attempt should report two combined attempts");
    CHECK(vecNear(out, {4.0f, 8.0f, 0.0f}), "second attempt should sum two vectors");
    buffer.retain(k, out);

    attempts = buffer.combine(k, {5.0f, 1.0f, 3.0f}, out);
    CHECK(attempts == 3, "third attempt should report three combined attempts");
    CHECK(vecNear(out, {9.0f, 9.0f, 3.0f}), "third attempt should sum all three vectors");
    pass("sum across attempts");
}

void test_llr_magnitude_grows_with_attempts() {
    // The point of chase combining: |LLR| should grow with each
    // independent observation so the LDPC decoder sees stronger
    // confidence. If combining ever stops increasing magnitude
    // (e.g. averaging), HARQ is disabled in practice.
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    const auto k = key(0x010203, 31);
    std::vector<float> out;
    constexpr float kSingle = 4.0f;

    buffer.combine(k, {kSingle, -kSingle}, out);
    buffer.retain(k, out);

    for (int i = 2; i <= 5; ++i) {
        const int attempts = buffer.combine(k, {kSingle, -kSingle}, out);
        CHECK(attempts == i, "attempt count should advance");
        const float expected = static_cast<float>(i) * kSingle;
        CHECK(vecNear(out, {expected, -expected}),
              "magnitude must grow linearly with independent attempts");
        buffer.retain(k, out);
    }
    pass("llr magnitude grows with attempts");
}

void test_llr_saturation() {
    // Many retransmissions of a strong observation must not produce
    // unbounded LLR magnitudes that destabilize the LDPC decoder's
    // float math. The implementation caps |LLR| at 60.0 (see
    // src/fec/soft_combine.cpp's kMaxAccumulatedLLR).
    constexpr float kImplementationCap = 60.0f;
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    const auto k = key(0x010203, 32);
    std::vector<float> out;

    // Positive saturation: 50 attempts of |10| → uncapped sum=500,
    // capped at 60. Sign preserved.
    for (int i = 0; i < 50; ++i) {
        buffer.combine(k, {10.0f}, out);
        buffer.retain(k, out);
    }
    int attempts = buffer.combine(k, {10.0f}, out);
    CHECK(attempts == 51, "attempt count keeps advancing");
    CHECK(out[0] == kImplementationCap,
          "+ saturation pins to implementation cap exactly");

    // Negative saturation symmetric: same magnitude, opposite sign.
    const auto k2 = key(0x010203, 33);
    std::vector<float> out2;
    for (int i = 0; i < 50; ++i) {
        buffer.combine(k2, {-10.0f}, out2);
        buffer.retain(k2, out2);
    }
    buffer.combine(k2, {-10.0f}, out2);
    CHECK(out2[0] == -kImplementationCap,
          "- saturation pins to negative implementation cap");
    pass("llr saturation");
}

void test_drop_on_success() {
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    const auto k = key(0x010203, 10);
    std::vector<float> out;

    buffer.combine(k, {4.0f, 8.0f}, out);
    buffer.retain(k, out);
    buffer.drop(k);

    int attempts = buffer.combine(k, {10.0f, 12.0f}, out);
    CHECK(buffer.size() == 0, "drop should remove retained entry");
    CHECK(attempts == 1, "combine after drop should start fresh");
    CHECK(vecNear(out, {10.0f, 12.0f}), "combine after drop should be identity");
    pass("drop on success");
}

void test_ttl_eviction() {
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    buffer.setTTL(100);
    std::vector<float> out;

    buffer.combine(key(0x010203, 11), {1.0f}, out);
    buffer.retain(key(0x010203, 11), out);
    buffer.tick(99);
    CHECK(buffer.size() == 1, "entry should remain before TTL");
    buffer.tick(2);
    CHECK(buffer.size() == 0, "entry should evict after TTL");
    pass("TTL eviction");
}

void test_max_entries_lru_eviction() {
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    buffer.setMaxEntries(3);
    std::vector<float> out;

    const auto k1 = key(0x010203, 1);
    const auto k2 = key(0x010203, 2);
    const auto k3 = key(0x010203, 3);
    const auto k4 = key(0x010203, 4);

    buffer.combine(k1, {1.0f}, out); buffer.retain(k1, out);
    buffer.combine(k2, {2.0f}, out); buffer.retain(k2, out);
    buffer.combine(k3, {3.0f}, out); buffer.retain(k3, out);

    CHECK(buffer.combine(k1, {5.0f}, out) == 2, "touching k1 should find retained entry");
    buffer.combine(k4, {4.0f}, out); buffer.retain(k4, out);

    CHECK(buffer.size() == 3, "buffer should stay bounded at max entries");
    CHECK(buffer.combine(k2, {20.0f}, out) == 1, "least-recently-used entry should be evicted");
    CHECK(vecNear(out, {20.0f}), "evicted key should restart with identity");
    CHECK(buffer.combine(k1, {5.0f}, out) == 2, "recently touched entry should survive");
    pass("max entries LRU eviction");
}

void test_key_disambiguation() {
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    std::vector<float> out;

    const auto base = key(0x010203, 42, CodeRate::R1_2, 6);
    const auto other_sender = key(0x0A0B0C, 42, CodeRate::R1_2, 6);
    const auto other_rate = key(0x010203, 42, CodeRate::R3_4, 6);
    const auto other_cw = key(0x010203, 42, CodeRate::R1_2, 8);

    // Use small LLR magnitudes so the test is about key disambiguation,
    // not the saturation cap.
    buffer.combine(base, {1.0f}, out); buffer.retain(base, out);
    buffer.combine(other_sender, {2.0f}, out); buffer.retain(other_sender, out);
    buffer.combine(other_rate, {4.0f}, out); buffer.retain(other_rate, out);
    buffer.combine(other_cw, {8.0f}, out); buffer.retain(other_cw, out);

    CHECK(buffer.size() == 4, "distinct keys should occupy distinct entries");
    CHECK(buffer.combine(base, {3.0f}, out) == 2 && vecNear(out, {4.0f}),
          "base key should combine only with base entry (sum: 1+3)");
    CHECK(buffer.combine(other_sender, {3.0f}, out) == 2 && vecNear(out, {5.0f}),
          "sender hash should disambiguate same seq (sum: 2+3)");
    CHECK(buffer.combine(other_rate, {3.0f}, out) == 2 && vecNear(out, {7.0f}),
          "code rate should disambiguate same sender/seq (sum: 4+3)");
    CHECK(buffer.combine(other_cw, {3.0f}, out) == 2 && vecNear(out, {11.0f}),
          "cw count should disambiguate same sender/seq/rate (sum: 8+3)");
    pass("key disambiguation");
}

} // namespace

void test_phy_field_disambiguation() {
    // PHY parameters that change LLR scaling/ordering must produce
    // distinct keys. Otherwise stored LLRs from one PHY config could
    // be summed with new LLRs from another, corrupting the decode.
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    std::vector<float> out;

    SoftCombineBuffer::Key base;
    base.sender_hash = 0x010203;
    base.seq = 99;
    base.rate = CodeRate::R1_2;
    base.cw_count = 4;
    base.modulation = ultra::Modulation::DQPSK;
    base.channel_interleave = 0;
    base.carrier_count_hash = 0xAA;

    auto with_mod = base; with_mod.modulation = ultra::Modulation::QAM16;
    auto with_il = base;  with_il.channel_interleave = 1;
    auto with_cc = base;  with_cc.carrier_count_hash = 0xBB;

    buffer.combine(base, {2.0f}, out); buffer.retain(base, out);
    buffer.combine(with_mod, {3.0f}, out); buffer.retain(with_mod, out);
    buffer.combine(with_il,  {4.0f}, out); buffer.retain(with_il, out);
    buffer.combine(with_cc,  {5.0f}, out); buffer.retain(with_cc, out);

    CHECK(buffer.size() == 4, "modulation/interleave/carrier_count must each disambiguate");
    CHECK(buffer.combine(base, {1.0f}, out) == 2 && vecNear(out, {3.0f}),
          "base entry stays isolated from PHY-variant siblings");
    CHECK(buffer.combine(with_mod, {1.0f}, out) == 2 && vecNear(out, {4.0f}),
          "different modulation must not cross-pollinate");
    CHECK(buffer.combine(with_il,  {1.0f}, out) == 2 && vecNear(out, {5.0f}),
          "different channel_interleave must not cross-pollinate");
    CHECK(buffer.combine(with_cc,  {1.0f}, out) == 2 && vecNear(out, {6.0f}),
          "different carrier_count_hash must not cross-pollinate");
    pass("phy field disambiguation");
}

void test_size_mismatch_drops_entry() {
    // If a retransmission's LLR vector is a different size from the
    // retained entry, combining is impossible (different code rates,
    // different cw_count). The buffer should silently drop the stale
    // entry and treat the new attempt as the first.
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    const auto k = key(0x010203, 50);
    std::vector<float> out;

    buffer.combine(k, {1.0f, 2.0f, 3.0f}, out);
    buffer.retain(k, out);
    CHECK(buffer.size() == 1, "first attempt retained");

    // Same key, different size — must drop and restart fresh
    int attempts = buffer.combine(k, {5.0f}, out);
    CHECK(attempts == 1, "size mismatch should reset attempts to 1");
    CHECK(vecNear(out, {5.0f}), "size mismatch should be identity (no combining)");
    CHECK(buffer.size() == 0, "size-mismatched entry should be dropped");
    pass("size mismatch drops entry");
}

void test_enabled_accessor() {
    // Public accessor; verify it tracks setEnabled state.
    SoftCombineBuffer buffer;
    CHECK(!buffer.enabled(), "default state should be disabled");
    buffer.setEnabled(true);
    CHECK(buffer.enabled(), "after setEnabled(true)");
    buffer.setEnabled(false);
    CHECK(!buffer.enabled(), "after setEnabled(false)");
    pass("enabled accessor");
}

void test_setEnabled_false_clears_entries() {
    // Disabling the buffer must drop all retained entries — leaving
    // them around would leak across sessions if HARQ is later
    // re-enabled with a different connection.
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    std::vector<float> out;
    buffer.combine(key(0x010203, 1), {1.0f}, out); buffer.retain(key(0x010203, 1), out);
    buffer.combine(key(0x010203, 2), {2.0f}, out); buffer.retain(key(0x010203, 2), out);
    CHECK(buffer.size() == 2, "two entries before disable");
    buffer.setEnabled(false);
    CHECK(buffer.size() == 0, "setEnabled(false) must clear all entries");
    pass("setEnabled(false) clears entries");
}

void test_zero_sender_hash_is_noop() {
    // sender_hash=0 is the sentinel for "no peer identity yet"
    // (e.g. before CONNECT settles). Storing under that key would
    // pollute future sessions; combine should treat it as never-
    // retained, retain should refuse to store.
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    std::vector<float> out;
    SoftCombineBuffer::Key zero_key;
    zero_key.sender_hash = 0;
    zero_key.seq = 1;
    int attempts = buffer.combine(zero_key, {1.0f}, out);
    CHECK(attempts == 1, "zero sender_hash must always report fresh");
    buffer.retain(zero_key, {1.0f});
    CHECK(buffer.size() == 0, "zero sender_hash must not be retained");
    pass("zero sender_hash is no-op");
}

void test_max_entries_zero_disables_retention() {
    // setMaxEntries(0) is operator-style "disable retention without
    // disabling the buffer entirely". Must clear and prevent re-add.
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    std::vector<float> out;
    buffer.combine(key(0x010203, 5), {3.0f}, out);
    buffer.retain(key(0x010203, 5), out);
    CHECK(buffer.size() == 1, "retained while max>0");
    buffer.setMaxEntries(0);
    CHECK(buffer.size() == 0, "setMaxEntries(0) must clear");
    buffer.combine(key(0x010203, 6), {4.0f}, out);
    buffer.retain(key(0x010203, 6), out);
    CHECK(buffer.size() == 0, "setMaxEntries(0) must prevent retention");
    pass("max entries zero disables retention");
}

void test_empty_llrs_retain_is_noop() {
    // retain() with empty LLRs would create a useless entry that
    // could cause size-mismatch errors later.
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    buffer.retain(key(0x010203, 7), {});
    CHECK(buffer.size() == 0, "empty retain must not store anything");
    pass("empty retain is no-op");
}

int main() {
    test_disabled_noop();
    test_first_attempt_identity();
    test_sum_across_attempts();
    test_llr_magnitude_grows_with_attempts();
    test_llr_saturation();
    test_size_mismatch_drops_entry();
    test_enabled_accessor();
    test_setEnabled_false_clears_entries();
    test_zero_sender_hash_is_noop();
    test_max_entries_zero_disables_retention();
    test_empty_llrs_retain_is_noop();
    test_drop_on_success();
    test_ttl_eviction();
    test_max_entries_lru_eviction();
    test_key_disambiguation();
    test_phy_field_disambiguation();

    std::cout << "\nSoftCombineBuffer tests: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed == 0 ? 0 : 1;
}
