#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace silifork::live {

inline constexpr std::uint8_t kMagic[4] = {'S', 'F', 'L', 'K'};
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::uint16_t kDefaultPort = 19438;
inline constexpr std::size_t kHeaderSize = 16;
inline constexpr std::uint32_t kMaxPayloadSize = 16u * 1024u * 1024u;

enum class MessageType : std::uint16_t {
    Hello = 1,
    State = 2,
    ReplayBegin = 16,
    ReplayChunk = 17,
    ReplayCommit = 18,
    CatalogBegin = 24,
    CatalogChunk = 25,
    CatalogCommit = 26,
    Command = 32,
    CommandResult = 33,
    Ping = 40,
    Pong = 41,
};

enum Capability : std::uint32_t {
    CapabilityTelemetry = 1u << 0,
    CapabilityReplayStream = 1u << 1,
    CapabilityReplayControl = 1u << 2,
    CapabilityRngControl = 1u << 3,
    CapabilityCbfTiming = 1u << 4,
};

enum StateFlag : std::uint32_t {
    StateInLevel = 1u << 0,
    StateBotEnabled = 1u << 1,
    StatePlaying = 1u << 2,
    StateRecording = 1u << 3,
    StatePaused = 1u << 4,
    StateDead = 1u << 5,
    StateCompleted = 1u << 6,
    StateReplayLoaded = 1u << 7,
    StateRngActive = 1u << 8,
    StateCbfPrecise = 1u << 9,
    StateGeoHeroActive = 1u << 10,
};

enum class CommandType : std::uint16_t {
    EnterRecording = 1,
    EnterPlayback = 2,
    ToggleReplay = 3,
    RngStart = 4,
    RngStop = 5,
    RngToggle = 6,
    RngSetSelected = 7,
    RngSetWinner = 8,
    RngSetMinimumBeforeWin = 9,
    SetBotEnabled = 10,
    ToggleReplayTimeline = 11,
    RefreshCatalog = 12,
};

enum class CommandStatus : std::uint16_t {
    Ok = 0,
    Rejected = 1,
    Invalid = 2,
    NotFound = 3,
    Busy = 4,
};

struct Packet {
    MessageType type = MessageType::Ping;
    std::uint32_t sequence = 0;
    std::vector<std::uint8_t> payload;
};

struct State {
    std::uint64_t serverMonotonicNs = 0;
    std::uint64_t sessionId = 0;
    std::uint64_t replayRevision = 0;
    std::uint64_t catalogRevision = 0;
    std::uint32_t frame = 0;
    std::uint32_t attempt = 0;
    std::int64_t levelId = 0;
    std::uint32_t flags = 0;
    double levelTime = 0.0;
    double tps = 240.0;
    double speed = 1.0;
    float progress = 0.0f;
    std::uint32_t rngRemaining = 0;
    std::int32_t rngMinimumBeforeWin = 0;
    std::string replayName;
    std::string levelName;
    std::string rngWinningMacro;
    std::string rngStatus;
};

struct ReplayEvent {
    std::uint32_t frame = 0;
    float fraction = 0.0f;
    std::uint8_t button = 1;
    bool down = false;
    bool player2 = false;
};

struct ReplaySnapshot {
    std::uint64_t revision = 0;
    double tps = 240.0;
    std::string name;
    std::vector<ReplayEvent> events;
};

struct CatalogEntry {
    std::string name;
    std::int64_t levelId = 0;
    std::string levelName;
    bool selected = false;
    bool winning = false;
    bool completed = false;
};

struct CatalogSnapshot {
    std::uint64_t revision = 0;
    std::vector<CatalogEntry> entries;
};

struct Command {
    std::uint32_t id = 0;
    CommandType type = CommandType::RefreshCatalog;
    bool boolValue = false;
    std::int32_t intValue = 0;
    std::string text;
};

struct CommandResult {
    std::uint32_t id = 0;
    CommandStatus status = CommandStatus::Invalid;
    std::string message;
};

class Writer {
public:
    void u8(std::uint8_t value) { m_data.push_back(value); }
    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8));
    }
    void u32(std::uint32_t value) {
        for (int i = 0; i < 4; ++i)
            u8(static_cast<std::uint8_t>(value >> (i * 8)));
    }
    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
    void u64(std::uint64_t value) {
        for (int i = 0; i < 8; ++i)
            u8(static_cast<std::uint8_t>(value >> (i * 8)));
    }
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
    void f32(float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }
    void f64(double value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u64(bits);
    }
    void string(std::string const& value) {
        u32(static_cast<std::uint32_t>(value.size()));
        m_data.insert(m_data.end(), value.begin(), value.end());
    }
    void bytes(std::vector<std::uint8_t> const& value) {
        m_data.insert(m_data.end(), value.begin(), value.end());
    }
    std::vector<std::uint8_t> take() { return std::move(m_data); }

private:
    std::vector<std::uint8_t> m_data;
};

class Reader {
public:
    explicit Reader(std::vector<std::uint8_t> const& value)
      : m_data(value.data()), m_size(value.size()) {}
    Reader(std::uint8_t const* value, std::size_t size)
      : m_data(value), m_size(size) {}

