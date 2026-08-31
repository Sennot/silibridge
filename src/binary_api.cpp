#include "binary_api.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <vector>

namespace bridge {
namespace {

using namespace silifork::live;

constexpr std::uint32_t kExpectedTimestamp = 0x6A90F501;
constexpr std::uint32_t kExpectedImageSize = 0x36B000;
constexpr std::string_view kExpectedDllSha256 =
    "27249D1F61C544A42ECD10E7D3084FB86FAAD935E418E1D690C679710F3C330B";

constexpr std::uintptr_t kBotRva = 0x3330C0;
constexpr std::ptrdiff_t kBotMode = 0x00;
constexpr std::ptrdiff_t kBotEnabledBinding = 0x28;
constexpr std::ptrdiff_t kBotInitialized = 0x48;
constexpr std::ptrdiff_t kBotImplementation = 0x50;

constexpr std::ptrdiff_t kReplaySystem = 0x8C8;
constexpr std::ptrdiff_t kRngManager = 0xE90;
constexpr std::ptrdiff_t kTimelineManager = 0x38A0;

constexpr std::ptrdiff_t kReplayName = 0x220;
constexpr std::ptrdiff_t kRngOrderBegin = 0xD8;
constexpr std::ptrdiff_t kRngOrderEnd = 0xE0;
constexpr std::ptrdiff_t kRngOrderIndex = 0xF0;
constexpr std::ptrdiff_t kRngMinimum = 0x1488;
constexpr std::ptrdiff_t kRngWinner = 0x14A8;
constexpr std::ptrdiff_t kRngCurrent = 0x14C8;
constexpr std::ptrdiff_t kRngStatus = 0x14E8;
constexpr std::ptrdiff_t kRngActive = 0x1508;

constexpr std::uintptr_t kGetFrameRva = 0x4EBE0;
constexpr std::uintptr_t kEnterRecordingRva = 0x21460;
constexpr std::uintptr_t kEnterPlaybackRva = 0x215B0;
constexpr std::uintptr_t kToggleReplayRva = 0xD9B00;
constexpr std::uintptr_t kRngIsSelectedRva = 0xFF6D0;
constexpr std::uintptr_t kRngSetSelectedRva = 0xFF850;
constexpr std::uintptr_t kRngSetWinnerRva = 0xFF910;
constexpr std::uintptr_t kRngFlushRva = 0xFFA00;
constexpr std::uintptr_t kRngCommitMinimumRva = 0xFFA10;
constexpr std::uintptr_t kRngStopRva = 0x101520;
constexpr std::uintptr_t kRngStartRva = 0x1018A0;

struct Signature {
    std::uintptr_t rva;
    std::initializer_list<std::uint8_t> bytes;
};

constexpr std::array<Signature, 11> kSignatures{{
    {kGetFrameRva, {0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCE}},
    {kEnterRecordingRva, {0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53}},
    {kEnterPlaybackRva, {0x56, 0x57, 0x48, 0x83, 0xEC, 0x28}},
    {kToggleReplayRva, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55}},
    {kRngIsSelectedRva, {0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC}},
    {kRngSetSelectedRva, {0x56, 0x57, 0x48, 0x83, 0xEC, 0x38}},
    {kRngSetWinnerRva, {0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC}},
    {kRngFlushRva, {0x80, 0xB9, 0x09, 0x15, 0x00, 0x00, 0x01}},
    {kRngCommitMinimumRva, {0x8B, 0x81, 0x88, 0x14, 0x00, 0x00}},
    {kRngStopRva, {0x55, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83}},
    {kRngStartRva, {0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30}},
}};

bool readable(void const* address, std::size_t size) {
    if (!address || size == 0) return false;
    auto current = reinterpret_cast<std::uintptr_t>(address);
    auto const end = current + size;
    if (end < current) return false;
    while (current < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void const*>(current), &info,
                         sizeof(info)) != sizeof(info))
            return false;
        if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) ||
            info.Protect == PAGE_NOACCESS)
            return false;
        auto const next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) +
                          info.RegionSize;
        if (next <= current) return false;
        current = std::min(next, end);
    }
    return true;
}

