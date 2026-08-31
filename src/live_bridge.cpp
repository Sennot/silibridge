#include "live_bridge.hpp"

#include "binary_api.hpp"
#include "protocol.hpp"
#include "replay_reader.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <Geode/Geode.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace bridge {
namespace {

using namespace silifork::live;

constexpr auto kInvalidSocketValue = static_cast<std::uintptr_t>(INVALID_SOCKET);
constexpr std::size_t kReplayChunkEvents = 4096;
constexpr std::size_t kCatalogChunkEntries = 128;

std::uint64_t monotonicNowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool sendAll(SOCKET socket, std::vector<std::uint8_t> const& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        auto const remaining = std::min<std::size_t>(bytes.size() - sent,
                                                     64 * 1024);
        int const result = send(
            socket, reinterpret_cast<char const*>(bytes.data() + sent),
            static_cast<int>(remaining), 0);
        if (result <= 0) return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

void closeAtomicSocket(std::atomic<std::uintptr_t>& storage) {
    auto const raw = storage.exchange(kInvalidSocketValue,
                                      std::memory_order_acq_rel);
    auto const socket = static_cast<SOCKET>(raw);
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }
}

std::string catalogSignature(CatalogSnapshot const& catalog) {
    std::string result;
    for (auto const& entry : catalog.entries) {
        result += entry.name;
        result.push_back('\0');
        result.push_back(entry.selected ? '1' : '0');
        result.push_back(entry.winning ? '1' : '0');
        result.push_back(entry.completed ? '1' : '0');
    }
    return result;
}

} // namespace

class LiveBridge::Impl {
public:
    void start() {
        if (m_running.exchange(true, std::memory_order_acq_rel)) return;
        m_sessionId = monotonicNowNs() ^
                      (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32);
        m_worker = std::thread(&Impl::worker, this);
    }

    void stop() {
        if (!m_running.exchange(false, std::memory_order_acq_rel)) return;
        closeAtomicSocket(m_clientSocket);
        closeAtomicSocket(m_listenSocket);
        if (m_worker.joinable()) m_worker.join();
        m_connected.store(false, std::memory_order_release);
    }

    void pumpMainThread() {
        auto* siliforkMod = Loader::get()->getLoadedMod("peony.silicate");
        if (!siliforkMod) return;

        if (m_binaryPath.empty()) {
            m_binaryPath = siliforkMod->getBinaryPath();
            m_replayDirectory = siliforkMod->getPersistentDir(true) / "replays";
        }
        bool const compatible = m_api.attach(m_binaryPath);
        if (compatible && !m_loggedCompatible) {
            log::info("RhythmLink verified the exact supported SiliFork DLL");
            m_loggedCompatible = true;
        } else if (!compatible && !m_loggedFailure &&
                   m_api.status().find("Waiting") == std::string::npos) {
            log::error("RhythmLink bridge disabled: {}", m_api.status());
            m_loggedFailure = true;
        }

        bool forceCatalog = m_syncRequested.exchange(false,
                                                     std::memory_order_acq_rel);
        std::deque<Command> commands;
        {
            std::lock_guard lock(m_commandMutex);
            commands.swap(m_commands);
        }
        for (auto const& command : commands) {
            auto result = m_api.execute(command, m_replayDirectory);
            if (command.type == CommandType::RefreshCatalog ||
                result.status == CommandStatus::Ok)
                forceCatalog = true;
            enqueue(MessageType::CommandResult, encodeCommandResult(result));
        }

        auto runtime = m_api.snapshot();
        bool const forceReplay = forceCatalog;
        updateReplay(runtime.replayName, forceReplay);
        updateCatalog(runtime, forceCatalog);
        queueState(runtime);
    }

private:
    void enqueue(MessageType type, std::vector<std::uint8_t> payload) {
        if (!m_connected.load(std::memory_order_acquire)) return;
        auto packet = encodePacket(
            type, m_nextSequence.fetch_add(1, std::memory_order_relaxed), payload);
        std::lock_guard lock(m_outgoingMutex);
        if (type == MessageType::State) {
            m_latestState = std::move(packet);
        } else {
            m_outgoing.push_back(std::move(packet));
        }
    }

