#pragma once

#include <cstddef>
#include <vector>

namespace ultra {
namespace fec {

constexpr size_t kCarrierLdpcMultiplier = 307;
constexpr size_t kLdpcCodewordBits = 648;

struct AirGridIndex {
    size_t symbol;
    size_t carrier;
    size_t bit_lane;
};

// Forward (TX) permutation:
//   interleaved_index[i] = (307 * i) mod (648 * Ncw)
// Returns the air-grid index for each original coded-bit position.
std::vector<size_t> buildCarrierInterleaverV1(size_t Ncw);

// Inverse (RX) permutation, length 648 * Ncw.
std::vector<size_t> buildCarrierDeinterleaverV1(size_t Ncw);

AirGridIndex decomposeAirIndex(size_t a, size_t carriers, size_t bits_per_carrier);

}  // namespace fec
}  // namespace ultra
