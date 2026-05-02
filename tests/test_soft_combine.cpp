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

void test_average_across_attempts() {
    SoftCombineBuffer buffer;
    buffer.setEnabled(true);
    const auto k = key(0x010203, 9);

    std::vector<float> out;
    buffer.combine(k, {1.0f, 3.0f, -2.0f}, out);
    buffer.retain(k, out);

    int attempts = buffer.combine(k, {3.0f, 5.0f, 2.0f}, out);
    CHECK(attempts == 2, "second attempt should report two combined attempts");
    CHECK(vecNear(out, {2.0f, 4.0f, 0.0f}), "second attempt should average two vectors");
    buffer.retain(k, out);

    attempts = buffer.combine(k, {5.0f, 1.0f, 3.0f}, out);
    CHECK(attempts == 3, "third attempt should report three combined attempts");
    CHECK(vecNear(out, {3.0f, 3.0f, 1.0f}), "third attempt should average all attempts");
    pass("average across attempts");
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

    buffer.combine(base, {1.0f}, out); buffer.retain(base, out);
    buffer.combine(other_sender, {10.0f}, out); buffer.retain(other_sender, out);
    buffer.combine(other_rate, {20.0f}, out); buffer.retain(other_rate, out);
    buffer.combine(other_cw, {30.0f}, out); buffer.retain(other_cw, out);

    CHECK(buffer.size() == 4, "distinct keys should occupy distinct entries");
    CHECK(buffer.combine(base, {3.0f}, out) == 2 && vecNear(out, {2.0f}),
          "base key should combine only with base entry");
    CHECK(buffer.combine(other_sender, {14.0f}, out) == 2 && vecNear(out, {12.0f}),
          "sender hash should disambiguate same seq");
    CHECK(buffer.combine(other_rate, {24.0f}, out) == 2 && vecNear(out, {22.0f}),
          "code rate should disambiguate same sender/seq");
    CHECK(buffer.combine(other_cw, {34.0f}, out) == 2 && vecNear(out, {32.0f}),
          "cw count should disambiguate same sender/seq/rate");
    pass("key disambiguation");
}

} // namespace

int main() {
    test_disabled_noop();
    test_first_attempt_identity();
    test_average_across_attempts();
    test_drop_on_success();
    test_ttl_eviction();
    test_max_entries_lru_eviction();
    test_key_disambiguation();

    std::cout << "\nSoftCombineBuffer tests: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed == 0 ? 0 : 1;
}
