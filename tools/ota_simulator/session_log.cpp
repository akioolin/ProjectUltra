#include "ota_simulator/session_log.hpp"

#include "ota_simulator/scenario.hpp"
#include "protocol/frame_v2.hpp"
#include "replay/json_util.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ultra::tools::ota {
namespace {

namespace json = ultra::replay::json;
namespace v2 = ultra::protocol::v2;

int64_t toMs(double t_s) {
    return static_cast<int64_t>(t_s * 1000.0 + 0.5);
}

std::string decodeFrameTypeName(const gui::DecodeResult& result) {
    if (result.is_ping) {
        return "PING_OR_PONG";
    }
    if (!result.frame_data.empty()) {
        auto header = v2::parseHeader(result.frame_data);
        if (header.valid) {
            return v2::frameTypeToString(header.type);
        }
    }
    return result.success ? v2::frameTypeToString(result.frame_type) : "DECODE_FAIL";
}

int decodeFrameSeq(const gui::DecodeResult& result) {
    if (!result.frame_data.empty()) {
        auto header = v2::parseHeader(result.frame_data);
        if (header.valid) {
            return header.seq;
        }
    }
    return -1;
}

std::string withEndpointField(const std::string& endpoint,
                              const std::string& fields_json) {
    if (endpoint.empty()) {
        return fields_json;
    }
    const std::string tag = "\"endpoint\":\"" + json::escape(endpoint) + "\"";
    if (fields_json == "{}") {
        return "{" + tag + "}";
    }
    if (!fields_json.empty() && fields_json.front() == '{') {
        return "{" + tag + "," + fields_json.substr(1);
    }
    return "{" + tag + ",\"fields\":\"" + json::escape(fields_json) + "\"}";
}

}  // namespace

SessionLog::SessionLog(const std::string& path)
    : out_(path) {
    if (!out_) {
        throw std::runtime_error("failed to open session log: " + path);
    }
}

SessionLog::~SessionLog() {
    if (out_) {
        out_.flush();
    }
}

void SessionLog::writeState(double t_s, protocol::ConnectionState state) {
    writeLine(t_s, "protocol", "session.state",
              "{\"state\":\"" + connectionStateName(state) + "\"}");
}

void SessionLog::writeState(const std::string& endpoint, double t_s,
                            protocol::ConnectionState state) {
    writeLine(t_s, "protocol", "session.state",
              withEndpointField(endpoint, "{\"state\":\"" +
                                             connectionStateName(state) + "\"}"),
              endpoint);
}

void SessionLog::writeInject(double t_s, const std::string& file, double gain_db) {
    std::ostringstream fields;
    fields << "{\"file\":\"" << json::escape(file) << "\",\"gain_db\":"
           << std::fixed << std::setprecision(2) << gain_db << "}";
    writeLine(t_s, "ota_simulator", "inject_audio", fields.str());
}

void SessionLog::writeRxFrame(double t_s, const gui::DecodeResult& result) {
    const std::string frame_type = decodeFrameTypeName(result);
    const int seq = decodeFrameSeq(result);
    std::ostringstream fields;
    fields << "{\"frame_type\":\"" << json::escape(frame_type) << "\""
           << ",\"seq\":" << seq
           << ",\"frame_bytes\":" << result.frame_data.size()
           << ",\"codewords_ok\":" << result.codewords_ok
           << ",\"codewords_failed\":" << result.codewords_failed
           << ",\"snr_db\":" << std::fixed << std::setprecision(2) << result.snr_db
           << ",\"cfo_hz\":" << result.cfo_hz
           << ",\"sync_correlation\":" << result.sync_correlation
           << ",\"is_ping\":" << (result.is_ping ? "true" : "false")
           << "}";
    writeLine(t_s, "modem", result.success || result.is_ping ? "frame.rx" : "decode.fail",
              fields.str());
}