std::string readMsvcString(void const* address) {
    if (!readable(address, 32)) return {};
    auto const* raw = static_cast<std::uint8_t const*>(address);
    std::size_t size = 0, capacity = 0;
    std::memcpy(&size, raw + 16, sizeof(size));
    std::memcpy(&capacity, raw + 24, sizeof(capacity));
    if (size > 1024 * 1024 || capacity < size) return {};
    char const* data = reinterpret_cast<char const*>(raw);
    if (capacity >= 16) std::memcpy(&data, raw, sizeof(data));
    if (size == 0) return {};
    if (!readable(data, size)) return {};
    return std::string(data, size);
}

std::string sha256File(std::filesystem::path const& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<std::uint8_t> object;
    std::array<std::uint8_t, 32> digest{};
    std::string result;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) < 0)
        return {};
    DWORD objectSize = 0, received = 0, hashSize = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectSize),
                          sizeof(objectSize), &received, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashSize),
                          sizeof(hashSize), &received, 0) < 0 ||
        hashSize != digest.size()) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    object.resize(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr,
                         0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    std::ifstream input(path, std::ios::binary);
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto const count = input.gcount();
        if (count > 0 &&
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                           static_cast<ULONG>(count), 0) < 0) {
            input.setstate(std::ios::badbit);
            break;
        }
    }
    if (!input.eof() || BCryptFinishHash(hash, digest.data(),
                                         static_cast<ULONG>(digest.size()), 0) < 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0');
    for (auto byte : digest) stream << std::setw(2) << static_cast<int>(byte);
    return stream.str();
}

bool validMacroName(std::string const& name) {
    return !name.empty() && name.size() <= 240 && name != "." && name != ".." &&
           name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos &&
           name.find(':') == std::string::npos &&
           name.find('\0') == std::string::npos;
}

template <class Function>
Function functionAt(std::uintptr_t base, std::uintptr_t rva) {
    return reinterpret_cast<Function>(base + rva);
}

void* bindingValue(void* binding, std::uintptr_t moduleBase) {
    if (!readable(binding, sizeof(void*))) return nullptr;
    auto const vtable = *static_cast<std::uintptr_t**>(binding);
    if (!readable(vtable, 5 * sizeof(std::uintptr_t))) return nullptr;
    auto const fnAddress = vtable[1];
    if (fnAddress < moduleBase || fnAddress >= moduleBase + kExpectedImageSize)
        return nullptr;
    using GetValue = void*(__fastcall*)(void*);
    return reinterpret_cast<GetValue>(fnAddress)(binding);
}

bool notifyBinding(void* binding, std::uintptr_t moduleBase) {
    if (!readable(binding, sizeof(void*))) return false;
    auto const vtable = *static_cast<std::uintptr_t**>(binding);
    if (!readable(vtable, 5 * sizeof(std::uintptr_t))) return false;
    auto const fnAddress = vtable[4];
    if (fnAddress < moduleBase || fnAddress >= moduleBase + kExpectedImageSize)
        return false;
    using Notify = void(__fastcall*)(void*);
    reinterpret_cast<Notify>(fnAddress)(binding);
    return true;
}

CommandResult resultFor(Command const& command, CommandStatus status,
                        std::string message) {
    return {command.id, status, std::move(message)};
}

} // namespace