    void updateReplay(std::string const& name, bool force) {
        std::filesystem::path path;
        std::filesystem::file_time_type modified{};
        std::error_code error;
        if (!name.empty()) {
            path = m_replayDirectory / (name + ".slc");
            modified = std::filesystem::last_write_time(path, error);
        }

        bool const changed = force || name != m_replay.name ||
                             (!error && modified != m_replayModified);
        if (!changed) return;

        ReplaySnapshot next;
        next.revision = m_replay.revision + 1;
        next.name = name;
        if (!name.empty()) {
            auto parsed = readReplay(path);
            if (!parsed.ok) {
                // A replay may be observed while SiliFork is still saving it.
                // Keep the prior snapshot and retry on the next game frame.
                return;
            }
            next.tps = parsed.tps;
            next.events = std::move(parsed.events);
            m_currentCbfPrecise = parsed.cbfPrecise;
        } else {
            next.tps = 240.0;
            m_currentCbfPrecise = false;
        }
        m_replay = std::move(next);
        m_replayModified = modified;
        queueReplay(m_replay);
    }

    void updateCatalog(RuntimeSnapshot const& runtime, bool force) {
        auto const now = std::chrono::steady_clock::now();
        if (!force && now - m_lastCatalogScan < std::chrono::seconds(1)) return;
        m_lastCatalogScan = now;

        CatalogSnapshot next;
        std::error_code error;
        if (std::filesystem::exists(m_replayDirectory, error)) {
            for (std::filesystem::directory_iterator iterator(m_replayDirectory,
                                                               error), end;
                 !error && iterator != end; iterator.increment(error)) {
                auto const& entry = *iterator;
                if (!entry.is_regular_file(error) ||
                    entry.path().extension() != ".slc")
                    continue;
                CatalogEntry item;
                item.name = entry.path().stem().string();
                item.selected = m_api.isSelected(item.name);
                item.winning = item.name == runtime.rngWinningMacro;
                next.entries.push_back(std::move(item));
            }
        }
        std::ranges::sort(next.entries,
                          [](CatalogEntry const& left, CatalogEntry const& right) {
                              return left.name < right.name;
                          });

        auto const signature = catalogSignature(next);
        if (!force && signature == m_catalogSignature) return;
        next.revision = m_catalog.revision + 1;
        m_catalogSignature = signature;
        m_catalog = std::move(next);
        queueCatalog(m_catalog);
    }

    void queueState(RuntimeSnapshot const& runtime) {
        State state;
        state.serverMonotonicNs = monotonicNowNs();
        state.sessionId = m_sessionId;
        state.replayRevision = m_replay.revision;
        state.catalogRevision = m_catalog.revision;
        state.frame = runtime.frame;
        state.tps = m_replay.tps;
        state.speed = m_estimatedSpeed;
        state.replayName = runtime.replayName;
        state.rngRemaining = runtime.rngRemaining;
        state.rngMinimumBeforeWin = runtime.rngMinimumBeforeWin;
        state.rngWinningMacro = runtime.rngWinningMacro;
        state.rngStatus = runtime.initialized ? runtime.rngStatus : m_api.status();

        if (runtime.botEnabled) state.flags |= StateBotEnabled;
        if (runtime.playing) state.flags |= StatePlaying;
        if (runtime.recording) state.flags |= StateRecording;
        if (!runtime.replayName.empty()) state.flags |= StateReplayLoaded;
        if (runtime.rngActive) state.flags |= StateRngActive;
        if (m_currentCbfPrecise) state.flags |= StateCbfPrecise;

        if (auto* layer = PlayLayer::get()) {
            state.flags |= StateInLevel;
            state.attempt = static_cast<std::uint32_t>(
                std::max(0, layer->m_attempts));
            state.levelTime = layer->m_gameState.m_levelTime;
            if (layer->m_isPaused) state.flags |= StatePaused;
            if (layer->m_playerDied ||
                (layer->m_player1 && layer->m_player1->m_isDead))
                state.flags |= StateDead;
            if (layer->m_hasCompletedLevel) state.flags |= StateCompleted;
            if (layer->m_level) {
                state.levelId =
                    static_cast<std::int64_t>(layer->m_level->m_levelID.value());
                state.levelName = layer->m_level->m_levelName.c_str();
            }

            auto const now = std::chrono::steady_clock::now();
            if (m_haveFrameSample && runtime.frame >= m_lastSampleFrame) {
                double const seconds =
                    std::chrono::duration<double>(now - m_lastFrameSample).count();
                auto const delta = runtime.frame - m_lastSampleFrame;
                if (delta > 0 && seconds >= 0.001 && seconds <= 0.1 &&
                    state.tps > 0.0) {
                    double const measured = std::clamp(
                        static_cast<double>(delta) / (state.tps * seconds),
                        0.05, 20.0);
                    m_estimatedSpeed = m_estimatedSpeed * 0.65 + measured * 0.35;
                    state.speed = m_estimatedSpeed;
                }
            } else if (m_haveFrameSample) {
                m_estimatedSpeed = 1.0;
                state.speed = 1.0;
            }
            m_lastSampleFrame = runtime.frame;
            m_lastFrameSample = now;
            m_haveFrameSample = true;
        } else {
            m_haveFrameSample = false;
            m_estimatedSpeed = 1.0;
            state.speed = 1.0;
        }
        enqueue(MessageType::State, encodeState(state));
    }

