#include "ultra/fec.hpp"
#include "ldpc_802_11n.hpp"
#include <cmath>
#include <algorithm>
#include <limits>
#include <random>

namespace ultra {

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

struct LDPCDecoder::Impl {
    CodeRate rate;
    int lifting_size;
    CodeParams params;
    int max_iterations = 50;
    float min_sum_factor = 0.75f;
    bool last_success = false;
    int last_iters = 0;
    int last_unsatisfied_checks = -1;

    // Full H matrix representation
    // H_rows[i] contains all variable node indices connected to check i
    std::vector<std::vector<int>> H_rows;

    // Column view: for each variable node, which checks it connects to
    std::vector<std::vector<int>> H_cols;

    // Message passing buffers
    std::vector<std::vector<float>> var_to_check;
    std::vector<std::vector<float>> check_to_var;
    std::vector<float> llr_in;
    std::vector<float> llr_total;

    Impl(CodeRate r, int Z) : rate(r), lifting_size(Z), params(getCodeParams(r, Z)) {
        buildMatrix();
    }

    void updateCheckToVar(int m) {
        for (int i = 0; i < m; ++i) {
            const auto& row = H_rows[i];
            const size_t degree = row.size();
            if (degree <= 1) {
                std::fill(check_to_var[i].begin(), check_to_var[i].end(), 0.0f);
                continue;
            }

            float sign_product = 1.0f;
            float min1 = std::numeric_limits<float>::max();
            float min2 = std::numeric_limits<float>::max();
            size_t min1_index = 0;

            for (size_t e = 0; e < degree; ++e) {
                float msg = var_to_check[i][e];
                if (msg < 0.0f) {
                    sign_product = -sign_product;
                }

                float abs_msg = std::abs(msg);
                if (abs_msg < min1) {
                    min2 = min1;
                    min1 = abs_msg;
                    min1_index = e;
                } else if (abs_msg < min2) {
                    min2 = abs_msg;
                }
            }

            for (size_t e = 0; e < degree; ++e) {
                const float msg = var_to_check[i][e];
                const float sign = sign_product * (msg < 0.0f ? -1.0f : 1.0f);
                const float min_abs = (e == min1_index) ? min2 : min1;
                check_to_var[i][e] = sign * min_abs * min_sum_factor;
            }
        }
    }

    void buildMatrix() {
        int k = params.info_bits;
        int m = params.parity_bits;
        int n = k + m;

        // Try 802.11n standard matrix first
        const int* base_data = ldpc_802_11n::getBaseData(rate);
        if (base_data) {
            // Use IEEE 802.11n standard code
            // The expanded H matrix has optimized cycle structure (girth >= 6)
            // which dramatically reduces false convergence in BP decoding
            auto expanded = ldpc_802_11n::expand(rate, lifting_size);
            H_rows = std::move(expanded.H_rows);
            H_cols = std::move(expanded.H_cols);
        } else {
            // Fallback: random PRNG-based construction for R1/4
            // Produces H = [H_data | I]
            std::mt19937 rng(0x12345678 + static_cast<int>(rate));

            H_rows.clear();
            H_rows.resize(m);
            H_cols.clear();
            H_cols.resize(n);

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
                    H_rows[check].push_back(j);
                    H_cols[j].push_back(check);
                    check_degrees[check]++;
                }
            }

            // Ensure every check has at least one info bit connection
            for (int i = 0; i < m; ++i) {
                if (H_rows[i].empty()) {
                    int j = rng() % k;
                    H_rows[i].push_back(j);
                    H_cols[j].push_back(i);
                }
            }

            // Add identity matrix part: parity bit (k+i) connects to check i
            for (int i = 0; i < m; ++i) {
                int parity_idx = k + i;
                H_rows[i].push_back(parity_idx);
                H_cols[parity_idx].push_back(i);
            }
        }

