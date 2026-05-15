#include "image_util.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace ultra {
namespace gui {

namespace {

constexpr int kJPEGChannels = 3;

struct StbiImageDeleter {
    void operator()(unsigned char* ptr) const {
        stbi_image_free(ptr);
    }
};

using StbiImagePtr = std::unique_ptr<unsigned char, StbiImageDeleter>;

std::string stbiError(const char* prefix) {
    const char* reason = stbi_failure_reason();
    if (reason && reason[0] != '\0') {
        return std::string(prefix) + ": " + reason;
    }
    return prefix;
}

bool fileSizeBytes(const std::string& path, uint64_t& out) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > std::numeric_limits<uint64_t>::max()) {
        return false;
    }
    out = static_cast<uint64_t>(size);
    return true;
}

void fitInsideBox(int src_w, int src_h, int max_w, int max_h, int& dst_w, int& dst_h) {
    if (src_w <= max_w && src_h <= max_h) {
        dst_w = src_w;
        dst_h = src_h;
        return;
    }

    const int64_t width_bound_h =
        (static_cast<int64_t>(src_h) * max_w + src_w / 2) / src_w;
    if (width_bound_h <= max_h) {
        dst_w = max_w;
        dst_h = std::max(1, static_cast<int>(width_bound_h));
        return;
    }

    const int64_t height_bound_w =
        (static_cast<int64_t>(src_w) * max_h + src_h / 2) / src_h;
    dst_w = std::max(1, std::min(max_w, static_cast<int>(height_bound_w)));
    dst_h = max_h;
}

} // namespace

bool sniffImageFormat(const std::string& path, ImageFormat& out) {
    out = ImageFormat::Unknown;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    unsigned char magic[8] = {};
    in.read(reinterpret_cast<char*>(magic), sizeof(magic));
    const std::streamsize n = in.gcount();

    if (n >= 3 && magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) {
        out = ImageFormat::JPEG;
        return true;
    }

    constexpr unsigned char kPngMagic[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    };
    if (n >= 8 && std::equal(std::begin(kPngMagic), std::end(kPngMagic), magic)) {
        out = ImageFormat::PNG;
        return true;
    }

    return false;
}

bool readImageInfo(const std::string& path, ImageInfo& out) {
    out = ImageInfo{};

    ImageFormat format = ImageFormat::Unknown;
    if (!sniffImageFormat(path, format)) {
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    if (!stbi_info(path.c_str(), &width, &height, &channels) ||
        width <= 0 || height <= 0 || channels <= 0) {
        return false;
    }

    uint64_t size = 0;
    if (!fileSizeBytes(path, size)) {
        return false;
    }

    out.width = width;
    out.height = height;
    out.channels = channels;
    out.file_size_bytes = size;
    out.format = format;
    return true;
}

bool resizeAndEncodeJPEG(const std::string& src_path,
                         int max_w, int max_h, int quality,
                         const std::string& dst_path,
                         std::string& error) {
    error.clear();

    if (max_w <= 0 || max_h <= 0) {
        error = "invalid resize bounds";
        return false;
    }
    if (quality < 1 || quality > 100) {
        error = "invalid JPEG quality";
        return false;
    }

    int src_w = 0;
    int src_h = 0;
    int src_channels = 0;
    StbiImagePtr src(stbi_load(src_path.c_str(), &src_w, &src_h, &src_channels, kJPEGChannels));
    (void)src_channels;
    if (!src || src_w <= 0 || src_h <= 0) {
        error = stbiError("failed to decode image");
        return false;
    }

    int dst_w = 0;
    int dst_h = 0;
    fitInsideBox(src_w, src_h, max_w, max_h, dst_w, dst_h);

    const size_t pixel_count = static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h);
    if (pixel_count > std::numeric_limits<size_t>::max() / kJPEGChannels) {
        error = "resized image is too large";
        return false;
    }

    std::vector<unsigned char> resized(pixel_count * kJPEGChannels);
    if (!stbir_resize_uint8_srgb(src.get(), src_w, src_h, 0,
                                 resized.data(), dst_w, dst_h, 0,
                                 STBIR_RGB)) {
        error = "failed to resize image";
        return false;
    }

    if (!stbi_write_jpg(dst_path.c_str(), dst_w, dst_h, kJPEGChannels,
                        resized.data(), quality)) {
        error = "failed to encode JPEG";
        return false;
    }

    return true;
}

} // namespace gui
} // namespace ultra