bool BinaryApi::attach(std::filesystem::path const& binaryPath) {
    if (m_checked) return m_compatible;
    auto module = GetModuleHandleW(binaryPath.filename().c_str());
    if (!module) module = GetModuleHandleW(L"peony.silicate.dll");
    if (!module) {
        m_status = "Waiting for peony.silicate.dll";
        return false;
    }

    m_checked = true;
    auto const hash = sha256File(binaryPath);
    if (hash != kExpectedDllSha256) {
        m_status = "Unsupported SiliFork DLL (SHA-256 mismatch)";
        return false;
    }

    auto const base = reinterpret_cast<std::uintptr_t>(module);
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    if (!readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        m_status = "Invalid SiliFork PE image";
        return false;
    }
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS64 const*>(base + dos->e_lfanew);
    if (!readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.TimeDateStamp != kExpectedTimestamp ||
        nt->OptionalHeader.SizeOfImage != kExpectedImageSize ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        m_status = "Unsupported SiliFork PE build";
        return false;
    }
    for (auto const& signature : kSignatures) {
        auto const* address = reinterpret_cast<std::uint8_t const*>(base + signature.rva);
        if (!readable(address, signature.bytes.size()) ||
            !std::equal(signature.bytes.begin(), signature.bytes.end(), address)) {
            m_status = "Unsupported SiliFork function signatures";
            return false;
        }
    }

    m_base = base;
    m_compatible = true;
    m_status = "Exact SiliFork v1.0.9 binary verified";
    return true;
}

void* BinaryApi::bot() const {
    return m_compatible ? reinterpret_cast<void*>(m_base + kBotRva) : nullptr;
}

void* BinaryApi::implementation() const {
    auto* instance = static_cast<std::uint8_t*>(bot());
    if (!instance || !readable(instance + kBotImplementation, sizeof(void*)))
        return nullptr;
    return *reinterpret_cast<void**>(instance + kBotImplementation);
}

void* BinaryApi::replaySystem() const {
    auto* impl = static_cast<std::uint8_t*>(implementation());
    return impl ? impl + kReplaySystem : nullptr;
}

void* BinaryApi::rngManager() const {
    auto* impl = static_cast<std::uint8_t*>(implementation());
    return impl ? impl + kRngManager : nullptr;
}

void* BinaryApi::timelineManager() const {
    auto* impl = static_cast<std::uint8_t*>(implementation());
    return impl ? impl + kTimelineManager : nullptr;
}

bool BinaryApi::ready() const {
    auto* instance = static_cast<std::uint8_t*>(bot());
    return instance && readable(instance + kBotInitialized, 1) &&
           instance[kBotInitialized] != 0 && implementation();
}

RuntimeSnapshot BinaryApi::snapshot() const {
    RuntimeSnapshot value;
    value.compatible = m_compatible;
    value.initialized = ready();
    if (!value.initialized) return value;

    auto* instance = static_cast<std::uint8_t*>(bot());
    auto* impl = static_cast<std::uint8_t*>(implementation());
    auto* replay = static_cast<std::uint8_t*>(replaySystem());
    auto* rng = static_cast<std::uint8_t*>(rngManager());
    if (!instance || !impl || !replay || !rng) return value;

    auto* enabledBinding = *reinterpret_cast<void**>(instance + kBotEnabledBinding);
    if (auto* enabled = static_cast<bool*>(bindingValue(enabledBinding, m_base));
        readable(enabled, sizeof(bool)))
        value.botEnabled = *enabled;

    auto const mode = *reinterpret_cast<std::int32_t*>(instance + kBotMode);
    value.recording = mode == 0;
    value.playing = mode == 1;
    using GetFrame = std::uint32_t(__fastcall*)(void*);
    value.frame = functionAt<GetFrame>(m_base, kGetFrameRva)(impl);

    value.replayName = readMsvcString(replay + kReplayName);
    value.rngWinningMacro = readMsvcString(rng + kRngWinner);
    value.rngStatus = readMsvcString(rng + kRngStatus);
    auto const current = readMsvcString(rng + kRngCurrent);
    value.rngActive = *reinterpret_cast<bool*>(rng + kRngActive);
    value.rngMinimumBeforeWin = *reinterpret_cast<std::int32_t*>(rng + kRngMinimum);
    if (!current.empty()) value.replayName = current;

    auto* begin = *reinterpret_cast<std::uint8_t**>(rng + kRngOrderBegin);
    auto* end = *reinterpret_cast<std::uint8_t**>(rng + kRngOrderEnd);
    auto const index = *reinterpret_cast<std::size_t*>(rng + kRngOrderIndex);
    if (begin && end && end >= begin &&
        static_cast<std::size_t>(end - begin) % 32 == 0) {
        auto const count = static_cast<std::size_t>(end - begin) / 32;
        value.rngRemaining = static_cast<std::uint32_t>(
            std::min<std::size_t>(index >= count ? 0 : count - index,
                                  std::numeric_limits<std::uint32_t>::max()));
    }
    return value;
}

