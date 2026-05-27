#pragma once

#include "ultra/types.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace ultra {
namespace protocol {

// Transfer-layer header for a one-way file burst stream (design §14.18).
//
// Rides in the FRONT of the first burst's decoded payload — sent ONCE (the
// stop-and-wait ARQ resends burst #0 if it is lost, so it is reliably delivered
// without per-burst repetition). It declares everything the receiver needs to
// reassemble, optionally decompress, verify, and name the file. Compression is
// OPTIONAL per file: codec == None means the bytes are sent raw.
//
// Completion is size-driven: the receiver accumulates `payload_size` payload
// bytes, then (if compressed) inflates to `original_size`, verifies `crc32`, and
// writes the file as `name`. No explicit END frame is required.
struct FileStreamHeader {
    static constexpr uint8_t kMagic = 0xF5;
    static constexpr uint8_t kVersion = 1;

    enum class Codec : uint8_t {
        None = 0,      // raw — file not compressed (the optional path)
        Deflate = 1,   // zlib/deflate
        Zstd = 2,
    };

    // Fixed prefix (before the variable-length name): magic + version + codec +
    // four LE32 fields + name_len = 20 bytes.
    static constexpr size_t kFixedSize = 1 + 1 + 1 + 4 + 4 + 4 + 4 + 1;

    uint8_t version = kVersion;
    Codec codec = Codec::None;
    uint32_t original_size = 0;   // uncompressed file size (progress + inflate buffer)
    uint32_t payload_size = 0;    // bytes transmitted (== original_size when codec==None)
    uint32_t crc32 = 0;           // CRC32 of the ORIGINAL (decompressed) file
    uint32_t start_offset = 0;    // reserved for resume-after-dead-link; 0 = from start
    std::string name;             // filename, truncated to 255 bytes on the wire

    bool isCompressed() const { return codec != Codec::None; }

    // Bytes this header occupies on the wire (payload begins after this).
    size_t wireSize() const {
        return kFixedSize + std::min<size_t>(name.size(), 255);
    }

    Bytes serialize() const {
        Bytes out;
        const uint8_t nlen = static_cast<uint8_t>(std::min<size_t>(name.size(), 255));
        out.reserve(kFixedSize + nlen);
        out.push_back(kMagic);
        out.push_back(version);
        out.push_back(static_cast<uint8_t>(codec));
        appendLE32(out, original_size);
        appendLE32(out, payload_size);
        appendLE32(out, crc32);
        appendLE32(out, start_offset);
        out.push_back(nlen);
        out.insert(out.end(), name.begin(), name.begin() + nlen);
        return out;
    }

    // Parse from the front of a decoded byte stream; payload begins at the
    // returned header's wireSize(). Returns nullopt if malformed or truncated.
    static std::optional<FileStreamHeader> deserialize(const Bytes& data) {
        if (data.size() < kFixedSize) return std::nullopt;
        if (data[0] != kMagic) return std::nullopt;
        FileStreamHeader h;
        h.version = data[1];
        if (h.version != kVersion) return std::nullopt;
        h.codec = static_cast<Codec>(data[2]);
        h.original_size = readLE32(data, 3);
        h.payload_size = readLE32(data, 7);
        h.crc32 = readLE32(data, 11);
        h.start_offset = readLE32(data, 15);
        const uint8_t nlen = data[19];
        if (data.size() < kFixedSize + nlen) return std::nullopt;
        h.name.assign(reinterpret_cast<const char*>(data.data() + kFixedSize), nlen);
        return h;
    }

private:
    static void appendLE32(Bytes& out, uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    }
    static uint32_t readLE32(const Bytes& d, size_t p) {
        return static_cast<uint32_t>(d[p]) |
               (static_cast<uint32_t>(d[p + 1]) << 8) |
               (static_cast<uint32_t>(d[p + 2]) << 16) |
               (static_cast<uint32_t>(d[p + 3]) << 24);
    }
};

}  // namespace protocol
}  // namespace ultra