        // Allocate message buffers (works for both code types)
        var_to_check.resize(m);
        check_to_var.resize(m);
        for (int i = 0; i < m; ++i) {
            var_to_check[i].resize(H_rows[i].size(), 0);
            check_to_var[i].resize(H_rows[i].size(), 0);
        }
    }

    int countUnsatisfiedChecks(const std::vector<uint8_t>& bits) const {
        int unsatisfied = 0;
        for (size_t i = 0; i < H_rows.size(); ++i) {
            uint8_t sum = 0;
            for (int j : H_rows[i]) {
                if (j < static_cast<int>(bits.size())) {
                    sum ^= bits[j];
                }
            }
            if (sum != 0) ++unsatisfied;
        }
        return unsatisfied;
    }

    bool checkParity(const std::vector<uint8_t>& bits) const {
        return countUnsatisfiedChecks(bits) == 0;
    }

    Bytes decodeBP(std::span<const float> llrs) {
        int n = params.info_bits + params.parity_bits;
        int k = params.info_bits;
        int m = params.parity_bits;

        // Initialize channel LLRs
        llr_in.assign(n, 0);
        llr_total.assign(n, 0);

        for (int j = 0; j < n && j < static_cast<int>(llrs.size()); ++j) {
            llr_in[j] = llrs[j];
            llr_total[j] = llrs[j];
        }

        // Initialize variable-to-check messages with channel LLRs
        for (int i = 0; i < m; ++i) {
            for (size_t e = 0; e < H_rows[i].size(); ++e) {
                int j = H_rows[i][e];
                var_to_check[i][e] = llr_in[j];
            }
            std::fill(check_to_var[i].begin(), check_to_var[i].end(), 0);
        }

        std::vector<uint8_t> hard_bits(n);

        // Iterative decoding
        last_success = false;
        last_unsatisfied_checks = m;
        for (last_iters = 0; last_iters < max_iterations; ++last_iters) {
            // Check-to-variable messages (min-sum approximation)
            updateCheckToVar(m);

            // Variable-to-check messages and total LLRs
            llr_total = llr_in;

            for (int i = 0; i < m; ++i) {
                for (size_t e = 0; e < H_rows[i].size(); ++e) {
                    int j = H_rows[i][e];
                    llr_total[j] += check_to_var[i][e];
                }
            }

            // Update var-to-check messages
            for (int i = 0; i < m; ++i) {
                for (size_t e = 0; e < H_rows[i].size(); ++e) {
                    int j = H_rows[i][e];
                    var_to_check[i][e] = llr_total[j] - check_to_var[i][e];
                    var_to_check[i][e] = std::max(-50.0f, std::min(50.0f, var_to_check[i][e]));
                }
            }

            // Make hard decisions and check parity
            for (int j = 0; j < n; ++j) {
                hard_bits[j] = (llr_total[j] < 0) ? 1 : 0;
            }

            last_unsatisfied_checks = countUnsatisfiedChecks(hard_bits);
            if (last_unsatisfied_checks == 0) {
                last_success = true;
                break;
            }
        }

        // Extract information bits and convert to bytes
        Bytes output;
        output.reserve((k + 7) / 8);

        uint8_t byte = 0;
        int bit_count = 0;
        for (int j = 0; j < k; ++j) {
            uint8_t bit = (llr_total[j] < 0) ? 1 : 0;
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

        return output;
    }
};

LDPCDecoder::LDPCDecoder(CodeRate rate, int lifting_size)
    : impl_(std::make_unique<Impl>(rate, lifting_size)) {}

LDPCDecoder::~LDPCDecoder() = default;

Bytes LDPCDecoder::decode(ByteSpan coded_data) {
    std::vector<float> llrs;
    llrs.reserve(coded_data.size() * 8);

    for (uint8_t byte : coded_data) {
        for (int b = 7; b >= 0; --b) {
            uint8_t bit = (byte >> b) & 1;
            llrs.push_back(bit ? -6.0f : 6.0f);
        }
    }

    return decodeSoft(llrs);
}

