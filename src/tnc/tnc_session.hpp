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
