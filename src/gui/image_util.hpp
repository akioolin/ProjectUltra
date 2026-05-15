#pragma once

#include <cstdint>
#include <string>

namespace ultra {
namespace gui {

enum class ImageFormat { Unknown, JPEG, PNG };

struct ImageInfo {
    int width = 0;
    int height = 0;
    int channels = 0;
    uint64_t file_size_bytes = 0;
    ImageFormat format = ImageFormat::Unknown;
};

bool sniffImageFormat(const std::string& path, ImageFormat& out);
bool readImageInfo(const std::string& path, ImageInfo& out);
bool resizeAndEncodeJPEG(const std::string& src_path,
                         int max_w, int max_h, int quality,
                         const std::string& dst_path,
                         std::string& error);

} // namespace gui
} // namespace ultra
