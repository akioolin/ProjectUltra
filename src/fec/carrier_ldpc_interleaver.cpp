#include "carrier_ldpc_interleaver.hpp"

#include <numeric>
#include <stdexcept>

namespace ultra {
namespace fec {

namespace {

constexpr size_t kMinCarrierLdpcCodewords = 1;
constexpr size_t kMaxCarrierLdpcCodewords = 8;

size_t carrierLdpcBitCount(size_t Ncw) {
    if (Ncw < kMinCarrierLdpcCodewords || Ncw > kMaxCarrierLdpcCodewords) {
        throw std::invalid_argument("CarrierLDPC v1 supports Ncw in 1..8");
    }
    return kLdpcCodewordBits * Ncw;
}

}  // namespace

std::vector<size_t> buildCarrierInterleaverV1(size_t Ncw) {
    const size_t total_bits = carrierLdpcBitCount(Ncw);
    if (std::gcd(kCarrierLdpcMultiplier, total_bits) != 1) {
        throw std::logic_error("CarrierLDPC v1 multiplier is not coprime to frame length");
    }

    std::vector<size_t> interleaver(total_bits);
    for (size_t i = 0; i < total_bits; ++i) {
        interleaver[i] = (kCarrierLdpcMultiplier * i) % total_bits;
    }
    return interleaver;
}

std::vector<size_t> buildCarrierDeinterleaverV1(size_t Ncw) {
    const auto interleaver = buildCarrierInterleaverV1(Ncw);
    std::vector<size_t> deinterleaver(interleaver.size());

    for (size_t i = 0; i < interleaver.size(); ++i) {
        deinterleaver[interleaver[i]] = i;
    }
    return deinterleaver;
}

AirGridIndex decomposeAirIndex(size_t a, size_t carriers, size_t bits_per_carrier) {
    if (carriers == 0 || bits_per_carrier == 0) {
        throw std::invalid_argument("CarrierLDPC air-grid dimensions must be non-zero");
    }

    const size_t bits_per_symbol = carriers * bits_per_carrier;
    return AirGridIndex{
        a / bits_per_symbol,
        (a % bits_per_symbol) / bits_per_carrier,
        a % bits_per_carrier,
    };
}

}  // namespace fec
}  // namespace ultra
