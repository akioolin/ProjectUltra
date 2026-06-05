#pragma once

#include "modem_adapter.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ultra::tnc {

class TNCSession {
public:
    using EmitFn = std::function<void(std::string_view line)>;
    using DataOutFn = std::function<void(const std::vector<uint8_t>&)>;

    TNCSession(ModemAdapter& modem, EmitFn cmd_emit, DataOutFn data_out);

    bool handleControlLine(std::string_view line);
    void handleDataBytes(const std::vector<uint8_t>& bytes);

    void onModemConnected(const std::string& src, const std::string& dst, int bw);
    void onModemDisconnected();
    void onModemPTT(bool on);
    void onModemDataReceived(const std::vector<uint8_t>& bytes);
    void onModemBufferLevel(int bytes);
    void onModemSNR(float db);
    void onModemBitrate(int bps);
    void onModemIncomingCall(const std::string& peer);

    void tick(uint32_t elapsed_ms);

    State getState() const { return state_; }
    const std::string& getMyCall() const { return mycall_; }

    // Pure encoder for the on-wire payload framing. Picks compression
    // when it actually saves bytes after the 1-byte marker overhead
    // and falls back to raw otherwise. Static and side-effect free so
    // tests can probe each branch (raw, compressible, uncompressible,
    // below-threshold, compression-disabled) without spinning up a
    // full session. Returns wire bytes ready for ModemAdapter::sendBinary.
    static std::vector<uint8_t> encodePayloadForWire(
        const std::vector<uint8_t>& payload, bool compression_enabled);

private:
    ModemAdapter& modem_;
    EmitFn cmd_emit_;
    DataOutFn data_out_;

    State state_ = State::IDLE;
    std::string mycall_;
    std::vector<std::string> secondary_calls_;
    int bandwidth_hz_ = 2300;
    bool compression_enabled_ = false;
    bool chat_enabled_ = false;
    bool cwid_enabled_ = false;
    bool pending_inbound_ = false;
    uint32_t iamalive_timer_ms_ = 0;
    int last_buffer_level_ = -1;
    uint32_t last_buffer_emit_ms_ = 0;
    int pending_buffer_level_ = -1;
    float last_snr_db_ = 0.0f;
    int last_bitrate_bps_ = 0;

    // TCP→modem batching: TCP delivers user data in many small chunks,
    // but Connection::sendPayload() replaces (not appends to) its pending
    // fragment queue on every call. Calling sendBinary() per TCP chunk
    // therefore strands fragments mid-transfer. Accumulate bytes in a
    // local buffer and flush via one sendBinary() call after a brief
    // quiet period — same shape cli_simulator uses, which Connection
    // handles correctly.
    std::vector<uint8_t> data_tx_buffer_;
    uint32_t data_tx_quiet_ms_ = 0;
    static constexpr uint32_t kDataTxFlushQuietMs = 200;
    static constexpr size_t kDataTxFlushSizeBytes = 64 * 1024;
    // Traffic-class boundary (PHY_ADAPTATION_DESIGN §3/§7). A flushed block at or below
    // this size is treated as INTERACTIVE (Winlink-B2F control exchange — banners,
    // proposals, FS answers — and short messages) and sent on the NON-BURST short-LDPC
    // (z=27) SelectiveRepeatARQ path with the ISS/IRS turn gate: low latency, survives by
    // brevity. A larger block is a BULK transfer and pays for the burst + long-LDPC (z=81)
    // + deep-interleave file path. The burst path's group/anchor machinery is wrong for
    // tiny alternating messages (it can't re-acquire burst timing on every turn-flip).
    static constexpr size_t kInteractiveMaxBytes = 4096;
    // The accumulated buffer is flushed as ONE modem file-transfer unit (Z=81 burst path),
    // staged to a temp file. Counter keeps each flush's temp name unique; last path is
    // cleaned up on the next flush / disconnect (its transfer has progressed by then).
    uint64_t tx_file_counter_ = 0;
    std::string last_tx_temp_path_;

    // BULK-ACCUMULATE (env ULTRA_TNC_BULK_ACCUM, prototype): coalesce a flow-
    // controlled Winlink-B2F body into ONE z=81 burst-file instead of letting
    // Pat's VARA flow control trickle it as sub-4KB short-LDPC chunks. Pat
    // self-increments its own flow counter by len(b) per write (conn.go:246)
    // and stalls at 7*blocksize (~1750 B), un-stalling only when WE send a
    // BUFFER command resetting it. So while the body is hoarded in
    // data_tx_buffer_ (engine backlog still 0) we (a) promptly under-report
    // BUFFER to kAbsorbReportCap on every block so Pat keeps feeding, and
    // (b) wait kDataTxBulkQuietMs for Pat to finish dumping before flushing the
    // whole hoard as one burst. The instant the burst hits the engine the
    // backlog jumps to the true size (getTxBacklogBytes counts queued/sending
    // files), so we resume reporting the real draining count and Pat's Flush()
    // — which blocks on BUFFER 0 with a 1-min timeout — still terminates.
    bool bulk_accum_ = false;
    // Once a BULK body has been flushed as a burst-file in this session, keep the
    // TRAILING flushes (the Winlink-B2F FF terminator, any small frame after the
    // body) on the BURST path too — even though they are < kInteractiveMaxBytes.
    // Reason (BUG-TNC-B2F-002): the burst→non-burst transition strands the trailing
    // frame (the receiver, just out of burst RX, mis-decodes a non-burst full-anchor
    // frame). Routing it as a tiny burst reuses the PROVEN burst descriptor+group
    // decode (same path that delivers the body 10/10) — no transition. Reset per
    // connection. Only active under bulk_accum_.
    bool bulk_burst_started_ = false;
    static constexpr int kAbsorbReportCap = 50;
    static constexpr uint32_t kDataTxBulkQuietMs = 1500;
    // Pat's Flush() aborts if it receives no BUFFER command for 60 s (conn.go).
    // The burst-file path can freeze the engine backlog for >60 s during a
    // group-ACK timeout/retransmit; re-emit the current level this often so the
    // flush timer stays alive (a real VARA modem reports buffer state
    // continuously, so this is faithful, not a workaround). 3x margin vs 60 s.
    static constexpr uint32_t kBufferKeepaliveMs = 20000;

    static std::pair<std::string, std::string> parseCommand(std::string_view line);

    void emitOK();
    void emitWrong();
    void emitVersion();
    void emitConnected(const std::string& src, const std::string& dst, int bw);
    void emitDisconnected();
    void emitPtt(bool on);
    void emitBuffer(int bytes);
    void emitSN(float db);
    void emitBitrate(int bps);
    void emitIamalive();
    void emitPending();
    void emitCancelPending();

    void cmdMyCall(std::string_view args);
    void cmdBandwidth(int hz);
    void cmdListen(std::string_view args);
    void cmdConnect(std::string_view args);
    void cmdDisconnect();
    void cmdAbort();
    void cmdCompression(std::string_view args);
    void cmdChat(std::string_view args);
    void cmdCwid(std::string_view args);
    void cmdVersion(std::string_view args);
    void cmdBuffer(std::string_view args);
    void cmdSn(std::string_view args);
    void cmdBitrate(std::string_view args);
    void cmdPublic(std::string_view args);
    void cmdP2P(std::string_view args);
    void cmdWinlink(std::string_view args);
    void cmdIgnoreKissDcd(std::string_view args);
    void cmdIntegerNoop(std::string_view args);
    void cmdStats(std::string_view args);

    void flushDataTxBuffer();
};

} // namespace ultra::tnc
