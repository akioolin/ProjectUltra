#include "ultra/fec.hpp"
#include "ldpc_802_11n.hpp"
#include <stdexcept>
#include <bitset>
#include <random>
#include <algorithm>

namespace ultra {

/**
 * LDPC Encoder
 *
 * For R1/2, R2/3, R3/4, R5/6: uses IEEE 802.11n standard base matrices (Z=27, n=648).
 * These have optimized cycle structure (girth >= 6) for excellent BP decoding.
 *
 * For R1/4: uses a random PRNG-based construction (no 802.11n matrix exists for this rate).
 * R1/4 has enough redundancy that the random code works adequately.
 *
 * Code structure: H = [A | B], systematic encoding: parity = B^{-1} * A * info (mod 2)
 * For standard rates, B^{-1}*A is precomputed at construction via Gaussian elimination.
 * For R1/4 (H = [H_data | I]), parity = H_data * info directly.
 */

namespace {

constexpr int SUBBLOCK_SIZE = 27;
constexpr int BLOCK_COLS = 24;
constexpr int BLOCK_LENGTH = SUBBLOCK_SIZE * BLOCK_COLS;

struct CodeParams {
    int info_bits;
    int parity_bits;
    int num_check_rows;
};

// Dimensions in units of the lifting size Z (block counts of the 24-column
// base matrix). info_blocks + parity_blocks = 24 for every rate. Multiplying
// by Z gives the bit counts: Z=27 -> n=648 (default), Z=81 -> n=1944.
CodeParams getCodeParams(CodeRate rate, int Z) {
    int info_blk, par_blk;
    switch (rate) {
        case CodeRate::R1_4: info_blk = 6;  par_blk = 18; break;  // {162,486} @Z27
        case CodeRate::R1_2: info_blk = 12; par_blk = 12; break;  // {324,324} @Z27
        case CodeRate::R2_3: info_blk = 16; par_blk = 8;  break;  // {432,216} @Z27
        case CodeRate::R3_4: info_blk = 18; par_blk = 6;  break;  // {486,162} @Z27
        case CodeRate::R5_6: info_blk = 20; par_blk = 4;  break;  // {540,108} @Z27
        default:             info_blk = 12; par_blk = 12; break;
    }
    const int k = info_blk * Z;
    const int m = par_blk * Z;
    return {k, m, m};
}

} // anonymous namespace

struct LDPCEncoder::Impl {
    CodeRate rate;
    int lifting_size;
    CodeParams params;

    // Encoding matrix: for each parity bit i, encoding_rows[i] lists which
    // info bit indices to XOR together to produce parity bit i.
    //
    // For 802.11n codes: derived from B^{-1} * A via Gaussian elimination
    // For random codes (R1/4): same as H_data rows (since H = [H_data | I])
    std::vector<std::vector<int>> encoding_rows;

    Impl(CodeRate r, int Z) : rate(r), lifting_size(Z), params(getCodeParams(r, Z)) {
        buildMatrix();
    }