bool BinaryApi::isSelected(std::string const& name) const {
    if (!ready()) return false;
    using Function = bool(__fastcall*)(void const*, std::string const&);
    return functionAt<Function>(m_base, kRngIsSelectedRva)(rngManager(), name);
}

CommandResult BinaryApi::execute(Command const& command,
                                 std::filesystem::path const& replayDirectory) {
    if (!m_compatible)
        return resultFor(command, CommandStatus::Rejected, m_status);
    if (!ready())
        return resultFor(command, CommandStatus::Busy,
                         "SiliFork has not initialized yet");

    auto* instance = bot();
    auto* rng = rngManager();
    switch (command.type) {
        case CommandType::EnterRecording: {
            using Function = void(__fastcall*)(void*);
            functionAt<Function>(m_base, kEnterRecordingRva)(instance);
            return resultFor(command, CommandStatus::Ok, "Recording mode");
        }
        case CommandType::EnterPlayback: {
            using Function = void(__fastcall*)(void*);
            functionAt<Function>(m_base, kEnterPlaybackRva)(instance);
            return resultFor(command, CommandStatus::Ok, "Playback mode");
        }
        case CommandType::ToggleReplay: {
            if (!validMacroName(command.text))
                return resultFor(command, CommandStatus::Invalid,
                                 "Invalid replay name");
            if (!std::filesystem::is_regular_file(
                    replayDirectory / (command.text + ".slc")))
                return resultFor(command, CommandStatus::NotFound,
                                 "Replay file not found");
            if (snapshot().rngActive) {
                using Stop = void(__fastcall*)(void*, std::string);
                functionAt<Stop>(m_base, kRngStopRva)(rng, std::string{});
            }
            using Function = void(__fastcall*)(void*, std::string const&);
            functionAt<Function>(m_base, kToggleReplayRva)(replaySystem(),
                                                            command.text);
            return resultFor(command, CommandStatus::Ok,
                             "Toggled replay " + command.text);
        }
        case CommandType::RngStart: {
            using Function = bool(__fastcall*)(void*);
            bool const started = functionAt<Function>(m_base, kRngStartRva)(rng);
            auto status = snapshot().rngStatus;
            return resultFor(command,
                             started ? CommandStatus::Ok : CommandStatus::Rejected,
                             started ? "RNG started"
                                     : (status.empty() ? "RNG start rejected" : status));
        }
        case CommandType::RngStop: {
            using Function = void(__fastcall*)(void*, std::string);
            functionAt<Function>(m_base, kRngStopRva)(rng, std::string{});
            return resultFor(command, CommandStatus::Ok, "RNG stopped");
        }
        case CommandType::RngToggle: {
            Command nested = command;
            nested.type = snapshot().rngActive ? CommandType::RngStop
                                                : CommandType::RngStart;
            return execute(nested, replayDirectory);
        }
        case CommandType::RngSetSelected: {
            if (snapshot().rngActive)
                return resultFor(command, CommandStatus::Busy,
                                 "Stop RNG before editing its pool");
            if (!validMacroName(command.text) ||
                !std::filesystem::is_regular_file(
                    replayDirectory / (command.text + ".slc")))
                return resultFor(command, CommandStatus::NotFound,
                                 "Replay file not found");
            using Set = void(__fastcall*)(void*, std::string const&, bool);
            using Flush = void(__fastcall*)(void*);
            functionAt<Set>(m_base, kRngSetSelectedRva)(rng, command.text,
                                                        command.boolValue);
            functionAt<Flush>(m_base, kRngFlushRva)(rng);
            return resultFor(command, CommandStatus::Ok,
                             command.boolValue ? "Added replay to RNG pool"
                                               : "Removed replay from RNG pool");
        }
        case CommandType::RngSetWinner: {
            if (snapshot().rngActive)
                return resultFor(command, CommandStatus::Busy,
                                 "Stop RNG before changing its winner");
            if (!command.text.empty() &&
                (!validMacroName(command.text) ||
                 !std::filesystem::is_regular_file(
                     replayDirectory / (command.text + ".slc"))))
                return resultFor(command, CommandStatus::NotFound,
                                 "Winning replay file not found");
            using Set = void(__fastcall*)(void*, std::string const&);
            using Flush = void(__fastcall*)(void*);
            functionAt<Set>(m_base, kRngSetWinnerRva)(rng, command.text);
            functionAt<Flush>(m_base, kRngFlushRva)(rng);
            return resultFor(command, CommandStatus::Ok,
                             command.text.empty() ? "RNG winner cleared"
                                                  : "RNG winner updated");
        }
        case CommandType::RngSetMinimumBeforeWin: {
            if (snapshot().rngActive)
                return resultFor(command, CommandStatus::Busy,
                                 "Stop RNG before changing its minimum");
            auto const minimum = std::clamp(command.intValue, 0, 100000);
            *reinterpret_cast<std::int32_t*>(
                static_cast<std::uint8_t*>(rng) + kRngMinimum) = minimum;
            using Function = void(__fastcall*)(void*);
            functionAt<Function>(m_base, kRngCommitMinimumRva)(rng);
            return resultFor(command, CommandStatus::Ok,
                             "RNG minimum updated");
        }
        case CommandType::SetBotEnabled: {
            auto* instanceBytes = static_cast<std::uint8_t*>(instance);
            auto* binding = *reinterpret_cast<void**>(instanceBytes + kBotEnabledBinding);
            auto* value = static_cast<bool*>(bindingValue(binding, m_base));
            if (!readable(value, sizeof(bool)))
                return resultFor(command, CommandStatus::Rejected,
                                 "Bot binding is unavailable");
            *value = command.boolValue;
            if (!notifyBinding(binding, m_base))
                return resultFor(command, CommandStatus::Rejected,
                                 "Bot binding notification failed");
            return resultFor(command, CommandStatus::Ok,
                             command.boolValue ? "Bot enabled" : "Bot disabled");
        }
        case CommandType::ToggleReplayTimeline: {
            auto* timeline = static_cast<std::uint8_t*>(timelineManager());
            auto* binding = timeline
                ? *reinterpret_cast<void**>(timeline + 0x18)
                : nullptr;
            auto* value = static_cast<bool*>(bindingValue(binding, m_base));
            if (!readable(value, sizeof(bool)))
                return resultFor(command, CommandStatus::Rejected,
                                 "Timeline binding is unavailable");
            *value = true;
            if (!notifyBinding(binding, m_base))
                return resultFor(command, CommandStatus::Rejected,
                                 "Timeline binding notification failed");
            return resultFor(command, CommandStatus::Ok,
                             "Toggled SiliFork replay timeline");
        }
        case CommandType::RefreshCatalog:
            return resultFor(command, CommandStatus::Ok,
                             "Replay catalog refresh requested");
    }
    return resultFor(command, CommandStatus::Invalid, "Unknown command");
}

} // namespace bridge
