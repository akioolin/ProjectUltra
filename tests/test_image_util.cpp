#include "gui/image_util.hpp"
#include "helpers/temp_dir.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using ultra::gui::ImageFormat;
using ultra::gui::ImageInfo;
using ultra::gui::readImageInfo;
using ultra::gui::resizeAndEncodeJPEG;
using ultra::gui::sniffImageFormat;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

std::filesystem::path sampleFixturePath() {
    return std::filesystem::path(PROJECT_SOURCE_DIR) /
           "tests" / "fixtures" / "image_util" / "sample.jpg";
}

void writeBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    expect(out.good(), "failed to create fixture file: " + path.string());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    expect(out.good(), "failed to create text file: " + path.string());
    out << text;
}

void testSniffImageFormat() {
    ultra::test::TempDir tmp("ultra_image_util_sniff");
    expect(tmp.valid(), "temp dir unavailable");

    constexpr uint8_t kTinyPng[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
        0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00,
        0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
        0x42, 0x60, 0x82
    };

    const auto png_path = tmp.child("one_pixel.png");
    const auto text_path = tmp.child("plain.txt");
    writeBytes(png_path, std::vector<uint8_t>(std::begin(kTinyPng), std::end(kTinyPng)));
    writeText(text_path, "not an image\n");

    ImageFormat fmt = ImageFormat::Unknown;
    expect(sniffImageFormat(sampleFixturePath().string(), fmt), "JPEG fixture was not sniffed");
    expect(fmt == ImageFormat::JPEG, "JPEG fixture format mismatch");

    fmt = ImageFormat::Unknown;
    expect(sniffImageFormat(png_path.string(), fmt), "PNG fixture was not sniffed");
    expect(fmt == ImageFormat::PNG, "PNG fixture format mismatch");

    fmt = ImageFormat::JPEG;
    expect(!sniffImageFormat(text_path.string(), fmt), "text file should not sniff as image");
    expect(fmt == ImageFormat::Unknown, "text file should reset format to Unknown");
}

void testReadImageInfo() {
    ImageInfo info;
    expect(readImageInfo(sampleFixturePath().string(), info), "failed to read JPEG info");
    expect(info.format == ImageFormat::JPEG, "JPEG info format mismatch");
    expect(info.width == 160, "JPEG width mismatch");
    expect(info.height == 120, "JPEG height mismatch");
    expect(info.channels == 3, "JPEG channel count mismatch");
    expect(info.file_size_bytes > 0 && info.file_size_bytes <= 10 * 1024,
           "JPEG fixture size outside expected range");
}

void testResizeAndEncodeJPEG() {
    ultra::test::TempDir tmp("ultra_image_util_resize");
    expect(tmp.valid(), "temp dir unavailable");

    const auto dst = tmp.child("thumb.jpg");
    std::string error;
    expect(resizeAndEncodeJPEG(sampleFixturePath().string(), 320, 240, 70,
                               dst.string(), error),
           "resize failed: " + error);
    expect(std::filesystem::exists(dst), "resized JPEG was not written");
    expect(std::filesystem::file_size(dst) <= 25 * 1024, "resized JPEG exceeds 25 KB");

    ImageInfo info;
    expect(readImageInfo(dst.string(), info), "resized JPEG info unreadable");
    expect(info.width <= 320 && info.height <= 240, "resized JPEG exceeds bounding box");
    expect(info.width == 160 && info.height == 120, "in-box source should keep dimensions");
}

} // namespace

int main() {
    testSniffImageFormat();
    testReadImageInfo();
    testResizeAndEncodeJPEG();
    std::cout << "ImageUtil tests passed\n";
    return 0;
}