    void buildMatrix() {
        int k = params.info_bits;
        int m = params.parity_bits;

        // Try 802.11n standard matrix first
        const int* base_data = ldpc_802_11n::getBaseData(rate);
        if (base_data) {
            // Use IEEE 802.11n standard code
            auto expanded = ldpc_802_11n::expand(rate, lifting_size);
            encoding_rows = std::move(expanded.enc_rows);
            return;
        }

        // Fallback: random PRNG-based construction for R1/4
        // This produces H = [H_data | I] where encoding_rows = H_data rows
        encoding_rows.clear();
        encoding_rows.resize(m);

        std::mt19937 rng(0x12345678 + static_cast<int>(rate));

        int target_check_degree = 4;
        int target_var_degree = std::max(3, (target_check_degree * m) / k);
        target_var_degree = std::min(target_var_degree, m / 2);

        std::vector<int> check_degrees(m, 0);
        int max_check_degree = target_check_degree + 2;

        for (int j = 0; j < k; ++j) {
            std::vector<int> available_checks;
            for (int i = 0; i < m; ++i) {
                if (check_degrees[i] < max_check_degree) {
                    available_checks.push_back(i);
                }
            }

            // Fisher-Yates shuffle (deterministic across platforms)
            for (size_t i = available_checks.size(); i > 1; --i) {
                size_t j = rng() % i;
                std::swap(available_checks[i - 1], available_checks[j]);
            }
            int connections = std::min(target_var_degree, static_cast<int>(available_checks.size()));

            for (int d = 0; d < connections; ++d) {
                int check = available_checks[d];
                encoding_rows[check].push_back(j);
                check_degrees[check]++;
            }
        }

        // Ensure every check has at least one connection
        for (int i = 0; i < m; ++i) {
            if (encoding_rows[i].empty()) {
                int j = rng() % k;
                encoding_rows[i].push_back(j);
            }
        }
    }
};

LDPCEncoder::LDPCEncoder(CodeRate rate, int lifting_size)
    : impl_(std::make_unique<Impl>(rate, lifting_size)) {}

LDPCEncoder::~LDPCEncoder() = default;

Bytes LDPCEncoder::encode(ByteSpan data) {
    // Handle multi-block encoding for data larger than one LDPC block
    // IMPORTANT: We work at bit-level to avoid losing bits at block boundaries
    // when k is not a multiple of 8 (e.g., k=486 bits = 60.75 bytes)

    int k = impl_->params.info_bits;
    int n = k + impl_->params.parity_bits;

    // Convert entire input to bits
    std::vector<uint8_t> all_bits;
    all_bits.reserve(data.size() * 8);
    for (uint8_t byte : data) {
        for (int b = 7; b >= 0; --b) {
            all_bits.push_back((byte >> b) & 1);
        }
    }

    // Encode k bits at a time
    Bytes output;
    size_t bit_offset = 0;

    while (bit_offset < all_bits.size()) {
        // Extract k info bits for this block (pad with zeros if needed at the end)
        std::vector<uint8_t> info_bits(k, 0);
        for (int j = 0; j < k && bit_offset + j < all_bits.size(); ++j) {
            info_bits[j] = all_bits[bit_offset + j];
        }

        // Calculate parity bits using the encoding matrix
        int m = impl_->params.parity_bits;
        std::vector<uint8_t> parity(m, 0);
        for (int i = 0; i < m; ++i) {
            uint8_t sum = 0;
            for (int j : impl_->encoding_rows[i]) {
                sum ^= info_bits[j];
            }
            parity[i] = sum;
        }

        // Combine info + parity into codeword bits
        std::vector<uint8_t> codeword(n);
        std::copy(info_bits.begin(), info_bits.end(), codeword.begin());
        std::copy(parity.begin(), parity.end(), codeword.begin() + k);

        // Convert codeword bits to bytes
        uint8_t byte = 0;
        int bit_count = 0;
        for (uint8_t bit : codeword) {
            byte = (byte << 1) | bit;
            ++bit_count;
            if (bit_count == 8) {
                output.push_back(byte);
                byte = 0;
                bit_count = 0;
            }
        }
        if (bit_count > 0) {
            output.push_back(byte << (8 - bit_count));
        }

        bit_offset += k;  // Advance by exactly k bits (not k_bytes)
    }

    return output;
}

size_t LDPCEncoder::getCodedSize(size_t input_size) const {
    int k = impl_->params.info_bits;
    int n = k + impl_->params.parity_bits;

    size_t input_bits = input_size * 8;
    size_t num_blocks = (input_bits + k - 1) / k;
    size_t output_bits = num_blocks * n;

    return (output_bits + 7) / 8;
}

CodeRate LDPCEncoder::getRate() const {
    return impl_->rate;
}

void LDPCEncoder::setRate(CodeRate rate) {
    // Skip rebuild if rate hasn't changed. See LDPCDecoder::setRate for
    // why this matters — same hot-path issue on the encoder side.
    if (impl_->rate == rate) return;
    impl_->rate = rate;
    impl_->params = getCodeParams(rate, impl_->lifting_size);
    impl_->buildMatrix();
}

} // namespace ultra