    void queueReplay(ReplaySnapshot const& replay) {
        Writer begin;
        begin.u64(replay.revision);
        begin.f64(replay.tps);
        begin.u32(static_cast<std::uint32_t>(replay.events.size()));
        begin.string(replay.name);
        enqueue(MessageType::ReplayBegin, begin.take());

        for (std::size_t offset = 0; offset < replay.events.size();
             offset += kReplayChunkEvents) {
            auto const count = std::min(kReplayChunkEvents,
                                        replay.events.size() - offset);
            Writer chunk;
            chunk.u64(replay.revision);
            chunk.u32(static_cast<std::uint32_t>(offset));
            chunk.u32(static_cast<std::uint32_t>(count));
            for (std::size_t index = 0; index < count; ++index)
                encodeReplayEvent(chunk, replay.events[offset + index]);
            enqueue(MessageType::ReplayChunk, chunk.take());
        }
        Writer commit;
        commit.u64(replay.revision);
        enqueue(MessageType::ReplayCommit, commit.take());
    }

    void queueCatalog(CatalogSnapshot const& catalog) {
        Writer begin;
        begin.u64(catalog.revision);
        begin.u32(static_cast<std::uint32_t>(catalog.entries.size()));
        enqueue(MessageType::CatalogBegin, begin.take());

        for (std::size_t offset = 0; offset < catalog.entries.size();
             offset += kCatalogChunkEntries) {
            auto const count = std::min(kCatalogChunkEntries,
                                        catalog.entries.size() - offset);
            Writer chunk;
            chunk.u64(catalog.revision);
            chunk.u32(static_cast<std::uint32_t>(offset));
            chunk.u32(static_cast<std::uint32_t>(count));
            for (std::size_t index = 0; index < count; ++index)
                encodeCatalogEntry(chunk, catalog.entries[offset + index]);
            enqueue(MessageType::CatalogChunk, chunk.take());
        }
        Writer commit;
        commit.u64(catalog.revision);
        enqueue(MessageType::CatalogCommit, commit.take());
    }

    std::vector<std::uint8_t> helloPacket() {
        Writer hello;
        hello.u32(CapabilityTelemetry | CapabilityReplayStream |
                  CapabilityReplayControl | CapabilityRngControl |
                  CapabilityCbfTiming);
        hello.u16(kProtocolVersion);
        hello.u16(kDefaultPort);
        hello.u64(m_sessionId);
        return encodePacket(MessageType::Hello,
                            m_nextSequence.fetch_add(1,
                                                     std::memory_order_relaxed),
                            hello.take());
    }

    void receivePackets(SOCKET socket, std::vector<std::uint8_t>& incoming,
                        bool& alive) {
        char buffer[32 * 1024];
        int const received = recv(socket, buffer, sizeof(buffer), 0);
        if (received > 0) {
            incoming.insert(incoming.end(), buffer, buffer + received);
            Packet packet;
            while (extractPacket(incoming, packet)) {
                if (packet.type == MessageType::Command) {
                    Command command;
                    if (decodeCommand(packet.payload, command)) {
                        std::lock_guard lock(m_commandMutex);
                        if (m_commands.size() < 256)
                            m_commands.push_back(std::move(command));
                    }
                } else if (packet.type == MessageType::Ping) {
                    enqueue(MessageType::Pong, packet.payload);
                }
            }
        } else if (received == 0) {
            alive = false;
        } else {
            int const error = WSAGetLastError();
            if (error != WSAETIMEDOUT && error != WSAEWOULDBLOCK)
                alive = false;
        }
    }

