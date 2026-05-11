#include "diagnostics/redaction.hpp"

#include <algorithm>
#include <cctype>

namespace ultra::diagnostics {

std::string jsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += '?';
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

namespace {

std::string lowerCopy(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool keyLooksSensitive(const std::string& key) {
    const std::string k = lowerCopy(key);
    return k.find("callsign") != std::string::npos ||
           k == "call" ||
           k.find("mycall") != std::string::npos ||
           k.find("remote_call") != std::string::npos;
}

} // namespace

std::string redactConfigJson(std::string json, const RedactionOptions& options) {
    if (options.include_callsigns || json.empty()) {
        return json;
    }

    size_t pos = 0;
    while ((pos = json.find('"', pos)) != std::string::npos) {
        const size_t key_start = pos + 1;
        const size_t key_end = json.find('"', key_start);
        if (key_end == std::string::npos) {
            break;
        }
        const std::string key = json.substr(key_start, key_end - key_start);
        size_t colon = key_end + 1;
        while (colon < json.size() && std::isspace(static_cast<unsigned char>(json[colon]))) {
            ++colon;
        }
        if (colon >= json.size() || json[colon] != ':') {
            pos = key_end + 1;
            continue;
        }
        size_t value = colon + 1;
        while (value < json.size() && std::isspace(static_cast<unsigned char>(json[value]))) {
            ++value;
        }
        if (!keyLooksSensitive(key) || value >= json.size() || json[value] != '"') {
            pos = value + 1;
            continue;
        }
        const size_t val_start = value + 1;
        size_t val_end = val_start;
        bool escaped = false;
        while (val_end < json.size()) {
            const char c = json[val_end];
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                break;
            }
            ++val_end;
        }
        if (val_end >= json.size()) {
            break;
        }
        json.replace(val_start, val_end - val_start, "REDACTED");
        pos = val_start + 8;
    }
    return json;
}

} // namespace ultra::diagnostics