void SessionLog::writeRxFrame(const std::string& endpoint, double t_s,
                              const gui::DecodeResult& result) {
    const std::string frame_type = decodeFrameTypeName(result);
    const int seq = decodeFrameSeq(result);
    std::ostringstream fields;
    fields << "{\"frame_type\":\"" << json::escape(frame_type) << "\""
           << ",\"seq\":" << seq
           << ",\"frame_bytes\":" << result.frame_data.size()
           << ",\"codewords_ok\":" << result.codewords_ok
           << ",\"codewords_failed\":" << result.codewords_failed
           << ",\"snr_db\":" << std::fixed << std::setprecision(2) << result.snr_db
           << ",\"cfo_hz\":" << result.cfo_hz
           << ",\"sync_correlation\":" << result.sync_correlation
           << ",\"is_ping\":" << (result.is_ping ? "true" : "false")
           << "}";
    writeLine(t_s, "modem", result.success || result.is_ping ? "frame.rx" : "decode.fail",
              withEndpointField(endpoint, fields.str()), endpoint);
}

void SessionLog::writeTxFrame(double t_s, const std::string& frame_type,
                              int seq, const gui::DecodeResult& result) {
    std::ostringstream fields;
    fields << "{\"frame_type\":\"" << json::escape(frame_type) << "\""
           << ",\"seq\":" << seq
           << ",\"frame_bytes\":" << result.frame_data.size()
           << ",\"codewords_ok\":" << result.codewords_ok
           << ",\"codewords_failed\":" << result.codewords_failed
           << ",\"snr_db\":" << std::fixed << std::setprecision(2) << result.snr_db
           << ",\"cfo_hz\":" << result.cfo_hz
           << ",\"sync_correlation\":" << result.sync_correlation
           << "}";
    writeLine(t_s, "modem", "frame.tx", fields.str());
}

void SessionLog::writeTxFrame(const std::string& endpoint, double t_s,
                              const std::string& frame_type, int seq,
                              const gui::DecodeResult& result) {
    std::ostringstream fields;
    fields << "{\"frame_type\":\"" << json::escape(frame_type) << "\""
           << ",\"seq\":" << seq
           << ",\"frame_bytes\":" << result.frame_data.size()
           << ",\"codewords_ok\":" << result.codewords_ok
           << ",\"codewords_failed\":" << result.codewords_failed
           << ",\"snr_db\":" << std::fixed << std::setprecision(2) << result.snr_db
           << ",\"cfo_hz\":" << result.cfo_hz
           << ",\"sync_correlation\":" << result.sync_correlation
           << "}";
    writeLine(t_s, "modem", "frame.tx", withEndpointField(endpoint, fields.str()),
              endpoint);
}

void SessionLog::writeAssert(double t_s, bool pass, const std::string& description) {
    writeLine(t_s, "ota_simulator", pass ? "assert.pass" : "assert.fail",
              "{\"description\":\"" + json::escape(description) + "\"}");
}

void SessionLog::writeAssert(const std::string& endpoint, double t_s, bool pass,
                             const std::string& description) {
    writeLine(t_s, "ota_simulator", pass ? "assert.pass" : "assert.fail",
              withEndpointField(endpoint, "{\"description\":\"" +
                                          json::escape(description) + "\"}"),
              endpoint);
}

void SessionLog::writeNote(double t_s, const std::string& event,
                           const std::string& fields_json) {
    writeLine(t_s, "ota_simulator", event, fields_json);
}

void SessionLog::writeNote(const std::string& endpoint, double t_s,
                           const std::string& event,
                           const std::string& fields_json) {
    writeLine(t_s, "ota_simulator", event, withEndpointField(endpoint, fields_json),
              endpoint);
}

void SessionLog::writeLine(double t_s, const std::string& component,
                           const std::string& event,
                           const std::string& fields_json,
                           const std::string& endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    out_ << "{\"seq\":" << seq_++
         << ",\"t_ms\":" << toMs(t_s)
         << ",\"component\":\"" << json::escape(component)
         << "\",\"event\":\"" << json::escape(event) << "\"";
    if (!endpoint.empty()) {
        out_ << ",\"endpoint\":\"" << json::escape(endpoint) << "\"";
    }
    out_ << ",\"fields\":" << fields_json << "}\n";
}

}  // namespace ultra::tools::ota
