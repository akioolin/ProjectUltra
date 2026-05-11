#include "diagnostics/event_buffer.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cstring>

namespace ultra::diagnostics {

namespace {
void copyFixed(char* dst, size_t dst_size, const char* src) noexcept {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    std::snprintf(dst, dst_size, "%s", src);
}
} // namespace

DiagEvent makeDiagEvent(const char* component,
                        const char* event,
                        const char* fields_json,
                        const char* privacy) noexcept {
    DiagEvent out;
    copyFixed(out.component, sizeof(out.component), component);
    copyFixed(out.event, sizeof(out.event), event);
    copyFixed(out.privacy, sizeof(out.privacy), privacy);
    copyFixed(out.fields_json, sizeof(out.fields_json), fields_json);
    return out;
}

EventBuffer::EventBuffer(size_t capacity) {
    reset(capacity);
}

void EventBuffer::reset(size_t capacity) {
    capacity_ = std::max<size_t>(capacity, 1);
    slots_ = std::make_unique<Slot[]>(capacity_);
    for (size_t i = 0; i < capacity_; ++i) {
        slots_[i].sequence.store(0, std::memory_order_relaxed);
        slots_[i].length.store(0, std::memory_order_relaxed);
        for (auto& b : slots_[i].bytes) {
            b.store('\0', std::memory_order_relaxed);
        }
    }
    write_seq_.store(0, std::memory_order_release);
    dropped_events_.store(0, std::memory_order_release);
}

void EventBuffer::utcTimestamp(char* dst, size_t dst_size) noexcept {
    if (!dst || dst_size == 0) {
        return;
    }
    std::time_t now = std::time(nullptr);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &now);
#else
    gmtime_r(&now, &tm_utc);
#endif
    std::strftime(dst, dst_size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

void EventBuffer::push(const DiagEvent& event) noexcept {
    if (!slots_ || capacity_ == 0) {
        return;
    }

    const uint64_t pos = write_seq_.fetch_add(1, std::memory_order_acq_rel);
    if (pos >= capacity_) {
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
    }

    Slot& slot = slots_[pos % capacity_];
    slot.sequence.store(0, std::memory_order_release);

    char ts[32]{};
    utcTimestamp(ts, sizeof(ts));

    char line[kMaxJsonLine]{};
    const char* fields = event.fields_json[0] ? event.fields_json : "{}";
    std::snprintf(line, sizeof(line),
                  "{\"schema\":1,\"seq\":%llu,\"ts_utc\":\"%s\","
                  "\"component\":\"%s\",\"event\":\"%s\",\"privacy\":\"%s\","
                  "\"fields\":%s}",
                  static_cast<unsigned long long>(pos + 1),
                  ts,
                  event.component,
                  event.event,
                  event.privacy[0] ? event.privacy : "redacted",
                  fields);

    const size_t len = std::min(std::strlen(line), kMaxJsonLine - 1);
    for (size_t i = 0; i < len; ++i) {
        slot.bytes[i].store(line[i], std::memory_order_relaxed);
    }
    slot.bytes[len].store('\0', std::memory_order_relaxed);
    slot.length.store(static_cast<uint16_t>(len), std::memory_order_release);
    slot.sequence.store(pos + 1, std::memory_order_release);
}

std::vector<std::string> EventBuffer::snapshotLines() const {
    std::vector<std::string> out;
    if (!slots_ || capacity_ == 0) {
        return out;
    }

    const uint64_t end = write_seq_.load(std::memory_order_acquire);
    const uint64_t start = end > capacity_ ? end - capacity_ : 0;
    out.reserve(static_cast<size_t>(end - start));

    for (uint64_t pos = start; pos < end; ++pos) {
        const Slot& slot = slots_[pos % capacity_];
        const uint64_t expected = pos + 1;
        const uint64_t seq_before = slot.sequence.load(std::memory_order_acquire);
        if (seq_before != expected) {
            continue;
        }
        const uint16_t len = slot.length.load(std::memory_order_acquire);
        if (len == 0 || len >= kMaxJsonLine) {
            continue;
        }
        std::string line;
        line.resize(len);
        for (uint16_t i = 0; i < len; ++i) {
            line[i] = slot.bytes[i].load(std::memory_order_relaxed);
        }
        const uint64_t seq_after = slot.sequence.load(std::memory_order_acquire);
        if (seq_after == expected) {
            out.push_back(std::move(line));
        }
    }
    return out;
}

std::vector<std::string> EventBuffer::snapshotLinesFrom(uint64_t next_sequence,
                                                        uint64_t* next_sequence_out,
                                                        uint64_t* dropped_before) const {
    std::vector<std::string> out;
    if (next_sequence_out) {
        *next_sequence_out = next_sequence == 0 ? 1 : next_sequence;
    }
    if (dropped_before) {
        *dropped_before = 0;
    }
    if (!slots_ || capacity_ == 0) {
        return out;
    }

    const uint64_t end = write_seq_.load(std::memory_order_acquire);
    if (end == 0) {
        return out;
    }

    uint64_t seq = next_sequence == 0 ? 1 : next_sequence;
    const uint64_t earliest = end > capacity_ ? end - capacity_ + 1 : 1;
    if (seq < earliest) {
        if (dropped_before) {
            *dropped_before = earliest - seq;
        }
        seq = earliest;
    }
    out.reserve(static_cast<size_t>(end >= seq ? end - seq + 1 : 0));

    for (; seq <= end; ++seq) {
        const uint64_t pos = seq - 1;
        const Slot& slot = slots_[pos % capacity_];
        const uint64_t seq_before = slot.sequence.load(std::memory_order_acquire);
        if (seq_before != seq) {
            break;
        }
        const uint16_t len = slot.length.load(std::memory_order_acquire);
        if (len == 0 || len >= kMaxJsonLine) {
            break;
        }
        std::string line;
        line.resize(len);
        for (uint16_t i = 0; i < len; ++i) {
            line[i] = slot.bytes[i].load(std::memory_order_relaxed);
        }
        const uint64_t seq_after = slot.sequence.load(std::memory_order_acquire);
        if (seq_after != seq) {
            break;
        }
        out.push_back(std::move(line));
    }

    if (next_sequence_out) {
        *next_sequence_out = seq;
    }
    return out;
}

std::string EventBuffer::snapshotJsonl() const {
    auto lines = snapshotLines();
    std::string out;
    size_t total = 0;
    for (const auto& line : lines) {
        total += line.size() + 1;
    }
    out.reserve(total);
    for (const auto& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

} // namespace ultra::diagnostics
