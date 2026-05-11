#pragma once

#include <string>

namespace ultra::diagnostics {

struct RedactionOptions {
    bool include_callsigns = false;
};

std::string redactConfigJson(std::string json, const RedactionOptions& options = {});
std::string jsonEscape(const std::string& text);

} // namespace ultra::diagnostics
