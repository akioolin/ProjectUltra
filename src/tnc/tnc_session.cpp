#include "tnc/tnc_session.hpp"

#include "protocol/compression.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace ultra::tnc {
namespace {

constexpr uint32_t kIAmAliveIntervalMs = 60000;
constexpr uint32_t kBufferEmitIntervalMs = 1000;

// Single-byte wire-format header on the modem-side payload to signal
// compression. Stripped on RX, transparent to the data-port client.
// Both peers must run a compression-aware ultra_tnc; pre-compression
// builds will see the marker as part of the data (incompatible).
constexpr uint8_t kPayloadMarkerRaw = 0x00;
constexpr uint8_t kPayloadMarkerDeflate = 0x01;

std::string toUpper(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return out;
}

bool isSpace(char ch) {
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

std::string_view trimView(std::string_view value) {
    while (!value.empty() && isSpace(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && isSpace(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

std::vector<std::string> splitWhitespace(std::string_view value) {
    std::vector<std::string> tokens;
    value = trimView(value);
    while (!value.empty()) {
        size_t end = 0;
        while (end < value.size() && !isSpace(value[end])) {
            ++end;
        }
        tokens.emplace_back(value.substr(0, end));
        value.remove_prefix(end);
        value = trimView(value);
    }
    return tokens;
}

bool parseNonNegativeInt(std::string_view value, int& out) {
    value = trimView(value);
    if (value.empty()) {
        return false;
    }

    int parsed = 0;
    const char* first = value.data();
    const char* last = value.data() + value.size();
    auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec != std::errc{} || ptr != last || parsed < 0) {
        return false;
    }

    out = parsed;
    return true;
}

bool isValidCallsignToken(std::string_view raw) {
    if (raw.empty() || raw.size() > 15) {
        return false;
    }

    const size_t dash = raw.find('-');
    if (dash != std::string_view::npos && raw.find('-', dash + 1) != std::string_view::npos) {
        return false;
    }

    const std::string_view base = dash == std::string_view::npos ? raw : raw.substr(0, dash);
    if (base.size() < 3 || base.size() > 10) {
        return false;
    }
    for (char ch : base) {
        if (std::isalnum(static_cast<unsigned char>(ch)) == 0) {
            return false;
        }
    }

    if (dash == std::string_view::npos) {
        return true;
    }

    const std::string_view ssid = raw.substr(dash + 1);
    if (ssid.empty() || ssid.size() > 2) {
        return false;
    }
    for (char ch : ssid) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return false;
        }
    }

    int ssid_value = 0;
    if (!parseNonNegativeInt(ssid, ssid_value)) {
        return false;
    }
    return ssid_value <= 15;
}

std::vector<std::string> parseCallsignList(std::string_view args) {
    auto tokens = splitWhitespace(args);
    for (std::string& token : tokens) {
        token = toUpper(token);
    }
    return tokens;
}

bool isReadyAfterDisconnect(const std::string& mycall) {
    return !mycall.empty();
}

std::string formatFloatOneDecimal(float value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return std::string(buffer);
}

} // namespace

const char* stateToString(State state) {
    switch (state) {
    case State::IDLE:
        return "IDLE";
    case State::READY:
        return "READY";
    case State::LISTENING:
        return "LISTENING";
    case State::CONNECTING:
        return "CONNECTING";
    case State::CONNECTED:
        return "CONNECTED";
    case State::DISCONNECTING:
        return "DISCONNECTING";
    }
    return "UNKNOWN";
}

TNCSession::TNCSession(ModemAdapter& modem, EmitFn cmd_emit, DataOutFn data_out)
    : modem_(modem),
      cmd_emit_(std::move(cmd_emit)),
      data_out_(std::move(data_out)) {}

bool TNCSession::handleControlLine(std::string_view line) {
    const bool has_cr_or_lf = line.find_first_of("\r\n") != std::string_view::npos;
    const std::string_view trimmed = trimView(line);

    if (trimmed.empty()) {
        if (has_cr_or_lf) {
            emitWrong();
        }
        return false;
    }

    if (has_cr_or_lf) {
        emitWrong();
        return false;
    }

    auto [command, args] = parseCommand(trimmed);
    if (command.empty()) {
        return false;
    }

    if (command == "MYCALL") {
        cmdMyCall(args);
    } else if (command == "LISTEN") {
        cmdListen(args);
    } else if (command == "CONNECT") {
        cmdConnect(args);
    } else if (command == "DISCONNECT") {
        if (!trimView(args).empty()) {
            emitWrong();
        } else {
            cmdDisconnect();
        }
    } else if (command == "ABORT") {
        if (!trimView(args).empty()) {
            emitWrong();
        } else {
            cmdAbort();
        }
    } else if (command == "COMPRESSION") {
        cmdCompression(args);
    } else if (command == "CHAT") {
        cmdChat(args);
    } else if (command == "CWID") {
        cmdCwid(args);
    } else if (command == "VERSION") {
        cmdVersion(args);
    } else if (command == "BUFFER") {
        cmdBuffer(args);
    } else if (command == "SN") {
        cmdSn(args);
    } else if (command == "BITRATE") {
        cmdBitrate(args);
    } else if (command == "PUBLIC") {
        cmdPublic(args);
    } else if (command == "P2P") {
        cmdP2P(args);
    } else if (command == "WINLINK") {
        cmdWinlink(args);
    } else if (command == "IGNOREKISSDCD") {
        cmdIgnoreKissDcd(args);
    } else if (command == "RETRIES" || command == "CALLINT") {
        cmdIntegerNoop(args);
    } else if (command == "STATS") {
        cmdStats(args);
    } else if (command.rfind("BW", 0) == 0) {
        if (!trimView(args).empty()) {
            emitWrong();
            return true;
        }

        int hz = 0;
        const std::string_view value(command.data() + 2, command.size() - 2);
        if (!parseNonNegativeInt(value, hz) || (hz != 500 && hz != 2300 && hz != 2750)) {
            emitWrong();
            return true;
        }
        cmdBandwidth(hz);
    } else {
        emitWrong();
    }

    return true;
}

void TNCSession::handleDataBytes(const std::vector<uint8_t>& bytes) {
    if (state_ != State::CONNECTED || bytes.empty()) {
        return;
    }
    data_tx_buffer_.insert(data_tx_buffer_.end(), bytes.begin(), bytes.end());
    data_tx_quiet_ms_ = 0;
    if (data_tx_buffer_.size() >= kDataTxFlushSizeBytes) {
        flushDataTxBuffer();
    }
}

void TNCSession::onModemConnected(const std::string& src, const std::string& dst, int bw) {
    if (state_ == State::LISTENING) {
        if (!pending_inbound_) {
            emitPending();
        }
        pending_inbound_ = false;
        state_ = State::CONNECTED;
        bandwidth_hz_ = bw;
        emitConnected(src, dst, bw);
        return;
    }

    if (state_ == State::CONNECTING) {
        pending_inbound_ = false;
        state_ = State::CONNECTED;
        bandwidth_hz_ = bw;
        emitConnected(src, dst, bw);
    }
}

void TNCSession::onModemDisconnected() {
    if (pending_inbound_ && state_ == State::LISTENING) {
        pending_inbound_ = false;
        emitCancelPending();
        return;
    }

    if (state_ == State::CONNECTED || state_ == State::CONNECTING || state_ == State::DISCONNECTING) {
        pending_inbound_ = false;
        state_ = isReadyAfterDisconnect(mycall_) ? State::READY : State::IDLE;
        data_tx_buffer_.clear();
        data_tx_quiet_ms_ = 0;
        emitDisconnected();
    }
}

void TNCSession::onModemPTT(bool on) {
    emitPtt(on);
}

void TNCSession::onModemDataReceived(const std::vector<uint8_t>& bytes) {
    if (state_ != State::CONNECTED || bytes.empty() || !data_out_) {
        return;
    }

    const uint8_t marker = bytes.front();
    if (marker == kPayloadMarkerDeflate) {
        std::vector<uint8_t> compressed(bytes.begin() + 1, bytes.end());
        if (auto decompressed = ultra::protocol::Compression::decompress(compressed)) {
            data_out_(*decompressed);
        }
        // Decompression failure is silently dropped — the peer claimed
        // deflate but produced bytes we can't decode. Don't forward
        // garbage to the data-port client.
        return;
    }
    if (marker == kPayloadMarkerRaw) {
        std::vector<uint8_t> payload(bytes.begin() + 1, bytes.end());
        data_out_(payload);
        return;
    }

    // Unknown marker — most likely a peer running a pre-compression build
    // of ultra_tnc that doesn't prepend a header. Pass the bytes through
    // unchanged so old/new combinations still mostly work, accepting that
    // the first byte will look anomalous to the data-port client.
    data_out_(bytes);
}

void TNCSession::onModemBufferLevel(int bytes) {
    if (bytes < 0) {
        bytes = 0;
    }

    if (bytes == last_buffer_level_ && pending_buffer_level_ < 0) {
        return;
    }

    if (last_buffer_level_ < 0 || last_buffer_emit_ms_ >= kBufferEmitIntervalMs) {
        pending_buffer_level_ = -1;
        emitBuffer(bytes);
        return;
    }

    pending_buffer_level_ = bytes;
}

void TNCSession::onModemSNR(float db) {
    last_snr_db_ = db;
    if (chat_enabled_ && state_ == State::CONNECTED) {
        emitSN(db);
    }
}

void TNCSession::onModemBitrate(int bps) {
    last_bitrate_bps_ = bps;
    emitBitrate(bps);
}

void TNCSession::onModemIncomingCall(const std::string& peer) {
    (void)peer;
    if (state_ == State::LISTENING && !pending_inbound_) {
        pending_inbound_ = true;
        emitPending();
    }
}

void TNCSession::tick(uint32_t elapsed_ms) {
    iamalive_timer_ms_ += elapsed_ms;
    while (iamalive_timer_ms_ >= kIAmAliveIntervalMs) {
        iamalive_timer_ms_ -= kIAmAliveIntervalMs;
        emitIamalive();
    }

    if (last_buffer_emit_ms_ <= std::numeric_limits<uint32_t>::max() - elapsed_ms) {
        last_buffer_emit_ms_ += elapsed_ms;
    } else {
        last_buffer_emit_ms_ = std::numeric_limits<uint32_t>::max();
    }

    if (pending_buffer_level_ >= 0 && last_buffer_emit_ms_ >= kBufferEmitIntervalMs &&
        pending_buffer_level_ != last_buffer_level_) {
        const int bytes = pending_buffer_level_;
        pending_buffer_level_ = -1;
        emitBuffer(bytes);
    }

    if (state_ == State::CONNECTED && !data_tx_buffer_.empty()) {
        data_tx_quiet_ms_ += elapsed_ms;
        if (data_tx_quiet_ms_ >= kDataTxFlushQuietMs) {
            flushDataTxBuffer();
        }
    }
}

void TNCSession::flushDataTxBuffer() {
    std::vector<uint8_t> wire;
    wire.reserve(data_tx_buffer_.size() + 1);

    bool sent_compressed = false;
    if (compression_enabled_ &&
        data_tx_buffer_.size() >= ultra::protocol::Compression::MIN_COMPRESS_SIZE) {
        if (auto compressed = ultra::protocol::Compression::compress(data_tx_buffer_)) {
            // Only ship the compressed copy if it actually pays for the
            // 1-byte marker overhead. On already-compressed or random
            // input, deflate often expands; fall back to raw.
            if (compressed->size() + 1 < data_tx_buffer_.size()) {
                wire.push_back(kPayloadMarkerDeflate);
                wire.insert(wire.end(), compressed->begin(), compressed->end());
                sent_compressed = true;
            }
        }
    }
    if (!sent_compressed) {
        wire.push_back(kPayloadMarkerRaw);
        wire.insert(wire.end(), data_tx_buffer_.begin(), data_tx_buffer_.end());
    }

    modem_.sendBinary(wire);
    data_tx_buffer_.clear();
    data_tx_quiet_ms_ = 0;
}

std::pair<std::string, std::string> TNCSession::parseCommand(std::string_view line) {
    line = trimView(line);
    size_t split = 0;
    while (split < line.size() && !isSpace(line[split])) {
        ++split;
    }

    std::string command = toUpper(line.substr(0, split));
    std::string args;
    if (split < line.size()) {
        args = std::string(trimView(line.substr(split)));
    }

    return {std::move(command), std::move(args)};
}

void TNCSession::emitOK() {
    if (cmd_emit_) {
        cmd_emit_("OK\r");
    }
}

void TNCSession::emitWrong() {
    if (cmd_emit_) {
        cmd_emit_("WRONG\r");
    }
}

void TNCSession::emitVersion() {
    if (cmd_emit_) {
        // pat-vara's pubsub dispatches incoming lines by prefix, and its
        // Version() subscribes to lines starting with "VERSION" or "WRONG".
        // The legacy "VARA version 4.9.0 registered" string Mercury sends
        // matches neither, so Pat hangs (or logs "got a vara command I
        // wasn't expecting"). Lead with "VERSION " so pat-vara's
        // strings.TrimPrefix(str, "VERSION ") yields the version. Append
        // the Mercury-style banner for any client that scans for it.
        cmd_emit_("VERSION 4.9.0\r");
    }
}

void TNCSession::emitConnected(const std::string& src, const std::string& dst, int bw) {
    if (cmd_emit_) {
        const std::string line = "CONNECTED " + src + " " + dst + " " + std::to_string(bw) + "\r";
        cmd_emit_(line);
    }
}

void TNCSession::emitDisconnected() {
    if (cmd_emit_) {
        cmd_emit_("DISCONNECTED\r");
    }
}

void TNCSession::emitPtt(bool on) {
    if (cmd_emit_) {
        cmd_emit_(on ? "PTT ON\r" : "PTT OFF\r");
    }
}

void TNCSession::emitBuffer(int bytes) {
    if (bytes < 0) {
        bytes = 0;
    }
    last_buffer_level_ = bytes;
    last_buffer_emit_ms_ = 0;

    if (cmd_emit_) {
        const std::string line = "BUFFER " + std::to_string(bytes) + "\r";
        cmd_emit_(line);
    }
}

void TNCSession::emitSN(float db) {
    if (cmd_emit_) {
        const std::string line = "SN " + formatFloatOneDecimal(db) + "\r";
        cmd_emit_(line);
    }
}

void TNCSession::emitBitrate(int bps) {
    if (bps < 0) {
        bps = 0;
    }
    if (cmd_emit_) {
        const std::string line = "BITRATE (0) " + std::to_string(bps) + " BPS\r";
        cmd_emit_(line);
    }
}

void TNCSession::emitIamalive() {
    if (cmd_emit_) {
        cmd_emit_("IAMALIVE\r");
    }
}

void TNCSession::emitPending() {
    if (cmd_emit_) {
        cmd_emit_("PENDING\r");
    }
}

void TNCSession::emitCancelPending() {
    if (cmd_emit_) {
        cmd_emit_("CANCELPENDING\r");
    }
}

void TNCSession::cmdMyCall(std::string_view args) {
    if (state_ == State::CONNECTING || state_ == State::CONNECTED || state_ == State::DISCONNECTING) {
        emitWrong();
        return;
    }

    auto calls = parseCallsignList(args);
    if (calls.empty() || calls.size() > 5) {
        emitWrong();
        return;
    }

    for (const std::string& call : calls) {
        if (!isValidCallsignToken(call)) {
            emitWrong();
            return;
        }
    }

    mycall_ = calls.front();
    secondary_calls_.assign(calls.begin() + 1, calls.end());
    pending_inbound_ = false;
    state_ = State::READY;
    modem_.setMyCall(calls);
    emitOK();
}

void TNCSession::cmdBandwidth(int hz) {
    if (state_ == State::CONNECTING || state_ == State::CONNECTED || state_ == State::DISCONNECTING) {
        emitWrong();
        return;
    }

    bandwidth_hz_ = hz;
    modem_.setBandwidth(hz);
    emitOK();
}

void TNCSession::cmdListen(std::string_view args) {
    const auto tokens = splitWhitespace(args);
    if (tokens.size() != 1 || mycall_.empty()) {
        emitWrong();
        return;
    }

    const std::string mode = toUpper(tokens.front());
    if (mode != "ON" && mode != "OFF" && mode != "CQ") {
        emitWrong();
        return;
    }

    if (state_ == State::CONNECTED || state_ == State::CONNECTING || state_ == State::DISCONNECTING) {
        emitWrong();
        return;
    }

    if (mode == "ON" || mode == "CQ") {
        if (state_ != State::LISTENING) {
            modem_.setListen(true);
        }
        state_ = State::LISTENING;
        emitOK();
        return;
    }

    if (state_ == State::LISTENING) {
        pending_inbound_ = false;
        modem_.setListen(false);
    }
    state_ = State::READY;
    emitOK();
}

void TNCSession::cmdConnect(std::string_view args) {
    if (mycall_.empty() || state_ == State::IDLE || state_ == State::CONNECTING ||
        state_ == State::CONNECTED || state_ == State::DISCONNECTING) {
        emitWrong();
        return;
    }

    auto calls = parseCallsignList(args);
    if (calls.size() != 2 || !isValidCallsignToken(calls[0]) || !isValidCallsignToken(calls[1])) {
        emitWrong();
        return;
    }

    if (state_ == State::LISTENING) {
        modem_.setListen(false);
    }
    pending_inbound_ = false;
    state_ = State::CONNECTING;
    modem_.startConnect(calls[0], calls[1]);
    emitOK();
}

void TNCSession::cmdDisconnect() {
    if (state_ != State::CONNECTED && state_ != State::DISCONNECTING) {
        emitWrong();
        return;
    }

    if (state_ == State::CONNECTED) {
        state_ = State::DISCONNECTING;
        modem_.disconnect();
    }
    emitOK();
}

void TNCSession::cmdAbort() {
    if (state_ != State::CONNECTING && state_ != State::CONNECTED && state_ != State::DISCONNECTING) {
        emitWrong();
        return;
    }

    modem_.abort();
    pending_inbound_ = false;
    state_ = isReadyAfterDisconnect(mycall_) ? State::READY : State::IDLE;
    emitOK();
}

void TNCSession::cmdCompression(std::string_view args) {
    const auto tokens = splitWhitespace(args);
    if (tokens.size() > 1) {
        emitWrong();
        return;
    }

    if (tokens.empty()) {
        compression_enabled_ = false;
        emitOK();
        return;
    }

    const std::string mode = toUpper(tokens.front());
    if (mode != "OFF" && mode != "TEXT" && mode != "FILES" && mode != "ON") {
        emitWrong();
        return;
    }

    compression_enabled_ = mode != "OFF";
    emitOK();
}

void TNCSession::cmdChat(std::string_view args) {
    const auto tokens = splitWhitespace(args);
    if (tokens.size() != 1) {
        emitWrong();
        return;
    }

    const std::string mode = toUpper(tokens.front());
    if (mode == "ON") {
        chat_enabled_ = true;
        emitOK();
    } else if (mode == "OFF") {
        chat_enabled_ = false;
        emitOK();
    } else {
        emitWrong();
    }
}

void TNCSession::cmdCwid(std::string_view args) {
    const auto tokens = splitWhitespace(args);
    if (tokens.size() != 1) {
        emitWrong();
        return;
    }

    const std::string mode = toUpper(tokens.front());
    if (mode == "ON") {
        cwid_enabled_ = true;
        emitOK();
    } else if (mode == "OFF") {
        cwid_enabled_ = false;
        emitOK();
    } else {
        emitWrong();
    }
}

void TNCSession::cmdVersion(std::string_view args) {
    if (!trimView(args).empty()) {
        emitWrong();
        return;
    }
    emitVersion();
}

void TNCSession::cmdBuffer(std::string_view args) {
    if (!trimView(args).empty()) {
        emitWrong();
        return;
    }
    emitBuffer(modem_.getTxBackloggBytes());
}

void TNCSession::cmdSn(std::string_view args) {
    if (!trimView(args).empty()) {
        emitWrong();
        return;
    }
    last_snr_db_ = static_cast<float>(modem_.getCurrentSNR_db());
    emitSN(last_snr_db_);
}

void TNCSession::cmdBitrate(std::string_view args) {
    if (!trimView(args).empty()) {
        emitWrong();
        return;
    }
    last_bitrate_bps_ = modem_.getCurrentBitrate_bps();
    emitBitrate(last_bitrate_bps_);
}

void TNCSession::cmdPublic(std::string_view args) {
    const auto tokens = splitWhitespace(args);
    if (tokens.size() != 1) {
        emitWrong();
        return;
    }

    const std::string mode = toUpper(tokens.front());
    if (mode == "ON" || mode == "OFF") {
        emitOK();
    } else {
        emitWrong();
    }
}

void TNCSession::cmdP2P(std::string_view args) {
    const auto tokens = splitWhitespace(args);
    if (tokens.empty() || (tokens.size() == 1 && toUpper(tokens.front()) == "SESSION")) {
        emitOK();
    } else {
        emitWrong();
    }
}

void TNCSession::cmdWinlink(std::string_view args) {
    const auto tokens = splitWhitespace(args);
    if (tokens.empty() || (tokens.size() == 1 && toUpper(tokens.front()) == "SESSION")) {
        emitOK();
    } else {
        emitWrong();
    }
}

void TNCSession::cmdIgnoreKissDcd(std::string_view args) {
    (void)args;
    emitOK();
}

void TNCSession::cmdIntegerNoop(std::string_view args) {
    int value = 0;
    if (parseNonNegativeInt(args, value)) {
        emitOK();
    } else {
        emitWrong();
    }
}

void TNCSession::cmdStats(std::string_view args) {
    if (!trimView(args).empty()) {
        emitWrong();
        return;
    }

    const ModemStats s = modem_.getStats();
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "STATS frames_sent=%d frames_recv=%d retx=%d timeouts=%d "
                  "failed=%d out_of_order=%d rate=%s mod=%s mode=%s snr=%d "
                  "bps=%d backlog=%d\r",
                  s.frames_sent, s.frames_received, s.retransmissions,
                  s.timeouts, s.failed, s.out_of_order,
                  s.code_rate.empty() ? "?" : s.code_rate.c_str(),
                  s.modulation.empty() ? "?" : s.modulation.c_str(),
                  s.waveform.empty() ? "?" : s.waveform.c_str(),
                  s.snr_db, s.bitrate_bps, s.tx_backlog_bytes);
    cmd_emit_(buf);
}

} // namespace ultra::tnc
