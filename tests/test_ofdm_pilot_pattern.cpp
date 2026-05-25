#include "../src/ofdm/pilot_pattern.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ultra;

namespace {

int tests_passed = 0;
int tests_failed = 0;

#define TEST(name) \
    std::cout << "Testing " << name << "... "; \
    try

#define PASS() \
    std::cout << "PASS\n"; \
    ++tests_passed

#define FAIL(msg) \
    std::cout << "FAIL: " << msg << "\n"; \
    ++tests_failed

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ModemConfig makeQam16Config() {
    ModemConfig cfg;
    cfg.modulation = Modulation::QAM16;
    cfg.code_rate = CodeRate::R1_4;
    cfg.num_carriers = 59;
    cfg.fft_size = 1024;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 5;
    cfg.scattered_pilots = true;
    return cfg;
}

void build(const ModemConfig& cfg,
           size_t symbol_index,
           std::vector<int>& all_fft,
           std::vector<int>& data_fft,
           std::vector<int>& pilot_fft,
           std::vector<size_t>& data_logical,
           std::vector<size_t>& pilot_logical,
           std::vector<bool>& is_pilot,
           std::vector<Complex>& pilot_sequence) {
    ofdm_pilots::buildCarrierPattern(cfg,
                                     symbol_index,
                                     all_fft,
                                     data_fft,
                                     pilot_fft,
                                     data_logical,
                                     pilot_logical,
                                     is_pilot,
                                     pilot_sequence);
}

void test_scattered_qam16_counts_and_union() {
    TEST("QAM16 scattered pilots keep constant overhead and cover the band") {
        const ModemConfig cfg = makeQam16Config();
        const size_t expected_pilots = ofdm_pilots::pilotCount(cfg);
        const size_t expected_data = cfg.num_carriers - expected_pilots;
        std::set<size_t> union_logical;

        for (size_t s = 0; s < ofdm_pilots::patternPeriod(cfg); ++s) {
            std::vector<int> all_fft, data_fft, pilot_fft;
            std::vector<size_t> data_logical, pilot_logical;
            std::vector<bool> is_pilot;
            std::vector<Complex> pilot_sequence;
            build(cfg, s, all_fft, data_fft, pilot_fft,
                  data_logical, pilot_logical, is_pilot, pilot_sequence);

            require(all_fft.size() == cfg.num_carriers, "all-carrier count changed");
            require(pilot_fft.size() == expected_pilots, "pilot count changed");
            require(data_fft.size() == expected_data, "data count changed");
            require(pilot_sequence.size() == pilot_fft.size(),
                    "pilot sequence count mismatch");

            std::set<size_t> symbol_pilots(pilot_logical.begin(),
                                           pilot_logical.end());
            require(symbol_pilots.size() == pilot_logical.size(),
                    "duplicate pilot in one symbol");
            union_logical.insert(pilot_logical.begin(), pilot_logical.end());
        }

        require(union_logical.size() == cfg.num_carriers,
                "scattered cycle did not sample every logical carrier");
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_pilot_symbol_is_logical_carrier_stable() {
    TEST("scattered pilot value follows logical carrier, not pilot ordinal") {
        const ModemConfig cfg = makeQam16Config();

        for (size_t logical = 0; logical < cfg.num_carriers; ++logical) {
            bool seen = false;
            Complex first(0, 0);
            for (size_t s = 0; s < ofdm_pilots::patternPeriod(cfg); ++s) {
                std::vector<int> all_fft, data_fft, pilot_fft;
                std::vector<size_t> data_logical, pilot_logical;
                std::vector<bool> is_pilot;
                std::vector<Complex> pilot_sequence;
                build(cfg, s, all_fft, data_fft, pilot_fft,
                      data_logical, pilot_logical, is_pilot, pilot_sequence);

                for (size_t i = 0; i < pilot_logical.size(); ++i) {
                    if (pilot_logical[i] != logical) {
                        continue;
                    }
                    if (!seen) {
                        first = pilot_sequence[i];
                        seen = true;
                    } else {
                        require(pilot_sequence[i] == first,
                                "pilot symbol changed for one logical carrier");
                    }
                }
            }
            require(seen, "logical carrier never appeared as a pilot");
        }
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_differential_modes_keep_fixed_comb() {
    TEST("differential modes keep the legacy fixed pilot comb") {
        ModemConfig cfg = makeQam16Config();
        cfg.modulation = Modulation::DQPSK;
        cfg.code_rate = CodeRate::R1_2;
        cfg.pilot_spacing = 10;
        cfg.scattered_pilots = true;

        require(!ofdm_pilots::scatteredPilotsActive(cfg),
                "DQPSK should not activate scattered pilots");
        require(ofdm_pilots::patternPeriod(cfg) == 1,
                "DQPSK pattern period should stay fixed");

        std::vector<int> all_fft0, data_fft0, pilot_fft0;
        std::vector<size_t> data_logical0, pilot_logical0;
        std::vector<bool> is_pilot0;
        std::vector<Complex> pilot_sequence0;
        build(cfg, 0, all_fft0, data_fft0, pilot_fft0,
              data_logical0, pilot_logical0, is_pilot0, pilot_sequence0);

        std::vector<int> all_fft1, data_fft1, pilot_fft1;
        std::vector<size_t> data_logical1, pilot_logical1;
        std::vector<bool> is_pilot1;
        std::vector<Complex> pilot_sequence1;
        build(cfg, 7, all_fft1, data_fft1, pilot_fft1,
              data_logical1, pilot_logical1, is_pilot1, pilot_sequence1);

        require(pilot_logical0 == pilot_logical1,
                "fixed comb changed with symbol index");
        require(pilot_logical0 == std::vector<size_t>({0, 10, 20, 30, 40, 50}),
                "legacy DQPSK pilot positions changed");
        require(pilot_sequence0 == pilot_sequence1,
                "legacy DQPSK pilot sequence changed");
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

}  // namespace

int main() {
    test_scattered_qam16_counts_and_union();
    test_pilot_symbol_is_logical_carrier_stable();
    test_differential_modes_keep_fixed_comb();

    std::cout << "Passed: " << tests_passed << "\n";
    std::cout << "Failed: " << tests_failed << "\n";
    return tests_failed == 0 ? 0 : 1;
}
