#include "replay_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>

namespace bridge {
namespace {

using silifork::live::ReplayEvent;

constexpr std::size_t kMaxEvents = 10'000'000;
constexpr std::uint32_t kMaxCbfEntries = 4'000'000;

template <class T>
bool readValue(std::vector<std::uint8_t> const& bytes, std::size_t& offset,
               T& value) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

bool readSizedInteger(std::vector<std::uint8_t> const& bytes,
                      std::size_t& offset, std::size_t size,
                      std::uint64_t& value) {
    if (size != 1 && size != 2 && size != 4 && size != 8) return false;
    if (offset > bytes.size() || size > bytes.size() - offset) return false;
    value = 0;
    std::memcpy(&value, bytes.data() + offset, size);
    offset += size;
    return true;
}

bool saneTps(double value) {
    return std::isfinite(value) && value >= 1.0 && value <= 10'000.0;
}

bool pushEvent(std::vector<ReplayEvent>& events, std::uint64_t frame,
               std::uint8_t button, bool down, bool player2) {
    if (frame > std::numeric_limits<std::uint32_t>::max() ||
        events.size() >= kMaxEvents)
        return false;
    events.push_back({static_cast<std::uint32_t>(frame), 0.0f, button, down,
                      player2});
    return true;
}

bool parseSlc3(std::vector<std::uint8_t> const& bytes,
               ReplayReadResult& result) {
    constexpr std::size_t kHeaderSize = 8;
    constexpr std::uint16_t kMetaSize = 64;
    if (bytes.size() < kHeaderSize + 2 + kMetaSize + 1 ||
        std::memcmp(bytes.data(), "SLC3RPLY", kHeaderSize) != 0)
        return false;

    std::size_t offset = kHeaderSize;
    std::uint16_t metaSize = 0;
    if (!readValue(bytes, offset, metaSize) || metaSize != kMetaSize ||
        metaSize > bytes.size() - offset)
        return false;

    double tps = 0.0;
    std::memcpy(&tps, bytes.data() + offset, sizeof(tps));
    result.tps = saneTps(tps) ? tps : 240.0;
    offset += metaSize;

    while (offset < bytes.size()) {
        if (bytes[offset] == 0xCC) break;
        if (bytes.size() - offset < 12) return false;

        std::uint32_t atomId = 0;
        std::uint64_t rawAtomSize = 0;
        if (!readValue(bytes, offset, atomId) ||
            !readValue(bytes, offset, rawAtomSize))
            return false;
        std::uint64_t const atomSize = rawAtomSize & ~(0xFFull << 56);
        if (atomSize > bytes.size() - offset) return false;
        std::size_t const atomEnd = offset + static_cast<std::size_t>(atomSize);

        if (atomId != 1) {
            offset = atomEnd;
            continue;
        }

        std::uint64_t actionCount = 0;
        if (!readValue(bytes, offset, actionCount) || actionCount > kMaxEvents)
            return false;

        std::uint64_t currentFrame = 0;
        std::uint64_t emittedActions = 0;
        while (emittedActions < actionCount && offset + 2 <= atomEnd) {
            std::uint16_t header = 0;
            if (!readValue(bytes, offset, header)) return false;
            std::uint8_t const section = static_cast<std::uint8_t>(header >> 14);

            if (section <= 1) {
                std::size_t const byteSize = std::size_t{1} << ((header >> 12) & 3);
                std::uint64_t const count = std::uint64_t{1} << ((header >> 8) & 0xF);
                std::uint64_t const repeats = section == 1
                    ? (std::uint64_t{1} << ((header >> 3) & 0x1F))
                    : 1;
                if (count > kMaxEvents || repeats > kMaxEvents ||
                    count > kMaxEvents / repeats)
                    return false;

                struct Input {
                    std::uint64_t delta = 0;
                    std::uint8_t button = 0;
                    bool holding = false;
                    bool player2 = false;
                };
                std::vector<Input> inputs;
                inputs.reserve(static_cast<std::size_t>(count));
                for (std::uint64_t index = 0; index < count; ++index) {
                    if (offset > atomEnd || byteSize > atomEnd - offset)
                        return false;
                    std::uint64_t state = 0;
                    if (!readSizedInteger(bytes, offset, byteSize, state))
                        return false;
                    inputs.push_back({
                        state >> 4,
                        static_cast<std::uint8_t>((state >> 2) & 3),
                        (state & 1) != 0,
                        (state & 2) != 0,
                    });
                }

                for (std::uint64_t repeat = 0; repeat < repeats; ++repeat) {
                    for (auto const& input : inputs) {
                        if (currentFrame >
                            std::numeric_limits<std::uint64_t>::max() - input.delta)
                            return false;
                        currentFrame += input.delta;
                        if (input.button == 0) {
                            if (!pushEvent(result.events, currentFrame, 1, true,
                                           input.player2) ||
                                !pushEvent(result.events, currentFrame, 1, false,
                                           input.player2))
                                return false;
                            emittedActions += 2;
                        } else if (input.button <= 3) {
                            if (!pushEvent(result.events, currentFrame,
                                           input.button, input.holding,
                                           input.player2))
                                return false;
                            ++emittedActions;
                        } else {
                            return false;
                        }
                        if (emittedActions > actionCount) return false;
                    }
                }
            } else if (section == 2) {
                std::uint8_t const specialType =
                    static_cast<std::uint8_t>((header >> 10) & 0xF);
                std::size_t const byteSize = std::size_t{1} << ((header >> 8) & 3);
                if (offset > atomEnd || byteSize > atomEnd - offset) return false;
                std::uint64_t delta = 0;
                if (!readSizedInteger(bytes, offset, byteSize, delta)) return false;
                if (currentFrame > std::numeric_limits<std::uint64_t>::max() - delta)
                    return false;
                currentFrame += delta;
                std::size_t const extra = specialType == 3 || specialType <= 2 ? 8 : 0;
                if (offset > atomEnd || extra > atomEnd - offset) return false;
                offset += extra;
                ++emittedActions;
            } else {
                return false;
            }
        }

        if (emittedActions != actionCount) return false;
        offset = atomEnd;
    }

    return true;
}

bool parseSlc2(std::vector<std::uint8_t> const& bytes,
               ReplayReadResult& result) {
    if (bytes.size() < 39 || std::memcmp(bytes.data(), "SILL", 4) != 0)
        return false;
    std::size_t offset = 4;
    double tps = 0.0;
    std::uint64_t metaSize = 0, inputCount = 0, blobCount = 0;
    if (!readValue(bytes, offset, tps) || !readValue(bytes, offset, metaSize) ||
        metaSize > bytes.size() - offset)
        return false;
    result.tps = saneTps(tps) ? tps : 240.0;
    offset += static_cast<std::size_t>(metaSize);
    if (!readValue(bytes, offset, inputCount) ||
        !readValue(bytes, offset, blobCount) ||
        inputCount > kMaxEvents || blobCount > inputCount + 1)
        return false;

    struct Blob {
        std::uint64_t byteSize = 0;
        std::uint64_t start = 0;
        std::uint64_t length = 0;
    };
    std::vector<Blob> blobs(static_cast<std::size_t>(blobCount));
    for (auto& blob : blobs) {
        if (!readValue(bytes, offset, blob.byteSize) ||
            !readValue(bytes, offset, blob.start) ||
            !readValue(bytes, offset, blob.length) ||
            (blob.byteSize != 1 && blob.byteSize != 2 &&
             blob.byteSize != 4 && blob.byteSize != 8) ||
            blob.start > inputCount || blob.length > inputCount - blob.start)
            return false;
    }

    std::uint64_t currentFrame = 0;
    for (auto const& blob : blobs) {
        for (std::uint64_t index = 0; index < blob.length; ++index) {
            std::uint64_t state = 0;
            if (!readSizedInteger(bytes, offset,
                                  static_cast<std::size_t>(blob.byteSize), state))
                return false;
            std::uint64_t const delta = state >> 5;
            if (currentFrame > std::numeric_limits<std::uint64_t>::max() - delta)
                return false;
            currentFrame += delta;
            std::uint8_t const type = static_cast<std::uint8_t>((state >> 2) & 7);
            if (type == 7) {
                double ignored = 0.0;
                if (!readValue(bytes, offset, ignored)) return false;
            } else if (type >= 1 && type <= 3) {
                if (!pushEvent(result.events, currentFrame, type,
                               (state & 1) != 0, (state & 2) != 0))
                    return false;
            }
        }
    }
    return true;
}

bool parseSlc1(std::vector<std::uint8_t> const& bytes,
               ReplayReadResult& result) {
    if (bytes.size() < 12) return false;
    std::size_t offset = 0;
    double tps = 0.0;
    std::uint32_t count = 0;
    if (!readValue(bytes, offset, tps) || !saneTps(tps) ||
        !readValue(bytes, offset, count) || count > kMaxEvents ||
        static_cast<std::size_t>(count) > (bytes.size() - offset) / 4)
        return false;
    result.tps = tps;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t state = 0;
        if (!readValue(bytes, offset, state)) return false;
        auto const button = static_cast<std::uint8_t>((state >> 1) & 3);
        if (button >= 1 && button <= 3 &&
            !pushEvent(result.events, state >> 4, button, (state & 1) != 0,
                       (state & 8) != 0))
            return false;
    }
    return true;
}

std::uint64_t eventKey(ReplayEvent const& event) {
    return static_cast<std::uint64_t>(event.frame) |
           (static_cast<std::uint64_t>(event.button & 3) << 32) |
           (static_cast<std::uint64_t>(event.down ? 1 : 0) << 34) |
           (static_cast<std::uint64_t>(event.player2 ? 1 : 0) << 35);
}

void applyCbf(std::filesystem::path const& replayPath,
              ReplayReadResult& result) {
    auto sidecar = replayPath;
    sidecar += ".cbf";
    std::ifstream input(sidecar, std::ios::binary);
    if (!input) return;
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    constexpr char kMagic[8] = {'S', 'F', 'C', 'B', 'F', '2', '\r', '\n'};
    if (bytes.size() < 12 || std::memcmp(bytes.data(), kMagic, 8) != 0)
        return;
    std::size_t offset = 8;
    std::uint32_t count = 0;
    if (!readValue(bytes, offset, count) || count > kMaxCbfEntries ||
        static_cast<std::size_t>(count) > (bytes.size() - offset) / 12)
        return;

    std::unordered_map<std::uint64_t, std::vector<std::size_t>> indices;
    indices.reserve(result.events.size());
    for (std::size_t index = 0; index < result.events.size(); ++index)
        indices[eventKey(result.events[index])].push_back(index);

    bool applied = false;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t frame = 0, occurrence = 0;
        std::uint16_t fraction = 0;
        std::uint8_t button = 0, flags = 0;
        if (!readValue(bytes, offset, frame) ||
            !readValue(bytes, offset, occurrence) ||
            !readValue(bytes, offset, fraction) ||
            !readValue(bytes, offset, button) ||
            !readValue(bytes, offset, flags) || button < 1 || button > 3 ||
            fraction >= 65535 || (flags & ~std::uint8_t{3}) != 0)
            return;

        ReplayEvent keyEvent{frame, 0.0f, button, (flags & 1) != 0,
                             (flags & 2) != 0};
        auto found = indices.find(eventKey(keyEvent));
        if (found == indices.end() || occurrence >= found->second.size())
            continue;
        result.events[found->second[occurrence]].fraction =
            static_cast<float>(fraction) / 65535.0f;
        applied = true;
    }
    result.cbfPrecise = applied;
}

} // namespace

ReplayReadResult readReplay(std::filesystem::path const& path) {
    ReplayReadResult result;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = "Replay file is not readable";
        return result;
    }
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        result.error = "Replay file is empty";
        return result;
    }

    bool parsed = false;
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "SLC3RPLY", 8) == 0)
        parsed = parseSlc3(bytes, result);
    else if (bytes.size() >= 4 && std::memcmp(bytes.data(), "SILL", 4) == 0)
        parsed = parseSlc2(bytes, result);
    else
        parsed = parseSlc1(bytes, result);

    if (!parsed) {
        result.events.clear();
        result.error = "Unsupported or malformed SLC replay";
        return result;
    }

    applyCbf(path, result);
    std::stable_sort(result.events.begin(), result.events.end(),
                     [](ReplayEvent const& left, ReplayEvent const& right) {
                         double const a = static_cast<double>(left.frame) + left.fraction;
                         double const b = static_cast<double>(right.frame) + right.fraction;
                         return a < b;
                     });
    result.ok = true;
    return result;
}

} // namespace bridge
