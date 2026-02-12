#pragma once

#include "ultra/types.hpp"
#include "imgui.h"

namespace ultra {
namespace gui {

class StatusWidget {
public:
    StatusWidget();
    void render(const ModemStats& stats, const ModemConfig& config, bool connected);
};

} // namespace gui
} // namespace ultra
