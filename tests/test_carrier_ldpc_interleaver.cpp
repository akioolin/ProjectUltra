#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/fec/carrier_ldpc_interleaver.hpp"
#include "../src/fec/ldpc_802_11n.hpp"

using namespace ultra::fec;

namespace {

constexpr size_t kSupportedCarriers = 59;
constexpr std::array<size_t, 3> kBitsPerCarrierCases = {1, 2, 3};
constexpr std::array<size_t, 4> kDqpskCodewordCases = {1, 2, 4, 8};
constexpr size_t kLdpcCirculantSize = ultra::ldpc_802_11n::Z;
constexpr size_t kLdpcBaseColumns = ultra::ldpc_802_11n::NB;
constexpr size_t kDesignMinColumnsPerSingleCarrier = 8;

static_assert(kCarrierLdpcMultiplier == 307);
static_assert(kLdpcCodewordBits == ultra::ldpc_802_11n::N);
static_assert(kLdpcCirculantSize == 27);
static_assert(kLdpcBaseColumns == 24);

struct RateColumnExpectation {
    const char* rate_name;
    size_t base_columns;
    size_t design_min_single_carrier_columns;
};

constexpr std::array<RateColumnExpectation, 4> kRateColumnExpectations = {{
    {"R1/4 project QC-LDPC n=648 Z=27", 24, 8},
    {"R1/2 IEEE 802.11n n=648 Z=27", 24, 8},
    {"R2/3 IEEE 802.11n n=648 Z=27", 24, 8},
    {"R3/4 IEEE 802.11n n=648 Z=27", 24, 8},
}};

int tests_passed = 0;
int tests_failed = 0;

#define TEST(name) \
    std::cout << "Testing " << name << "... "; \
    try

#define PASS() \
    std::cout << "PASS\n"; \
    tests_passed++;

#define FAIL(msg) \
    std::cout << "FAIL: " << msg << "\n"; \
    tests_failed++;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string profileName(size_t Ncw, size_t bits_per_carrier) {
    return "Ncw=" + std::to_string(Ncw) +
           " Q=" + std::to_string(bits_per_carrier);
}

std::vector<size_t> baseColumnsForCarrier(const std::vector<size_t>& interleaver,
                                          size_t bits_per_carrier,
                                          size_t erased_carrier) {
    std::vector<bool> touched(kLdpcBaseColumns, false);

    for (size_t i = 0; i < interleaver.size(); ++i) {
        const AirGridIndex grid = decomposeAirIndex(interleaver[i],
                                                    kSupportedCarriers,
                                                    bits_per_carrier);
        if (grid.carrier != erased_carrier) {
            continue;
        }

        const size_t ldpc_bit = i % kLdpcCodewordBits;
        const size_t base_column = ldpc_bit / kLdpcCirculantSize;
        require(base_column < touched.size(), "base column out of range");
        touched[base_column] = true;
    }

    std::vector<size_t> columns;
    for (size_t col = 0; col < touched.size(); ++col) {
        if (touched[col]) {
            columns.push_back(col);
        }
    }
    return columns;
}

size_t expectedMinimumSingleCarrierColumns(size_t Ncw) {
    switch (Ncw) {
        case 1: return 5;   // 307 counterexample: carrier 32 touches {6,8,17,19,21}.
        case 2: return 14;
        case 4: return 22;
        case 8: return 24;
        default: throw std::runtime_error("unexpected Ncw for DQPSK spread test");
    }
}

void test_gcd_for_supported_codeword_counts() {
    TEST("307 is coprime to 648*Ncw for Ncw=1..8") {
        for (size_t Ncw = 1; Ncw <= 8; ++Ncw) {
            const size_t total_bits = kLdpcCodewordBits * Ncw;
            require(std::gcd(kCarrierLdpcMultiplier, total_bits) == 1,
                    "gcd failed for Ncw=" + std::to_string(Ncw));
        }

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_air_index_decomposition() {
    TEST("air-grid decomposition for 59 carriers and Q=1,2,3") {
        for (size_t bits_per_carrier : kBitsPerCarrierCases) {
            const size_t bits_per_symbol = kSupportedCarriers * bits_per_carrier;

            AirGridIndex first = decomposeAirIndex(0, kSupportedCarriers, bits_per_carrier);
            require(first.symbol == 0 && first.carrier == 0 && first.bit_lane == 0,
                    "first index decomposition failed");

            AirGridIndex last = decomposeAirIndex(bits_per_symbol - 1,
                                                  kSupportedCarriers,
                                                  bits_per_carrier);
            require(last.symbol == 0 &&
                    last.carrier == kSupportedCarriers - 1 &&
                    last.bit_lane == bits_per_carrier - 1,
                    "last index in symbol decomposition failed");

            AirGridIndex next = decomposeAirIndex(bits_per_symbol,
                                                  kSupportedCarriers,
                                                  bits_per_carrier);
            require(next.symbol == 1 && next.carrier == 0 && next.bit_lane == 0,
                    "next symbol decomposition failed");
        }

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_bijection_for_all_supported_profiles() {
    TEST("forward and inverse compose to identity for all supported profiles") {
        for (size_t Ncw = 1; Ncw <= 8; ++Ncw) {
            const std::vector<size_t> interleaver = buildCarrierInterleaverV1(Ncw);
            const std::vector<size_t> deinterleaver = buildCarrierDeinterleaverV1(Ncw);
            const size_t total_bits = kLdpcCodewordBits * Ncw;

            require(interleaver.size() == total_bits, "interleaver size mismatch");
            require(deinterleaver.size() == total_bits, "deinterleaver size mismatch");

            for (size_t bits_per_carrier : kBitsPerCarrierCases) {
                std::vector<bool> seen(total_bits, false);
                for (size_t i = 0; i < total_bits; ++i) {
                    const size_t air_index = interleaver[i];
                    require(air_index < total_bits, "air index out of range for " +
                            profileName(Ncw, bits_per_carrier));
                    require(!seen[air_index], "duplicate air index for " +
                            profileName(Ncw, bits_per_carrier));
                    seen[air_index] = true;

                    require(deinterleaver[air_index] == i,
                            "RX(TX(i)) identity failed for " +
                            profileName(Ncw, bits_per_carrier));
                    require(interleaver[deinterleaver[air_index]] == air_index,
                            "TX(RX(a)) identity failed for " +
                            profileName(Ncw, bits_per_carrier));

                    const AirGridIndex grid = decomposeAirIndex(air_index,
                                                                kSupportedCarriers,
                                                                bits_per_carrier);
                    require(grid.carrier < kSupportedCarriers,
                            "carrier decomposition out of range");
                    require(grid.bit_lane < bits_per_carrier,
                            "bit-lane decomposition out of range");
                }
            }
        }

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_dqpsk_contiguous_carrier_masks_touch_all_codewords() {
    TEST("DQPSK contiguous masks of 1..8 carriers touch all codewords") {
        constexpr size_t bits_per_carrier = 2;

        for (size_t Ncw : kDqpskCodewordCases) {
            const std::vector<size_t> interleaver = buildCarrierInterleaverV1(Ncw);
            for (size_t block_len = 1; block_len <= 8; ++block_len) {
                for (size_t start = 0; start + block_len <= kSupportedCarriers; ++start) {
                    std::vector<bool> touched_codewords(Ncw, false);
                    size_t touched_count = 0;

                    for (size_t i = 0; i < interleaver.size(); ++i) {
                        const AirGridIndex grid = decomposeAirIndex(interleaver[i],
                                                                    kSupportedCarriers,
                                                                    bits_per_carrier);
                        if (grid.carrier < start || grid.carrier >= start + block_len) {
                            continue;
                        }

                        const size_t cw = i / kLdpcCodewordBits;
                        if (!touched_codewords[cw]) {
                            touched_codewords[cw] = true;
                            ++touched_count;
                        }
                    }

                    require(touched_count == Ncw,
                            "mask start=" + std::to_string(start) +
                            " len=" + std::to_string(block_len) +
                            " touched " + std::to_string(touched_count) +
                            " of " + std::to_string(Ncw) + " codewords");
                }
            }
        }

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_dqpsk_single_carrier_base_column_spread() {
    TEST("DQPSK single-carrier erasures spread across LDPC base columns") {
        constexpr size_t bits_per_carrier = 2;

        for (const RateColumnExpectation& expectation : kRateColumnExpectations) {
            require(expectation.base_columns == kLdpcBaseColumns,
                    std::string(expectation.rate_name) + " base-column count mismatch");
            require(expectation.design_min_single_carrier_columns ==
                        kDesignMinColumnsPerSingleCarrier,
                    std::string(expectation.rate_name) + " design column floor mismatch");
        }

        for (size_t Ncw : kDqpskCodewordCases) {
            const std::vector<size_t> interleaver = buildCarrierInterleaverV1(Ncw);
            const size_t expected_minimum = expectedMinimumSingleCarrierColumns(Ncw);
            size_t observed_minimum = kLdpcBaseColumns;

            for (size_t carrier = 0; carrier < kSupportedCarriers; ++carrier) {
                const std::vector<size_t> columns =
                    baseColumnsForCarrier(interleaver, bits_per_carrier, carrier);
                observed_minimum = std::min(observed_minimum, columns.size());

                if (Ncw >= 2) {
                    require(columns.size() >= kDesignMinColumnsPerSingleCarrier,
                            "Ncw=" + std::to_string(Ncw) +
                            " carrier=" + std::to_string(carrier) +
                            " touches fewer than 8 base columns");
                }
            }

            require(observed_minimum == expected_minimum,
                    "unexpected minimum base-column spread for Ncw=" +
                    std::to_string(Ncw) + ": got " +
                    std::to_string(observed_minimum) + ", expected " +
                    std::to_string(expected_minimum));
        }

        const std::vector<size_t> ncw1_interleaver = buildCarrierInterleaverV1(1);
        const std::vector<size_t> carrier32_columns =
            baseColumnsForCarrier(ncw1_interleaver, bits_per_carrier, 32);
        const std::vector<size_t> ncw1_counterexample = {6, 8, 17, 19, 21};
        require(carrier32_columns == ncw1_counterexample,
                "Ncw=1 carrier 32 counterexample changed");

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_ncw8_build_time_under_5ms() {
    TEST("Ncw=8 permutation builds under 5 ms") {
        const auto start = std::chrono::steady_clock::now();
        const std::vector<size_t> interleaver = buildCarrierInterleaverV1(8);
        const auto end = std::chrono::steady_clock::now();
        const std::chrono::duration<double, std::milli> elapsed = end - start;

        require(interleaver.size() == kLdpcCodewordBits * 8,
                "Ncw=8 interleaver size mismatch");
        require(elapsed.count() < 5.0,
                "Ncw=8 build took " + std::to_string(elapsed.count()) + " ms");

        std::cout << "(" << elapsed.count() << " ms) ";
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

}  // namespace

int main() {
    std::cout << "=== CarrierLDPC v1 Interleaver Tests ===\n\n";

    test_gcd_for_supported_codeword_counts();
    test_air_index_decomposition();
    test_bijection_for_all_supported_profiles();
    test_dqpsk_contiguous_carrier_masks_touch_all_codewords();
    test_dqpsk_single_carrier_base_column_spread();
    test_ncw8_build_time_under_5ms();

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << tests_passed << "\n";
    std::cout << "Failed: " << tests_failed << "\n";

    return tests_failed > 0 ? 1 : 0;
}