Bytes LDPCDecoder::decodeSoft(std::span<const float> llrs) {
    if (llrs.empty()) {
        impl_->last_success = false;
        impl_->last_iters = 0;
        impl_->last_unsatisfied_checks = -1;
        return {};
    }

    int n = impl_->params.info_bits + impl_->params.parity_bits;
    int k = impl_->params.info_bits;

    if (llrs.size() % static_cast<size_t>(n) != 0) {
        impl_->last_success = false;
        impl_->last_iters = 0;
        impl_->last_unsatisfied_checks = -1;
        return {};
    }

    if (llrs.size() == static_cast<size_t>(n)) {
        return impl_->decodeBP(llrs);
    }

    std::vector<uint8_t> hard_bits(n);

    // Multi-block: decode each n-bit codeword and collect decoded BITS (not bytes)
    std::vector<uint8_t> all_decoded_bits;
    size_t offset = 0;
    impl_->last_success = true;
    impl_->last_unsatisfied_checks = 0;

    while (offset + n <= llrs.size()) {
        std::span<const float> block_llrs(llrs.data() + offset, n);

        int m = impl_->params.parity_bits;

        // Initialize channel LLRs
        impl_->llr_in.assign(n, 0);
        impl_->llr_total.assign(n, 0);

        for (int j = 0; j < n; ++j) {
            impl_->llr_in[j] = block_llrs[j];
            impl_->llr_total[j] = block_llrs[j];
        }

        // Initialize variable-to-check messages
        for (int i = 0; i < m; ++i) {
            for (size_t e = 0; e < impl_->H_rows[i].size(); ++e) {
                int j = impl_->H_rows[i][e];
                impl_->var_to_check[i][e] = impl_->llr_in[j];
            }
            std::fill(impl_->check_to_var[i].begin(), impl_->check_to_var[i].end(), 0);
        }

        // Iterative decoding
        bool block_success = false;
        int block_unsatisfied = m;
        for (impl_->last_iters = 0; impl_->last_iters < impl_->max_iterations; ++impl_->last_iters) {
            // Check-to-variable messages (min-sum)
            impl_->updateCheckToVar(m);

            // Variable-to-check messages and total LLRs
            impl_->llr_total = impl_->llr_in;
            for (int i = 0; i < m; ++i) {
                for (size_t e = 0; e < impl_->H_rows[i].size(); ++e) {
                    int j = impl_->H_rows[i][e];
                    impl_->llr_total[j] += impl_->check_to_var[i][e];
                }
            }

            for (int i = 0; i < m; ++i) {
                for (size_t e = 0; e < impl_->H_rows[i].size(); ++e) {
                    int j = impl_->H_rows[i][e];
                    impl_->var_to_check[i][e] = impl_->llr_total[j] - impl_->check_to_var[i][e];
                    impl_->var_to_check[i][e] = std::max(-50.0f, std::min(50.0f, impl_->var_to_check[i][e]));
                }
            }

            // Check parity
            for (int j = 0; j < n; ++j) {
                hard_bits[j] = (impl_->llr_total[j] < 0) ? 1 : 0;
            }
            block_unsatisfied = impl_->countUnsatisfiedChecks(hard_bits);
            if (block_unsatisfied == 0) {
                block_success = true;
                break;
            }
        }
        impl_->last_unsatisfied_checks = block_unsatisfied;

        if (!block_success) {
            impl_->last_success = false;
        }

        // Extract exactly k info BITS from this block
        for (int j = 0; j < k; ++j) {
            uint8_t bit = (impl_->llr_total[j] < 0) ? 1 : 0;
            all_decoded_bits.push_back(bit);
        }

        offset += n;
    }

    // Convert all decoded bits to bytes
    Bytes output;
    output.reserve((all_decoded_bits.size() + 7) / 8);
    uint8_t byte = 0;
    int bit_count = 0;
    for (uint8_t bit : all_decoded_bits) {
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

    return output;
}

bool LDPCDecoder::lastDecodeSuccess() const {
    return impl_->last_success;
}

int LDPCDecoder::lastIterations() const {
    return impl_->last_iters;
}

int LDPCDecoder::lastUnsatisfiedChecks() const {
    return impl_->last_unsatisfied_checks;
}

void LDPCDecoder::setRate(CodeRate rate) {
    // Skip the expensive matrix rebuild if rate didn't change. Decode hot
    // paths (e.g. streaming_decoder.cpp:2359 in decodeFrame) call setRate()
    // on every frame regardless. buildMatrix() expands the IEEE 802.11n
    // parity-check matrix and allocates fresh H_rows/H_cols vectors with
    // thousands of entries — ~75ms each on Pi 5. For a 50 KB transfer at
    // a fixed rate that's ~69 seconds of pure no-op matrix rebuilding.
    if (impl_->rate == rate) return;
    impl_->rate = rate;
    impl_->params = getCodeParams(rate, impl_->lifting_size);
    impl_->buildMatrix();
}

CodeRate LDPCDecoder::getRate() const {
    return impl_->rate;
}

void LDPCDecoder::setMaxIterations(int max_iter) {
    impl_->max_iterations = max_iter;
}

void LDPCDecoder::setMinSumFactor(float factor) {
    impl_->min_sum_factor = factor;
}

// ============ Interleaver ============

Interleaver::Interleaver(size_t rows, size_t cols)
    : rows_(rows), cols_(cols) {
    size_t n = rows * cols;
    permutation_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        size_t row = i / cols;
        size_t col = i % cols;
        permutation_[i] = col * rows + row;
    }
}

