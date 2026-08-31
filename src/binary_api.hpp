#pragma once

#include "protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace bridge {

struct RuntimeSnapshot {
    bool compatible = false;
    bool initialized = false;
    bool botEnabled = false;
    bool playing = false;
    bool recording = false;
    bool rngActive = false;
    std::uint32_t frame = 0;
    std::uint32_t rngRemaining = 0;
    std::int32_t rngMinimumBeforeWin = 0;
    std::string replayName;
    std::string rngWinningMacro;
    std::string rngStatus;
};

class BinaryApi {
public:
    bool attach(std::filesystem::path const& binaryPath);
    bool compatible() const { return m_compatible; }
    bool ready() const;
    std::string const& status() const { return m_status; }
    RuntimeSnapshot snapshot() const;

    bool isSelected(std::string const& name) const;
    silifork::live::CommandResult execute(
        silifork::live::Command const& command,
        std::filesystem::path const& replayDirectory);

private:
    void* bot() const;
    void* implementation() const;
    void* replaySystem() const;
    void* rngManager() const;
    void* timelineManager() const;

    std::uintptr_t m_base = 0;
    bool m_checked = false;
    bool m_compatible = false;
    std::string m_status = "Waiting for peony.silicate";
};

} // namespace bridge
