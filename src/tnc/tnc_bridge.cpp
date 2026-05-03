#include "tnc_bridge.hpp"

#include "gui/audio_engine.hpp"
#include "protocol/frame_v2.hpp"
#include "tnc_server.hpp"
#include "ultra/types.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ultra::tnc {

namespace {

class RealProtocolEnginePort final : public ProtocolEnginePort {
public:
    explicit RealProtocolEnginePort(protocol::ProtocolEngine& engine)
        : engine_(engine) {}

    void setLocalCallsign(const std::string& call) override {
        engine_.setLocalCallsign(call);
    }

    std::string getLocalCallsign() const override {
        return engine_.getLocalCallsign();
    }

    void setAutoAccept(bool auto_accept) override {
        engine_.setAutoAccept(auto_accept);
    }

    bool connect(const std::string& remote_call) override {
        return engine_.connect(remote_call);
    }

    void disconnect() override {
        engine_.disconnect();
    }

    void abortTxNow() override {
        engine_.abortTxNow();
    }

    bool sendBinary(const ultra::Bytes& data) override {
        return engine_.sendBinary(data);
    }

    size_t getTxBacklogBytes() const override {
        return engine_.getTxBacklogBytes();
    }

    protocol::ConnectionState getState() const override {
        return engine_.getState();
    }

    std::string getRemoteCallsign() const override {
        return engine_.getRemoteCallsign();
    }

    float getMeasuredSNR() const override {
        return engine_.getMeasuredSNR();
    }

    protocol::WaveformMode getNegotiatedMode() const override {
        return engine_.getNegotiatedMode();
    }

    void setPreferredMode(protocol::WaveformMode mode) override {
        engine_.setPreferredMode(mode);
    }

    protocol::ConnectionStats getStats() const override {
        return engine_.getStats();
    }

    Modulation getDataModulation() const override {
        return engine_.getDataModulation();
    }

    CodeRate getDataCodeRate() const override {
        return engine_.getDataCodeRate();
    }

    void setConnectionChangedCallback(ConnectionChangedCallback cb) override {
        engine_.setConnectionChangedCallback(std::move(cb));
    }

    void setIncomingCallCallback(IncomingCallCallback cb) override {
        engine_.setIncomingCallCallback(std::move(cb));
    }

    void setDataReceivedCallback(DataReceivedCallback cb) override {
        engine_.setDataReceivedCallback(std::move(cb));
    }

private:
    protocol::ProtocolEngine& engine_;
};

class TNCServerEventSink final : public TNCBridgeEventSink {
public:
    explicit TNCServerEventSink(TNCServer& server)
        : server_(server) {}

    void postModemConnected(const std::string& src, const std::string& dst, int bw) override {
        server_.postModemConnected(src, dst, bw);
    }

    void postModemDisconnected() override {
        server_.postModemDisconnected();
    }

    void postModemPTT(bool on) override {
        server_.postModemPTT(on);
    }

    void postModemDataReceived(std::vector<uint8_t> bytes) override {
        server_.postModemDataReceived(std::move(bytes));
    }

    void postModemBufferLevel(int bytes) override {
        server_.postModemBufferLevel(bytes);
    }

    void postModemSNR(float db) override {
        server_.postModemSNR(db);
    }

    void postModemBitrate(int bps) override {
        server_.postModemBitrate(bps);
    }

