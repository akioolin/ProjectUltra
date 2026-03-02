#pragma once

#include "connection.hpp"
#include "frame_v2.hpp"
#include <functional>
#include <mutex>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#endif

namespace ultra {
namespace protocol {

#ifdef _WIN32
class ProtocolEngineMutex {
public:
    ProtocolEngineMutex() noexcept = default;
    ProtocolEngineMutex(const ProtocolEngineMutex&) = delete;
    ProtocolEngineMutex& operator=(const ProtocolEngineMutex&) = delete;

    void lock() noexcept { AcquireSRWLockExclusive(&lock_); }
    void unlock() noexcept { ReleaseSRWLockExclusive(&lock_); }

private:
    SRWLOCK lock_ = SRWLOCK_INIT;
};
#else
using ProtocolEngineMutex = std::mutex;
#endif

/**
 * Protocol Engine
 *
 * High-level interface that integrates the protocol stack:
 * - Frame serialization/deserialization (v2 format)
 * - Connection management
 * - ARQ for reliable delivery
 *
 * Connects to ModemEngine for actual TX/RX of audio.
 */
class ProtocolEngine {
public:
    using TxDataCallback = std::function<void(const Bytes& data)>;
    using MessageReceivedCallback = std::function<void(const std::string& from, const std::string& text)>;
    using ConnectionChangedCallback = std::function<void(ConnectionState state, const std::string& remote)>;
    using IncomingCallCallback = std::function<void(const std::string& from)>;

    using FileProgressCallback = Connection::FileProgressCallback;
    using FileReceivedCallback = Connection::FileReceivedCallback;
    using FileSentCallback = Connection::FileSentCallback;

    using ModeNegotiatedCallback = Connection::ModeNegotiatedCallback;
    using ConnectWaveformChangedCallback = Connection::ConnectWaveformChangedCallback;

    explicit ProtocolEngine(const ConnectionConfig& config = ConnectionConfig{});

    // --- Configuration ---

    void setLocalCallsign(const std::string& call);
    std::string getLocalCallsign() const;

    void setAutoAccept(bool auto_accept);
    bool getAutoAccept() const;

    // --- Callbacks ---

    void setTxDataCallback(TxDataCallback cb);
    void setMessageReceivedCallback(MessageReceivedCallback cb);
    void setConnectionChangedCallback(ConnectionChangedCallback cb);
    void setIncomingCallCallback(IncomingCallCallback cb);

    // --- Connection Control ---

    bool connect(const std::string& remote_call);
    void acceptCall();
    void rejectCall();
    void disconnect();
    void abortTxNow();

    // --- Data Transfer ---

    bool sendMessage(const std::string& text);
    bool sendMessages(const std::vector<std::string>& texts);  // Batch: burst-interleaved
    bool isReadyToSend() const;

    // --- File Transfer ---

    bool sendFile(const std::string& filepath);
    void setReceiveDirectory(const std::string& dir);
    void cancelFileTransfer();
    bool isFileTransferInProgress() const;
    FileTransferProgress getFileProgress() const;

    void setFileProgressCallback(FileProgressCallback cb);
    void setFileReceivedCallback(FileReceivedCallback cb);
    void setFileSentCallback(FileSentCallback cb);

    // --- Modem Interface ---

    void onRxData(const Bytes& data);
    void tick(uint32_t elapsed_ms);

    // --- State ---

    ConnectionState getState() const;
    std::string getRemoteCallsign() const;
    bool isConnected() const;

    ConnectionStats getStats() const;
    void resetStats();
    void reset();

    // --- Waveform Mode ---

    WaveformMode getNegotiatedMode() const;
    void setPreferredMode(WaveformMode mode);
    void setModeCapabilities(uint8_t caps);

    // Session-scoped narrowband override (cleared on disconnect/reset)
    void setNarrowbandOverride(WaveformMode mode);

    // Forced data mode - operator can override SNR-based selection
    // Set before calling connect(). Values are sent in CONNECT frame.
    // 0xFF (AUTO) = let responder decide based on SNR
    void setForcedModulation(Modulation mod);
    void setForcedCodeRate(CodeRate rate);
    Modulation getForcedModulation() const;
    CodeRate getForcedCodeRate() const;

    void setModeNegotiatedCallback(ModeNegotiatedCallback cb);
    void setConnectWaveformChangedCallback(ConnectWaveformChangedCallback cb);
    WaveformMode getConnectWaveform() const;
    void setInitialConnectWaveform(WaveformMode mode);

    // Callback when handshake is confirmed (safe to switch to negotiated waveform)
    using HandshakeConfirmedCallback = Connection::HandshakeConfirmedCallback;
    void setHandshakeConfirmedCallback(HandshakeConfirmedCallback cb);

    // --- Adaptive Data Mode ---

    // Set measured SNR from modem (used for adaptive mode selection)
    void setMeasuredSNR(float snr_db);
    float getMeasuredSNR() const;

    // Set channel quality including fading detection
    void setChannelQuality(float snr_db, float fading_index);
    float getFadingIndex() const;

    // Get current data mode
    Modulation getDataModulation() const;
    CodeRate getDataCodeRate() const;

    // Set callback for data mode changes
    using DataModeChangedCallback = Connection::DataModeChangedCallback;
    void setDataModeChangedCallback(DataModeChangedCallback cb);

    // --- Ping/Pong (fast presence check) ---

    // Callback when protocol wants to send a ping (modem transmits raw "ULTR")
    // Burst mode TX callback - transmits multiple frames as single audio burst (OFDM only)
    using TransmitBurstCallback = Connection::TransmitBurstCallback;
    void setTransmitBurstCallback(TransmitBurstCallback cb);

    using PingTxCallback = Connection::PingTxCallback;
    void setPingTxCallback(PingTxCallback cb);

    // Callback when we receive an incoming ping while disconnected (someone's calling)
    using PingReceivedCallback = Connection::PingReceivedCallback;
    void setPingReceivedCallback(PingReceivedCallback cb);

    // Call when modem detects "ULTR" magic (either PONG to our PING, or incoming PING)
    void onPingReceived();

private:
    Connection connection_;

    TxDataCallback on_tx_data_;
    MessageReceivedCallback on_message_received_;
    ConnectionChangedCallback on_connection_changed_;
    IncomingCallCallback on_incoming_call_;

    mutable ProtocolEngineMutex mutex_;

    Bytes rx_buffer_;

    std::vector<Bytes> tx_queue_;
    bool defer_tx_ = false;

    void handleTxFrame(const Bytes& frame_data);
    void processRxBuffer();
};

} // namespace protocol
} // namespace ultra
