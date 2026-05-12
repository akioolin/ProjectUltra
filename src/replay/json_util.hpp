#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

namespace ultra::replay::json {

inline std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

inline std::string lowerCopy(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

inline std::string upperCopy(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

inline std::string escape(const std::string& text) {
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

inline bool parseStringAt(const std::string& text, size_t quote, std::string* out) {
    if (quote >= text.size() || text[quote] != '"') {
        return false;
    }
    std::string value;
    bool escaped = false;
    for (size_t i = quote + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (escaped) {
            switch (c) {
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                case '/': value += '/'; break;
                case 'b': value += '\b'; break;
                case 'f': value += '\f'; break;
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                default: value += c; break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            if (out) {
                *out = value;
            }
            return true;
        } else {
            value += c;
        }
    }
    return false;
}

inline std::optional<size_t> findValue(const std::string& object, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = object.find(needle, pos)) != std::string::npos) {
        size_t colon = pos + needle.size();
        while (colon < object.size() && std::isspace(static_cast<unsigned char>(object[colon]))) {
            ++colon;
        }
        if (colon < object.size() && object[colon] == ':') {
            size_t value = colon + 1;
            while (value < object.size() && std::isspace(static_cast<unsigned char>(object[value]))) {
                ++value;
            }
            return value;
        }
        pos += needle.size();
    }
    return std::nullopt;
}

inline std::optional<std::string> stringValue(const std::string& object,
                                              const std::string& key) {
    const auto value = findValue(object, key);
    if (!value || *value >= object.size() || object[*value] != '"') {
        return std::nullopt;
    }
    std::string out;
    if (!parseStringAt(object, *value, &out)) {
        return std::nullopt;
    }
    return out;
}

inline std::optional<double> numberValue(const std::string& object,
                                         const std::string& key) {
    const auto value = findValue(object, key);
    if (!value || *value >= object.size()) {
        return std::nullopt;
    }
    const char* begin = object.c_str() + *value;
    char* end = nullptr;
    const double parsed = std::strtod(begin, &end);
    if (end == begin || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

inline std::optional<int64_t> intValue(const std::string& object,
                                       const std::string& key) {
    const auto n = numberValue(object, key);
    if (!n) {
        return std::nullopt;
    }
    return static_cast<int64_t>(*n);
}

inline std::optional<bool> boolValue(const std::string& object, const std::string& key) {
    const auto value = findValue(object, key);
    if (!value) {
        return std::nullopt;
    }
    if (object.compare(*value, 4, "true") == 0) {
        return true;
    }
    if (object.compare(*value, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

inline std::optional<std::string> rawObjectValue(const std::string& object,
                                                const std::string& key) {
    const auto value = findValue(object, key);
    if (!value || *value >= object.size() || object[*value] != '{') {
        return std::nullopt;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = *value; i < object.size(); ++i) {
        const char c = object[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return object.substr(*value, i - *value + 1);
            }
        }
    }
    return std::nullopt;
}

inline int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

inline std::optional<int64_t> parseUtcTimestampMs(const std::string& ts) {
    if (ts.size() < 20) {
        return std::nullopt;
    }
    auto parse2 = [&](size_t pos) -> std::optional<int> {
        if (pos + 2 > ts.size() ||
            !std::isdigit(static_cast<unsigned char>(ts[pos])) ||
            !std::isdigit(static_cast<unsigned char>(ts[pos + 1]))) {
            return std::nullopt;
        }
        return (ts[pos] - '0') * 10 + (ts[pos + 1] - '0');
    };
    auto parse4 = [&](size_t pos) -> std::optional<int> {
        if (pos + 4 > ts.size()) {
            return std::nullopt;
        }
        int value = 0;
        for (size_t i = 0; i < 4; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(ts[pos + i]))) {
                return std::nullopt;
            }
            value = value * 10 + (ts[pos + i] - '0');
        }
        return value;
    };

    auto year = parse4(0);
    auto month = parse2(5);
    auto day = parse2(8);
    auto hour = parse2(11);
    auto minute = parse2(14);
    auto second = parse2(17);
    if (!year || !month || !day || !hour || !minute || !second ||
        ts[4] != '-' || ts[7] != '-' || ts[10] != 'T' ||
        ts[13] != ':' || ts[16] != ':') {
        return std::nullopt;
    }

    int millisecond = 0;
    size_t pos = 19;
    if (pos < ts.size() && ts[pos] == '.') {
        ++pos;
        int scale = 100;
        while (pos < ts.size() &&
               std::isdigit(static_cast<unsigned char>(ts[pos])) &&
               scale > 0) {
            millisecond += (ts[pos] - '0') * scale;
            scale /= 10;
            ++pos;
        }
        while (pos < ts.size() && std::isdigit(static_cast<unsigned char>(ts[pos]))) {
            ++pos;
        }
    }
    if (pos >= ts.size() || ts[pos] != 'Z') {
        return std::nullopt;
    }

    const int64_t days = daysFromCivil(*year, static_cast<unsigned>(*month),
                                       static_cast<unsigned>(*day));
    const int64_t seconds_of_day =
        static_cast<int64_t>(*hour) * 3600 +
        static_cast<int64_t>(*minute) * 60 +
        static_cast<int64_t>(*second);
    return (days * 86400 + seconds_of_day) * 1000 + millisecond;
}

} // namespace ultra::replay::json