    void postModemIncomingCall(std::string peer) override {
        server_.postModemIncomingCall(std::move(peer));
    }

private:
    TNCServer& server_;
};

int clampSizeToInt(size_t value) {
    if (value > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(value);
}

} // namespace

TNCBridge::TNCBridge(protocol::ProtocolEngine& engine, gui::AudioEngine& audio)
    : owned_engine_(std::make_unique<RealProtocolEnginePort>(engine)),
      engine_(*owned_engine_),
      audio_(audio) {}

TNCBridge::TNCBridge(ProtocolEnginePort& engine, gui::AudioEngine& audio)
    : engine_(engine),
      audio_(audio) {}

TNCBridge::~TNCBridge() {
    stop();
}

void TNCBridge::attachServer(TNCServer* server) {
    std::shared_ptr<TNCBridgeEventSink> next;
    if (server) {
        next = std::make_shared<TNCServerEventSink>(*server);
    }
    std::lock_guard<std::mutex> lock(event_sink_mutex_);
    event_sink_ = std::move(next);
}

void TNCBridge::attachEventSink(TNCBridgeEventSink* sink) {
    // External ownership: wrap in shared_ptr with a no-op deleter so
    // the bridge doesn't destroy state it doesn't own. Concurrent
    // readers that copied the previous shared_ptr keep that sink
    // alive (or the no-op-deleter wrapper) until they're done.
    std::shared_ptr<TNCBridgeEventSink> next;
    if (sink) {
        next = std::shared_ptr<TNCBridgeEventSink>(sink, [](TNCBridgeEventSink*) {});
    }
    std::lock_guard<std::mutex> lock(event_sink_mutex_);
    event_sink_ = std::move(next);
}

std::shared_ptr<TNCBridgeEventSink> TNCBridge::snapshotEventSink() const {
    std::lock_guard<std::mutex> lock(event_sink_mutex_);
    return event_sink_;
}

void TNCBridge::setConnectionChangedCallback(ConnectionChangedCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    connection_changed_cb_ = std::move(cb);
}

void TNCBridge::setPreferredWaveformChangedCallback(PreferredWaveformChangedCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    preferred_waveform_changed_cb_ = std::move(cb);
}

void TNCBridge::setPttChangedCallback(PttChangedCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    ptt_changed_cb_ = std::move(cb);
}

void TNCBridge::setMyCall(const std::vector<std::string>& calls) {
    if (calls.empty()) {
        return;
    }

    const std::string primary = protocol::sanitizeCallsign(calls.front());
    if (primary.empty()) {
        return;
    }

    engine_.setLocalCallsign(primary);

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        local_call_ = primary;
    }
    state_.store(State::READY, std::memory_order_release);
}

void TNCBridge::setBandwidth(int hz) {
    const protocol::WaveformMode mode = waveformForBandwidth(hz);

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        requested_bw_ = hz;
    }

    engine_.setPreferredMode(mode);

    PreferredWaveformChangedCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = preferred_waveform_changed_cb_;
    }
    if (cb) {
        cb(mode);
    }
}

void TNCBridge::setListen(bool on) {
    engine_.setAutoAccept(on);
    state_.store(on ? State::LISTENING : State::READY, std::memory_order_release);
}

void TNCBridge::startConnect(const std::string& src, const std::string& dst) {
    const std::string local = protocol::sanitizeCallsign(src);
    const std::string remote = protocol::sanitizeCallsign(dst);
    if (local.empty() || remote.empty()) {
        return;
    }

    engine_.setLocalCallsign(local);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        local_call_ = local;
        remote_call_ = remote;
    }

    state_.store(State::CONNECTING, std::memory_order_release);
    if (!engine_.connect(remote)) {
        state_.store(State::READY, std::memory_order_release);
        if (auto sink = snapshotEventSink()) {
            sink->postModemDisconnected();
        }
    }
}

void TNCBridge::disconnect() {
    state_.store(State::DISCONNECTING, std::memory_order_release);
    engine_.disconnect();
}

void TNCBridge::abort() {
    engine_.abortTxNow();
    audio_.clearTxQueue();
    {
        std::lock_guard<std::mutex> lock(ptt_mutex_);
        postPTT(false);
        ptt_tail_ms_ = 0;
    }
    state_.store(State::READY, std::memory_order_release);
    engine_.disconnect();
}

bool TNCBridge::sendBinary(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        // Nothing to send is a no-op success — the caller has nothing
        // to retry. Distinguishes from engine refusal of real bytes.
        return true;
    }
    return engine_.sendBinary(bytes);
}

int TNCBridge::getTxBackloggBytes() const {
    return getTxBacklogBytes();
}

int TNCBridge::getTxBacklogBytes() const {
    return clampSizeToInt(engine_.getTxBacklogBytes());
}

int TNCBridge::getCurrentSNR_db() const {
    const float snr = engine_.getMeasuredSNR();
    if (!std::isfinite(snr)) {
        return 0;
    }
    return static_cast<int>(std::lround(snr));
}

int TNCBridge::getCurrentBitrate_bps() const {
    return bitrateEstimate(engine_.getNegotiatedMode());
}

State TNCBridge::getState() const {
    return state_.load(std::memory_order_acquire);
}

