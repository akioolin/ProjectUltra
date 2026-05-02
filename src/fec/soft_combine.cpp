#include "soft_combine.hpp"
#include <algorithm>
#include <limits>

namespace ultra::fec {

size_t SoftCombineBuffer::KeyHash::operator()(const Key& key) const {
    size_t h = static_cast<size_t>(key.sender_hash);
    h ^= static_cast<size_t>(key.seq) + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= static_cast<size_t>(key.cw_count) + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= static_cast<size_t>(key.rate) + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h;
}

int SoftCombineBuffer::combine(const Key& key, const std::vector<float>& incoming_llrs,
                               std::vector<float>& out_llrs) {
    out_llrs = incoming_llrs;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || key.sender_hash == 0) {
        return 1;
    }

    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return 1;
    }

    Entry& entry = it->second;
    if (entry.llrs.size() != incoming_llrs.size()) {
        eraseLocked(key);
        return 1;
    }

    const int next_attempts = std::max(1, entry.attempts) + 1;
    const float denom = static_cast<float>(next_attempts);
    for (size_t i = 0; i < incoming_llrs.size(); ++i) {
        out_llrs[i] = entry.llrs[i] + (incoming_llrs[i] - entry.llrs[i]) / denom;
    }

    touchLocked(entry);
    return next_attempts;
}

void SoftCombineBuffer::retain(const Key& key, std::vector<float> combined_llrs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || key.sender_hash == 0 || max_entries_ == 0 || combined_llrs.empty()) {
        return;
    }

    auto it = entries_.find(key);
    int attempts = 1;
    if (it != entries_.end()) {
        attempts = std::max(1, it->second.attempts) + 1;
        eraseLocked(key);
    } else {
        while (entries_.size() >= max_entries_ && !lru_.empty()) {
            eraseLocked(lru_.back());
        }
    }

    lru_.push_front(key);
    Entry entry;
    entry.llrs = std::move(combined_llrs);
    entry.age_ms = 0;
    entry.attempts = attempts;
    entry.lru_it = lru_.begin();
    entries_.emplace(key, std::move(entry));
    evictOverflowLocked();
}

void SoftCombineBuffer::drop(const Key& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    eraseLocked(key);
}

void SoftCombineBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    lru_.clear();
}

void SoftCombineBuffer::tick(uint32_t elapsed_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty()) {
        return;
    }

    for (auto& kv : entries_) {
        Entry& entry = kv.second;
        if (elapsed_ms > std::numeric_limits<uint32_t>::max() - entry.age_ms) {
            entry.age_ms = std::numeric_limits<uint32_t>::max();
        } else {
            entry.age_ms += elapsed_ms;
        }
    }
    evictExpiredLocked();
}

void SoftCombineBuffer::setTTL(uint32_t ttl_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    ttl_ms_ = ttl_ms;
    evictExpiredLocked();
}

void SoftCombineBuffer::setMaxEntries(size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_entries_ = n;
    evictOverflowLocked();
}

void SoftCombineBuffer::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    if (!enabled_) {
        entries_.clear();
        lru_.clear();
    }
}

size_t SoftCombineBuffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

bool SoftCombineBuffer::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

void SoftCombineBuffer::touchLocked(Entry& entry) {
    lru_.splice(lru_.begin(), lru_, entry.lru_it);
    entry.lru_it = lru_.begin();
}

void SoftCombineBuffer::eraseLocked(const Key& key) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return;
    }
    lru_.erase(it->second.lru_it);
    entries_.erase(it);
}

void SoftCombineBuffer::evictExpiredLocked() {
    if (ttl_ms_ == 0) {
        entries_.clear();
        lru_.clear();
        return;
    }

    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.age_ms >= ttl_ms_) {
            lru_.erase(it->second.lru_it);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void SoftCombineBuffer::evictOverflowLocked() {
    if (max_entries_ == 0) {
        entries_.clear();
        lru_.clear();
        return;
    }

    while (entries_.size() > max_entries_ && !lru_.empty()) {
        eraseLocked(lru_.back());
    }
}

} // namespace ultra::fec
