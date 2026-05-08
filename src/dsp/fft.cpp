#include "ultra/dsp.hpp"

#include <stdexcept>

#ifndef POCKETFFT_CACHE_SIZE
#define POCKETFFT_CACHE_SIZE 64
#endif

#if defined(__APPLE__) || defined(__unix__)
#define POCKETFFT_USE_POSIX_MEMALIGN
#endif

#include "pocketfft/pocketfft_hdronly.h"

namespace ultra {

struct FFT::Impl {
    explicit Impl(size_t n)
        : shape{n},
          complex_stride{static_cast<ptrdiff_t>(sizeof(Complex))},
          real_stride{static_cast<ptrdiff_t>(sizeof(Sample))},
          axes{0} {}

    pocketfft::shape_t shape;
    pocketfft::stride_t complex_stride;
    pocketfft::stride_t real_stride;
    pocketfft::shape_t axes;
};

FFT::FFT(size_t size) : size_(size), impl_(std::make_unique<Impl>(size)) {
    if ((size & (size - 1)) != 0) {
        throw std::invalid_argument("FFT size must be power of 2");
    }
}

FFT::~FFT() = default;

void FFT::forward(const Complex* in, Complex* out) {
    pocketfft::c2c<float>(
        impl_->shape, impl_->complex_stride, impl_->complex_stride, impl_->axes,
        pocketfft::FORWARD, in, out, 1.0f
    );
}

void FFT::forward(const std::vector<Complex>& in, std::vector<Complex>& out) {
    if (in.size() != size_) throw std::invalid_argument("Input size mismatch");
    out.resize(size_);
    forward(in.data(), out.data());
}

void FFT::inverse(const Complex* in, Complex* out) {
    pocketfft::c2c<float>(
        impl_->shape, impl_->complex_stride, impl_->complex_stride, impl_->axes,
        pocketfft::BACKWARD, in, out, 1.0f / static_cast<float>(size_)
    );
}

void FFT::inverse(const std::vector<Complex>& in, std::vector<Complex>& out) {
    if (in.size() != size_) throw std::invalid_argument("Input size mismatch");
    out.resize(size_);
    inverse(in.data(), out.data());
}

void FFT::forwardReal(const Sample* in, Complex* out) {
    pocketfft::r2c<float>(
        impl_->shape, impl_->real_stride, impl_->complex_stride, impl_->axes,
        pocketfft::FORWARD, in, out, 1.0f
    );
}

void FFT::inverseReal(const Complex* in, Sample* out) {
    pocketfft::c2r<float>(
        impl_->shape, impl_->complex_stride, impl_->real_stride, impl_->axes,
        pocketfft::BACKWARD, in, out, 1.0f / static_cast<float>(size_)
    );
}

} // namespace ultra