ModemStats TNCBridge::getStats() const {
    ModemStats out;
    const protocol::ConnectionStats stats = engine_.getStats();
    out.frames_sent = stats.arq.frames_sent;
    out.frames_received = stats.arq.frames_received;
    out.retransmissions = stats.arq.retransmissions;
    out.timeouts = stats.arq.timeouts;
    out.failed = stats.arq.failed;
    out.out_of_order = stats.arq.out_of_order;
    out.code_rate = codeRateToString(engine_.getDataCodeRate());
    out.modulation = modulationToString(engine_.getDataModulation());
    out.waveform = protocol::waveformModeToString(engine_.getNegotiatedMode());
    out.snr_db = getCurrentSNR_db();
    out.bitrate_bps = getCurrentBitrate_bps();
    out.tx_backlog_bytes = getTxBacklogBytes();
    return out;
}

void TNCBridge::start() {
    if (started_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        local_call_ = engine_.getLocalCallsign();
        remote_call_ = engine_.getRemoteCallsign();
    }
    state_.store(toTNCState(engine_.getState()), std::memory_order_release);
    wirePECallbacks();
    tick(0);
}

void TNCBridge::stop() {
    if (!started_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    clearPECallbacks();
    {
        std::lock_guard<std::mutex> lock(ptt_mutex_);
        postPTT(false);
        ptt_tail_ms_ = 0;
    }
}

void TNCBridge::tick(uint32_t elapsed_ms) {
    if (!started_.load(std::memory_order_acquire)) {
        return;
    }
    onAudioQueueState(!audio_.isTxQueueEmpty(), elapsed_ms);

    // Pat / Winlink need periodic BUFFER N updates to know when their
    // outbound data has finished transmitting. Poll the engine's TX
    // backlog and forward any change. TNCSession rate-limits non-zero
    // BUFFER N emissions to once per kBufferEmitIntervalMs (1 s) so a
    // long send doesn't flood the cmd port; BUFFER 0 transitions
    // bypass the rate limit so Pat's Flush() unblocks immediately.
    const int backlog = clampSizeToInt(engine_.getTxBacklogBytes());
    if (backlog != last_reported_backlog_) {
        last_reported_backlog_ = backlog;
        if (auto sink = snapshotEventSink()) {
            sink->postModemBufferLevel(backlog);
        }
    }
}

void TNCBridge::wirePECallbacks() {
    engine_.setConnectionChangedCallback([this](protocol::ConnectionState state, const std::string& info) {
        onConnectionChanged(state, info);
    });
    engine_.setDataReceivedCallback([this](const ultra::Bytes& bytes, bool more_data) {
        onDataReceived(bytes, more_data);
    });
    engine_.setIncomingCallCallback([this](const std::string& peer) {
        onIncomingCall(peer);
    });
}

void TNCBridge::clearPECallbacks() {
    engine_.setConnectionChangedCallback({});
    engine_.setDataReceivedCallback({});
    engine_.setIncomingCallCallback({});
}

void TNCBridge::onConnectionChanged(protocol::ConnectionState state, const std::string& info) {
    const State tnc_state = toTNCState(state);
    const State previous = state_.exchange(tnc_state, std::memory_order_acq_rel);

    std::string local;
    std::string remote;
    int bw = 2300;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        local = local_call_;
        if (state == protocol::ConnectionState::CONNECTED) {
            remote_call_ = info;
        } else if (state == protocol::ConnectionState::PROBING ||
                   state == protocol::ConnectionState::CONNECTING) {
            remote_call_ = info;
        }
        remote = remote_call_;
        bw = requested_bw_;
        if (state == protocol::ConnectionState::DISCONNECTED) {
            inbound_pending_ = false;
        }
    }
    // Inbound = we didn't initiate. Detect this from the prior TNC state:
    // a CONNECTING previous means we issued startConnect (outbound). Anything
    // else (LISTENING / READY / IDLE) is the peer dialing us. The
    // on_incoming_call_ callback only fires when auto_accept is OFF, but
    // pat-vara always sets auto_accept=true and fields the inbound case
    // via the CONNECTED line itself, so we can't rely on that signal.
    const bool inbound = (previous != State::CONNECTING);

    if (state == protocol::ConnectionState::CONNECTED && previous != State::CONNECTED) {
        if (remote.empty()) {
            remote = info;
        }
        // CONNECTED line must always be "<initiator> <responder> <bw>".
        // When the peer called us (inbound), they are the initiator.
        // Otherwise we initiated, so we are first.
        const std::string& initiator = inbound ? remote : local;
        const std::string& responder = inbound ? local : remote;
        if (auto sink = snapshotEventSink()) {
            sink->postModemConnected(initiator, responder, bw);
            sink->postModemBitrate(bitrateEstimate(waveformForBandwidth(bw)));
        }
    } else if (state == protocol::ConnectionState::DISCONNECTED && previous != State::IDLE &&
               previous != State::READY && previous != State::LISTENING) {
        if (auto sink = snapshotEventSink()) {
            sink->postModemDisconnected();
        }
    }

    ConnectionChangedCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = connection_changed_cb_;
    }
    if (cb) {
        cb(state, info);
    }
}

