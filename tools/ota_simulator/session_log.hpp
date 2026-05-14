#pragma once

#include "gui/modem/streaming_decoder.hpp"
#include "protocol/connection.hpp"

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace ultra::tools::ota {

class SessionLog {
public:
    explicit SessionLog(const std::string& path);
    ~SessionLog();

    void writeState(double t_s, protocol::ConnectionState state);
    void writeInject(double t_s, const std::string& file, double gain_db);
    void writeRxFrame(double t_s, const gui::DecodeResult& result);
    void writeTxFrame(double t_s, const std::string& frame_type,
                      int seq, const gui::DecodeResult& result);
    void writeAssert(double t_s, bool pass, const std::string& description);
    void writeNote(double t_s, const std::string& event, const std::string& fields_json);

private:
    void writeLine(double t_s, const std::string& component,
                   const std::string& event, const std::string& fields_json);

    std::ofstream out_;
    std::mutex mutex_;
    uint64_t seq_ = 0;
};

}  // namespace ultra::tools::ota
