#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ultra::diagnostics {

struct DiagEvent {
    char component[32]{};
    char event[48]{};
    char privacy[16]{};
    char fields_json[320]{};
};

DiagEvent makeDiagEvent(const char* component,
                        const char* event,
                        const char* fields_json = "{}",
                        const char* privacy = "redacted") noexcept;

class EventBuffer {
public:
    static constexpr size_t kDefaultCapacity = 4096;
    static constexpr size_t kMaxJsonLine = 512;

    explicit EventBuffer(size_t capacity = kDefaultCapacity);
    EventBuffer(const EventBuffer&) = delete;
    EventBuffer& operator=(const EventBuffer&) = delete;

    void reset(size_t capacity = kDefaultCapacity);
    void push(const DiagEvent& event) noexcept;

    std::vector<std::string> snapshotLines() const;
    std::vector<std::string> snapshotLinesFrom(uint64_t next_sequence,
                                               uint64_t* next_sequence_out,
                                               uint64_t* dropped_before = nullptr) const;
    std::string snapshotJsonl() const;

    uint64_t eventsWritten() const noexcept {
        return write_seq_.load(std::memory_order_acquire);
    }
    uint64_t eventsDropped() const noexcept {
        return dropped_events_.load(std::memory_order_acquire);
    }
    size_t capacity() const noexcept { return capacity_; }

private:
    struct Slot {
        std::atomic<uint64_t> sequence{0};
        std::atomic<uint16_t> length{0};
        std::array<std::atomic<char>, kMaxJsonLine> bytes{};
    };

    static void utcTimestamp(char* dst, size_t dst_size) noexcept;

    std::unique_ptr<Slot[]> slots_;
    size_t capacity_ = 0;
    std::atomic<uint64_t> write_seq_{0};
    std::atomic<uint64_t> dropped_events_{0};
};

} // namespace ultra::diagnostics
