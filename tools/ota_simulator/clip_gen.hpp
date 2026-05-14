#pragma once

#include <string>
#include <vector>

namespace ultra::tools::ota {

struct ClipGenOptions {
    std::string frame;
    std::string callsign;
    std::string peer_callsign = "W1ABC";
    std::string out_path;
};

int generateClip(const ClipGenOptions& options);

}  // namespace ultra::tools::ota
