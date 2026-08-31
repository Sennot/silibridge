#pragma once

#include "protocol.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace bridge {

struct ReplayReadResult {
    bool ok = false;
    bool cbfPrecise = false;
    double tps = 240.0;
    std::vector<silifork::live::ReplayEvent> events;
    std::string error;
};

ReplayReadResult readReplay(std::filesystem::path const& path);

} // namespace bridge