void TNCBridge::onDataReceived(const ultra::Bytes& bytes, bool more_data) {
    (void)more_data;
    if (bytes.empty()) {
        return;
    }

    if (auto sink = snapshotEventSink()) {
        sink->postModemDataReceived(bytes);
    }
}

void TNCBridge::onIncomingCall(const std::string& peer) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        remote_call_ = peer;
        inbound_pending_ = true;
    }

    if (auto sink = snapshotEventSink()) {
        sink->postModemIncomingCall(peer);
    }
}

void TNCBridge::onAudioQueueState(bool active, uint32_t elapsed_ms) {
    // Compute the PTT transition decision under the tail-tracking mutex,
    // then release before invoking postPTT() — postPTT calls user
    // callbacks (hardware PTT serial I/O), which could be slow or
    // re-enter bridge state and deadlock if held under ptt_mutex_.
    enum class Action { None, On, Off };
    Action action = Action::None;
    {
        std::lock_guard<std::mutex> lock(ptt_mutex_);
        if (active) {
            ptt_tail_ms_ = 0;
            action = Action::On;
        } else if (!ptt_active_.load(std::memory_order_acquire)) {
            ptt_tail_ms_ = 0;
        } else {
            ptt_tail_ms_ = std::min<uint32_t>(ptt_tail_ms_ + elapsed_ms, kPttTailMs);
            if (ptt_tail_ms_ >= kPttTailMs) {
                ptt_tail_ms_ = 0;
                action = Action::Off;
            }
        }
    }
    if (action == Action::On) {
        postPTT(true);
    } else if (action == Action::Off) {
        postPTT(false);
    }
}

protocol::WaveformMode TNCBridge::waveformForBandwidth(int hz) {
    if (hz == 500) {
        return protocol::WaveformMode::OFDM_NARROW;
    }
    return protocol::WaveformMode::OFDM_CHIRP;
}

int TNCBridge::bitrateEstimate(protocol::WaveformMode mode) {
    switch (mode) {
    case protocol::WaveformMode::OFDM_NARROW:
        return 230;
    case protocol::WaveformMode::MC_DPSK:
        return 938;
    case protocol::WaveformMode::OFDM_CHIRP:
        return 2300;
    case protocol::WaveformMode::OFDM_COX:
        return 4000;
    case protocol::WaveformMode::OTFS_EQ:
    case protocol::WaveformMode::OTFS_RAW:
    case protocol::WaveformMode::MFSK:
    case protocol::WaveformMode::AUTO:
        return 0;
    }
    return 0;
}

State TNCBridge::toTNCState(protocol::ConnectionState state) {
    switch (state) {
    case protocol::ConnectionState::DISCONNECTED:
        return State::READY;
    case protocol::ConnectionState::PROBING:
    case protocol::ConnectionState::CONNECTING:
        return State::CONNECTING;
    case protocol::ConnectionState::CONNECTED:
        return State::CONNECTED;
    case protocol::ConnectionState::DISCONNECTING:
        return State::DISCONNECTING;
    }
    return State::IDLE;
}

void TNCBridge::postPTT(bool on) {
    const bool previous = ptt_active_.exchange(on, std::memory_order_acq_rel);
    if (previous == on) {
        return;
    }

    if (auto sink = snapshotEventSink()) {
        sink->postModemPTT(on);
    }

    PttChangedCallback ptt_cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        ptt_cb = ptt_changed_cb_;
    }
    if (ptt_cb) {
        ptt_cb(on);
    }
}

} // namespace ultra::tnc