    bool u8(std::uint8_t& value) {
        if (left() < 1) return false;
        value = m_data[m_offset++];
        return true;
    }
    bool u16(std::uint16_t& value) {
        if (left() < 2) return false;
        value = static_cast<std::uint16_t>(m_data[m_offset]) |
                static_cast<std::uint16_t>(m_data[m_offset + 1] << 8);
        m_offset += 2;
        return true;
    }
    bool u32(std::uint32_t& value) {
        if (left() < 4) return false;
        value = 0;
        for (int i = 0; i < 4; ++i)
            value |= static_cast<std::uint32_t>(m_data[m_offset + i]) << (i * 8);
        m_offset += 4;
        return true;
    }
    bool i32(std::int32_t& value) {
        std::uint32_t raw = 0;
        if (!u32(raw)) return false;
        value = static_cast<std::int32_t>(raw);
        return true;
    }
    bool u64(std::uint64_t& value) {
        if (left() < 8) return false;
        value = 0;
        for (int i = 0; i < 8; ++i)
            value |= static_cast<std::uint64_t>(m_data[m_offset + i]) << (i * 8);
        m_offset += 8;
        return true;
    }
    bool i64(std::int64_t& value) {
        std::uint64_t raw = 0;
        if (!u64(raw)) return false;
        value = static_cast<std::int64_t>(raw);
        return true;
    }
    bool string(std::string& value) {
        std::uint32_t size = 0;
        if (!u32(size) || size > left()) return false;
        value.assign(reinterpret_cast<char const*>(m_data + m_offset), size);
        m_offset += size;
        return true;
    }
    std::size_t left() const { return m_size - m_offset; }
    bool done() const { return m_offset == m_size; }

private:
    std::uint8_t const* m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_offset = 0;
};

inline std::vector<std::uint8_t> encodePacket(
    MessageType type, std::uint32_t sequence,
    std::vector<std::uint8_t> const& payload) {
    Writer writer;
    for (auto byte : kMagic) writer.u8(byte);
    writer.u16(kProtocolVersion);
    writer.u16(static_cast<std::uint16_t>(type));
    writer.u32(static_cast<std::uint32_t>(payload.size()));
    writer.u32(sequence);
    writer.bytes(payload);
    return writer.take();
}

inline bool extractPacket(std::vector<std::uint8_t>& stream, Packet& out) {
    while (stream.size() >= 4 &&
           !std::equal(std::begin(kMagic), std::end(kMagic), stream.begin()))
        stream.erase(stream.begin());
    if (stream.size() < kHeaderSize) return false;

    Reader reader(stream.data() + 4, kHeaderSize - 4);
    std::uint16_t version = 0, type = 0;
    std::uint32_t size = 0, sequence = 0;
    if (!reader.u16(version) || !reader.u16(type) || !reader.u32(size) ||
        !reader.u32(sequence))
        return false;
    if (version != kProtocolVersion || size > kMaxPayloadSize) {
        stream.erase(stream.begin());
        return false;
    }

    auto const packetSize = kHeaderSize + static_cast<std::size_t>(size);
    if (stream.size() < packetSize) return false;
    out.type = static_cast<MessageType>(type);
    out.sequence = sequence;
    out.payload.assign(stream.begin() + static_cast<std::ptrdiff_t>(kHeaderSize),
                       stream.begin() + static_cast<std::ptrdiff_t>(packetSize));
    stream.erase(stream.begin(),
                 stream.begin() + static_cast<std::ptrdiff_t>(packetSize));
    return true;
}

inline std::vector<std::uint8_t> encodeState(State const& value) {
    Writer writer;
    writer.u64(value.serverMonotonicNs);
    writer.u64(value.sessionId);
    writer.u64(value.replayRevision);
    writer.u64(value.catalogRevision);
    writer.u32(value.frame);
    writer.u32(value.attempt);
    writer.i64(value.levelId);
    writer.u32(value.flags);
    writer.f64(value.levelTime);
    writer.f64(value.tps);
    writer.f64(value.speed);
    writer.f32(value.progress);
    writer.u32(value.rngRemaining);
    writer.i32(value.rngMinimumBeforeWin);
    writer.string(value.replayName);
    writer.string(value.levelName);
    writer.string(value.rngWinningMacro);
    writer.string(value.rngStatus);
    return writer.take();
}

inline void encodeReplayEvent(Writer& writer, ReplayEvent const& value) {
    writer.u32(value.frame);
    writer.f32(value.fraction);
    writer.u8(value.button);
    writer.u8(static_cast<std::uint8_t>((value.down ? 1u : 0u) |
                                        (value.player2 ? 2u : 0u)));
    writer.u16(0);
}

inline void encodeCatalogEntry(Writer& writer, CatalogEntry const& value) {
    writer.string(value.name);
    writer.i64(value.levelId);
    writer.string(value.levelName);
    writer.u8(static_cast<std::uint8_t>((value.selected ? 1u : 0u) |
                                        (value.winning ? 2u : 0u) |
                                        (value.completed ? 4u : 0u)));
}

inline bool decodeCommand(std::vector<std::uint8_t> const& payload,
                          Command& value) {
    Reader reader(payload);
    std::uint16_t type = 0;
    std::uint8_t boolValue = 0, reserved = 0;
    if (!reader.u32(value.id) || !reader.u16(type) ||
        !reader.u8(boolValue) || !reader.u8(reserved) ||
        !reader.i32(value.intValue) || !reader.string(value.text) ||
        !reader.done())
        return false;
    value.type = static_cast<CommandType>(type);
    value.boolValue = boolValue != 0;
    return true;
}

inline std::vector<std::uint8_t> encodeCommandResult(CommandResult const& value) {
    Writer writer;
    writer.u32(value.id);
    writer.u16(static_cast<std::uint16_t>(value.status));
    writer.u16(0);
    writer.string(value.message);
    return writer.take();
}

} // namespace silifork::live