Bytes Interleaver::interleave(ByteSpan data) {
    size_t n = rows_ * cols_;
    size_t byte_size = (n + 7) / 8;

    std::vector<uint8_t> bits(n, 0);
    for (size_t i = 0; i < data.size() && i * 8 < n; ++i) {
        for (int b = 0; b < 8 && i * 8 + b < n; ++b) {
            bits[i * 8 + b] = (data[i] >> (7 - b)) & 1;
        }
    }

    std::vector<uint8_t> interleaved(n);
    for (size_t i = 0; i < n; ++i) {
        interleaved[permutation_[i]] = bits[i];
    }

    Bytes output(byte_size, 0);
    for (size_t i = 0; i < n; ++i) {
        if (interleaved[i]) {
            output[i / 8] |= (1 << (7 - (i % 8)));
        }
    }

    return output;
}

Bytes Interleaver::deinterleave(ByteSpan data) {
    size_t n = rows_ * cols_;
    size_t byte_size = (n + 7) / 8;

    std::vector<uint8_t> bits(n, 0);
    for (size_t i = 0; i < data.size() && i * 8 < n; ++i) {
        for (int b = 0; b < 8 && i * 8 + b < n; ++b) {
            bits[i * 8 + b] = (data[i] >> (7 - b)) & 1;
        }
    }

    std::vector<uint8_t> deinterleaved(n);
    for (size_t i = 0; i < n; ++i) {
        deinterleaved[i] = bits[permutation_[i]];
    }

    Bytes output(byte_size, 0);
    for (size_t i = 0; i < n; ++i) {
        if (deinterleaved[i]) {
            output[i / 8] |= (1 << (7 - (i % 8)));
        }
    }

    return output;
}

std::vector<float> Interleaver::interleave(std::span<const float> soft_bits) {
    size_t n = soft_bits.size();
    std::vector<float> output(n);
    for (size_t i = 0; i < n && i < permutation_.size(); ++i) {
        output[permutation_[i]] = soft_bits[i];
    }
    return output;
}

std::vector<float> Interleaver::deinterleave(std::span<const float> soft_bits) {
    size_t n = soft_bits.size();
    std::vector<float> output(n);
    for (size_t i = 0; i < n && i < permutation_.size(); ++i) {
        output[i] = soft_bits[permutation_[i]];
    }
    return output;
}

