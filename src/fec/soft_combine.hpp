#pragma once

#include "ultra/types.hpp"
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ultra::fec {

class SoftCombineBuffer {
public:
    struct Key {
        uint32_t sender_hash = 0;
        uint16_t seq = 0;
        CodeRate rate = CodeRate::R1_4;
        uint8_t cw_count = 0;
        // PHY parameters that change LLR scaling and ordering. If these
        // differ between attempts, the stored LLRs aren't comparable
        // and combining would corrupt the decode. Including them in
        // the key forces a fresh entry instead of mis-combining.
        Modulation modulation = Modulation::DQPSK;
        uint8_t channel_interleave = 0;  // 0 = off, 1 = on

        bool operator==(const Key& other) const {
            return sender_hash == other.sender_hash &&
                   seq == other.seq &&
                   rate == other.rate &&
                   cw_count == other.cw_count &&
                   modulation == other.modulation &&
                   channel_interleave == other.channel_interleave;
        }
    };

    struct KeyHash {
        size_t operator()(const Key& key) const;
    };

    int combine(const Key& key, const std::vector<float>& incoming_llrs,
                std::vector<float>& out_llrs);
    void retain(const Key& key, std::vector<float> combined_llrs);
    void drop(const Key& key);
    void clear();
    void tick(uint32_t elapsed_ms);

    void setTTL(uint32_t ttl_ms);
    void setMaxEntries(size_t n);
    void setEnabled(bool enabled);

    size_t size() const;
    bool enabled() const;

private:
    struct Entry {
        std::vector<float> llrs;
        uint32_t age_ms = 0;
        int attempts = 1;
        std::list<Key>::iterator lru_it;
    };

    void touchLocked(Entry& entry);
    void eraseLocked(const Key& key);
    void evictExpiredLocked();
    void evictOverflowLocked();

    bool enabled_ = false;
    uint32_t ttl_ms_ = 30000;
    size_t max_entries_ = 32;

    std::list<Key> lru_;
    std::unordered_map<Key, Entry, KeyHash> entries_;
    mutable std::mutex mutex_;
};

} // namespace ultra::fec
