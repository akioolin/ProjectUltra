#pragma once

#include "ptt_driver.hpp"

#include <memory>

namespace ultra::ptt {

std::unique_ptr<IPttDriver> createPttDriver(const PttConfig& config);

} // namespace ultra::ptt
