#pragma once

#include "arq_interface.hpp"
#include "frame_v2.hpp"
#include <algorithm>
#include <deque>
#include <optional>
#include <array>
#include <cstdint>

namespace ultra {
namespace protocol {

/**
 * Selective Repeat ARQ Controller
 *
 * Sliding window ARQ for higher throughput on HF:
 * - Maintains window of N frames in flight simultaneously
 * - Retransmits only failed frames (not entire window)
 * - Uses SACK (Selective ACK) with bitmap for efficiency
 * - Reorders out-of-order frames at receiver
 *
 * Compared to Stop-and-Wait:
 * - 2-4x higher throughput for typical HF RTT
 * - More complex state management
 * - Requires more memory (TX/RX buffers)
 *
 * Window size should be chosen based on:
 * - Round-trip time (RTT = propagation + processing)
 * - Frame duration
 * - Target throughput
 */
class SelectiveRepeatARQ : public IARQController {
public:
    explicit SelectiveRepeatARQ(const ARQConfig& config = ARQConfig{});

    // --- IARQController Interface ---

    ARQMode getMode() const override { return ARQMode::SELECTIVE_REPEAT; }

    void setCallsigns(const std::string& local, const std::string& remote) override;

    bool sendData(const Bytes& data) override;
    bool sendData(const std::string& text) override;
    bool sendDataWithFlags(const Bytes& data, uint8_t flags) override;

    bool isReadyToSend() const override;
    size_t getAvailableSlots() const override;

    bool lastRxHadMoreData() const override { return last_rx_more_data_; }
    uint8_t lastRxFlags() const override { return last_rx_flags_; }

    void onFrameReceived(const Bytes& frame_data) override;

    void tick(uint32_t elapsed_ms) override;

    void setTransmitCallback(TransmitCallback cb) override;
    void setDataReceivedCallback(DataReceivedCallback cb) override;
    void setSendCompleteCallback(SendCompleteCallback cb) override;

    ARQStats getStats() const override { return stats_; }
    void resetStats() override { stats_ = ARQStats{}; }

    void reset() override;
    void abortPendingTx();

    // Set the code rate for DATA frame total_cw calculation
    void setCodeRate(CodeRate rate) { code_rate_ = rate; }
    CodeRate getCodeRate() const { return code_rate_; }

    // Set window size (1 = stop-and-wait behavior for MC-DPSK)
    void setWindowSize(size_t size) {
        if (size < 1) size = 1;
        if (size > MAX_WINDOW) size = MAX_WINDOW;
        config_.window_size = size;
    }
    size_t getWindowSize() const { return config_.window_size; }

    // Set ACK timeout (adaptive based on waveform frame duration)
    void setAckTimeout(uint32_t timeout_ms) {
        config_.ack_timeout_ms = timeout_ms;
        if (!have_rtt_estimator_) {
            adaptive_ack_timeout_ms_ = timeout_ms;
        }
    }
    uint32_t getAckTimeout() const { return config_.ack_timeout_ms; }

    // Set delayed SACK coalescing timer
    void setSackDelay(uint32_t ms) { config_.sack_delay_ms = std::max(1u, ms); }
    uint32_t getSackDelay() const { return config_.sack_delay_ms; }

    // Set max retries before giving up on a frame
    void setMaxRetries(int retries) { config_.max_retries = std::max(1, retries); }
    int getMaxRetries() const { return config_.max_retries; }

    // ACK repeat: send multiple copies with delay for fading reliability
    void setAckRepeatCount(int count) { ack_repeat_count_ = std::clamp(count, 1, 3); }
    void setAckRepeatDelay(uint32_t ms) { ack_repeat_delay_ms_ = std::max(1u, ms); }

private:
    enum class RetransmitCause : uint8_t {
        TIMEOUT,
        FAST_HOLE,
        HOLE_PROBE,
        NACK
    };

    // TX state per frame in window
    struct TXSlot {
        bool active = false;        // Slot in use
        Bytes frame_data;           // Serialized v2 frame to send/resend
        uint16_t seq = 0;           // Sequence number
        uint32_t timeout_ms = 0;    // Time until retransmit
        uint64_t first_tx_ms = 0;   // ARQ monotonic clock when first sent
        bool rtt_sample_eligible = false; // Karn-safe RTT sampling guard
        int retry_count = 0;        // Number of retransmits
        bool acked = false;         // ACK received (waiting for earlier frames)
        int hole_ack_count = 0;     // Consecutive ACKs showing this frame as gap
        int fast_retx_count = 0;    // Number of fast retransmits for current hole context
        uint32_t fast_retx_cooldown_ms = 0; // Prevent ACK-repeat storms from immediate re-retransmit
        bool hole_probe_armed = false;      // Timer armed for progress-based probe retx
        uint32_t hole_probe_timer_ms = 0;   // Countdown to hole probe retx
        int hole_probe_count = 0;           // Number of hole-probe retransmits in current epoch
    };