// ============ ChannelInterleaver ============

static size_t findCoprimeStep(size_t n, size_t total) {
    auto gcd = [](size_t a, size_t b) {
        while (b != 0) {
            size_t t = b;
            b = a % b;
            a = t;
        }
        return a;
    };

    size_t target_step = n * 3;
    if (target_step >= total) target_step = total / 2;

    for (size_t step = target_step; step < total; step++) {
        if (gcd(step, total) == 1) return step;
    }

    for (size_t step = n + 1; step < total; step++) {
        if (gcd(step, total) == 1) return step;
    }
    return n + 1;
}

ChannelInterleaver::ChannelInterleaver(size_t bits_per_symbol, size_t total_bits)
    : bits_per_symbol_(bits_per_symbol)
    , total_bits_(total_bits)
{
    num_symbols_ = (total_bits + bits_per_symbol - 1) / bits_per_symbol;

    size_t step = findCoprimeStep(bits_per_symbol, total_bits);

    symbol_separation_ = step / bits_per_symbol;
    if (symbol_separation_ < 1) symbol_separation_ = 1;

    permutation_.resize(total_bits);
    inverse_permutation_.resize(total_bits);

    for (size_t i = 0; i < total_bits; ++i) {
        size_t dest = (i * step) % total_bits;
        permutation_[i] = dest;
        inverse_permutation_[dest] = i;
    }
}

std::vector<float> ChannelInterleaver::interleave(std::span<const float> soft_bits) {
    size_t n = std::min(soft_bits.size(), total_bits_);
    std::vector<float> output(total_bits_, 0.0f);

    for (size_t i = 0; i < n; ++i) {
        output[permutation_[i]] = soft_bits[i];
    }
    return output;
}

std::vector<float> ChannelInterleaver::deinterleave(std::span<const float> soft_bits) {
    size_t n = std::min(soft_bits.size(), total_bits_);
    std::vector<float> output(total_bits_, 0.0f);

    for (size_t i = 0; i < n; ++i) {
        output[inverse_permutation_[i]] = soft_bits[i];
    }
    return output;
}

Bytes ChannelInterleaver::interleave(ByteSpan data) {
    std::vector<uint8_t> bits(total_bits_, 0);
    for (size_t i = 0; i < data.size() && i * 8 < total_bits_; ++i) {
        for (int b = 0; b < 8 && i * 8 + b < total_bits_; ++b) {
            bits[i * 8 + b] = (data[i] >> (7 - b)) & 1;
        }
    }

    std::vector<uint8_t> interleaved(total_bits_);
    for (size_t i = 0; i < total_bits_; ++i) {
        interleaved[permutation_[i]] = bits[i];
    }

    size_t byte_size = (total_bits_ + 7) / 8;
    Bytes output(byte_size, 0);
    for (size_t i = 0; i < total_bits_; ++i) {
        if (interleaved[i]) {
            output[i / 8] |= (1 << (7 - (i % 8)));
        }
    }
    return output;
}

Bytes ChannelInterleaver::deinterleave(ByteSpan data) {
    std::vector<uint8_t> bits(total_bits_, 0);
    for (size_t i = 0; i < data.size() && i * 8 < total_bits_; ++i) {
        for (int b = 0; b < 8 && i * 8 + b < total_bits_; ++b) {
            bits[i * 8 + b] = (data[i] >> (7 - b)) & 1;
        }
    }

    std::vector<uint8_t> deinterleaved(total_bits_);
    for (size_t i = 0; i < total_bits_; ++i) {
        deinterleaved[inverse_permutation_[i]] = bits[i];
    }

    size_t byte_size = (total_bits_ + 7) / 8;
    Bytes output(byte_size, 0);
    for (size_t i = 0; i < total_bits_; ++i) {
        if (deinterleaved[i]) {
            output[i / 8] |= (1 << (7 - (i % 8)));
        }
    }
    return output;
}

} // namespace ultra
