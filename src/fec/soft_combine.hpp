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
        uint8_t cw_index = 0;
        // PHY parameters that change LLR scaling and bit ordering. If
        // any of these differ between attempts, stored LLRs aren't
        // comparable and combining would corrupt the decode. Including
        // them in the key forces a fresh entry instead of mis-combining.
        Modulation modulation = Modulation::DQPSK;
        uint8_t channel_interleave = 0;  // 0 = off, 1 = on
        // Hash of (waveform_mode, ofdm_data_carriers). Bits-per-symbol
        // is implied by the modulation field above; pilot spacing
        // changes within the same waveform mode are NOT distinguished
        // here — that's a known gap (Codex review of commit 1f18683).
        // Today's modes (CHIRP/COX/NARROW) each have a fixed pilot
        // layout, so this is sufficient in practice; revisit if a
        // dynamic-pilot mode is added later.
        uint16_t carrier_count_hash = 0;

        bool operator==(const Key& other) const {
            return sender_hash == other.sender_hash &&
                   seq == other.seq &&
                   rate == other.rate &&
                   cw_count == other.cw_count &&
                   cw_index == other.cw_index &&
                   modulation == other.modulation &&
                   channel_interleave == other.channel_interleave &&
                   carrier_count_hash == other.carrier_count_hash;
        }
    };

    struct KeyHash {
        size_t operator()(const Key& key) const;
    };

    // Inputs that determine a HARQ key. Pulled out of the streaming
    // decoder so the carrier_count_hash construction and field
    // population can be unit tested without standing up the whole
    // decode pipeline. waveform_mode is the underlying byte value of
    // protocol::WaveformMode (kept as int here to avoid a circular
    // include with the protocol layer).
    struct HarqKeyInputs {
        uint32_t sender_hash = 0;
        uint16_t seq = 0;
        CodeRate rate = CodeRate::R1_4;
        uint8_t cw_count = 0;
        uint8_t cw_index = 0;
        Modulation modulation = Modulation::DQPSK;
        bool channel_interleave = false;
        int waveform_mode = 0;          // protocol::WaveformMode underlying byte
        int ofdm_data_carriers = 0;
    };

    static Key makeKey(const HarqKeyInputs& in);

    int combine(const Key& key, const std::vector<float>& incoming_llrs,
                std::vector<float>& out_llrs);
    void retain(const Key& key, std::vector<float> combined_llrs);
    void drop(const Key& key);
    void retainOnlySeqWindow(uint16_t base_seq, size_t window_size);
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