    // RX state per frame in receive window
    struct RXSlot {
        bool received = false;      // Frame received
        uint16_t seq = 0;           // Sequence number
        Bytes payload;              // Received payload
        uint8_t flags = 0;          // Frame flags
    };

    // Maximum window size
    static constexpr size_t MAX_WINDOW = 8;

    ARQConfig config_;
    CodeRate code_rate_ = CodeRate::R1_4;  // Default R1/4, updated when connected

    // Callsigns
    std::string local_call_;
    std::string remote_call_;

    // TX state
    std::array<TXSlot, MAX_WINDOW> tx_window_;
    uint16_t tx_base_seq_ = 0;      // First unACKed sequence number
    uint16_t tx_next_seq_ = 0;      // Next sequence to assign
    size_t tx_in_flight_ = 0;       // Number of frames in flight

    // RX state
    std::array<RXSlot, MAX_WINDOW> rx_window_;
    uint16_t rx_base_seq_ = 0;      // Next expected sequence
    bool last_rx_more_data_ = false;
    uint8_t last_rx_flags_ = 0;

    // Delayed SACK for half-duplex (wait for burst to complete)
    bool sack_pending_ = false;     // SACK waiting to be sent
    uint32_t sack_timer_ms_ = 0;    // Time until SACK is sent
    uint32_t frames_since_ack_ = 0; // Frames received since last ACK sent

    // ACK repeat config (time-diversity for fading channels)
    int ack_repeat_count_ = 1;         // Total copies (1=single, 2=double, 3=triple)
    uint32_t ack_repeat_delay_ms_ = 80; // Delay between copies

    // Pending repeat state (queue avoids overwriting repeats during ACK bursts)
    struct AckRepeatJob {
        Bytes frame_data;
        uint16_t base_seq = 0;
        uint8_t bitmap = 0;
        uint32_t timer_ms = 0;
        int copy_index = 0;  // 2 = first repeat copy, 3 = second repeat copy
    };
    std::deque<AckRepeatJob> ack_repeat_jobs_;

    // Track cumulative ACK base progress (for critical immediate duplicate)
    bool last_sack_base_valid_ = false;
    uint16_t last_sack_base_ = 0;
    bool last_ack_signature_valid_ = false;
    uint16_t last_ack_seq_ = 0;
    uint8_t last_ack_bitmap_ = 0;
    uint32_t ack_dedup_timer_ms_ = 0;

    // Monotonic ARQ time and adaptive RTO estimator (Karn-safe)
    uint64_t arq_time_ms_ = 0;
    bool have_rtt_estimator_ = false;
    float srtt_ms_ = 0.0f;
    float rttvar_ms_ = 0.0f;
    uint32_t adaptive_ack_timeout_ms_ = 0;

    // Statistics
    ARQStats stats_;

    // Callbacks
    TransmitCallback on_transmit_;
    DataReceivedCallback on_data_received_;
    SendCompleteCallback on_send_complete_;

    // Internal helpers
    size_t seqToSlot(uint16_t seq) const;
    bool isInTXWindow(uint16_t seq) const;
    bool isInRXWindow(uint16_t seq) const;

    void transmitData(const Bytes& data);
    void handleDataFrame(const v2::DataFrame& frame);
    void handleAckFrame(const v2::ControlFrame& frame);
    void handleNackFrame(const v2::ControlFrame& frame);

    void retransmitFrame(size_t slot, RetransmitCause cause);
    void advanceTXWindow();
    void advanceRXWindow();
    void sendSack();
    void maybeSampleRTT(TXSlot& slot);
    uint32_t currentAckTimeoutMs() const;
    uint32_t ackRepeatDelayForCopy(int copy_index) const;
    int ackRepeatJitterMs(uint16_t base_seq, uint8_t bitmap, int copy_index) const;

    uint8_t buildRXBitmap() const;
};

} // namespace protocol
} // namespace ultra