    void serveClient(SOCKET socket) {
        DWORD receiveTimeout = 20;
        DWORD sendTimeout = 1000;
        int one = 1;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<char const*>(&receiveTimeout),
                   sizeof(receiveTimeout));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<char const*>(&sendTimeout),
                   sizeof(sendTimeout));
        setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<char const*>(&one), sizeof(one));

        m_clientSocket.store(static_cast<std::uintptr_t>(socket),
                             std::memory_order_release);
        m_connected.store(true, std::memory_order_release);
        m_syncRequested.store(true, std::memory_order_release);
        std::vector<std::uint8_t> incoming;
        bool alive = sendAll(socket, helloPacket());
        if (alive)
            log::info("RhythmOverlay connected to SiliFork Bridge");
        while (alive && m_running.load(std::memory_order_acquire)) {
            std::deque<std::vector<std::uint8_t>> outgoing;
            std::optional<std::vector<std::uint8_t>> state;
            {
                std::lock_guard lock(m_outgoingMutex);
                outgoing.swap(m_outgoing);
                state.swap(m_latestState);
            }
            for (auto const& packet : outgoing) {
                if (!sendAll(socket, packet)) {
                    alive = false;
                    break;
                }
            }
            if (alive && state && !sendAll(socket, *state)) alive = false;
            if (alive) receivePackets(socket, incoming, alive);
        }
        m_connected.store(false, std::memory_order_release);
        auto expected = static_cast<std::uintptr_t>(socket);
        if (m_clientSocket.compare_exchange_strong(
                expected, kInvalidSocketValue, std::memory_order_acq_rel)) {
            shutdown(socket, SD_BOTH);
            closesocket(socket);
        }
        {
            std::lock_guard lock(m_outgoingMutex);
            m_outgoing.clear();
            m_latestState.reset();
        }
        log::info("RhythmOverlay disconnected from SiliFork Bridge");
    }

    void worker() {
        WSADATA winsock{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
            log::error("SiliFork Bridge could not initialize Winsock");
            return;
        }

        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            WSACleanup();
            return;
        }
        m_listenSocket.store(static_cast<std::uintptr_t>(listener),
                             std::memory_order_release);
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<char const*>(&one), sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kDefaultPort);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) == SOCKET_ERROR ||
            listen(listener, 1) == SOCKET_ERROR) {
            log::error("SiliFork Bridge could not listen on 127.0.0.1:{}",
                       kDefaultPort);
            closeAtomicSocket(m_listenSocket);
            WSACleanup();
            return;
        }
        u_long nonBlocking = 1;
        ioctlsocket(listener, FIONBIO, &nonBlocking);
        log::info("SiliFork Bridge listening on 127.0.0.1:{}", kDefaultPort);

        while (m_running.load(std::memory_order_acquire)) {
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                int const error = WSAGetLastError();
                if (error != WSAEWOULDBLOCK && error != WSAEINTR &&
                    m_running.load(std::memory_order_acquire))
                    log::warn("SiliFork Bridge accept failed: {}", error);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            serveClient(client);
        }

        closeAtomicSocket(m_clientSocket);
        closeAtomicSocket(m_listenSocket);
        WSACleanup();
    }

    BinaryApi m_api;
    std::filesystem::path m_binaryPath;
    std::filesystem::path m_replayDirectory;
    bool m_loggedCompatible = false;
    bool m_loggedFailure = false;

    ReplaySnapshot m_replay;
    CatalogSnapshot m_catalog;
    std::filesystem::file_time_type m_replayModified{};
    std::chrono::steady_clock::time_point m_lastCatalogScan{};
    std::string m_catalogSignature;
    bool m_currentCbfPrecise = false;
    bool m_haveFrameSample = false;
    std::uint32_t m_lastSampleFrame = 0;
    std::chrono::steady_clock::time_point m_lastFrameSample{};
    double m_estimatedSpeed = 1.0;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_syncRequested{false};
    std::atomic<std::uintptr_t> m_listenSocket{kInvalidSocketValue};
    std::atomic<std::uintptr_t> m_clientSocket{kInvalidSocketValue};
    std::atomic<std::uint32_t> m_nextSequence{1};
    std::uint64_t m_sessionId = 0;
    std::thread m_worker;

    std::mutex m_commandMutex;
    std::deque<Command> m_commands;
    std::mutex m_outgoingMutex;
    std::deque<std::vector<std::uint8_t>> m_outgoing;
    std::optional<std::vector<std::uint8_t>> m_latestState;
};

LiveBridge& LiveBridge::get() {
    static LiveBridge instance;
    return instance;
}

LiveBridge::LiveBridge() : m_impl(std::make_unique<Impl>()) {}
LiveBridge::~LiveBridge() { stop(); }
void LiveBridge::start() { m_impl->start(); }
void LiveBridge::stop() { m_impl->stop(); }
void LiveBridge::pumpMainThread() { m_impl->pumpMainThread(); }

} // namespace bridge
