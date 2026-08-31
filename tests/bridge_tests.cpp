#include "protocol.hpp"
#include "replay_reader.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

template <class T>
void append(std::vector<std::uint8_t>& bytes, T const& value) {
    auto const* first = reinterpret_cast<std::uint8_t const*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(value));
}

void write(std::filesystem::path const& path,
           std::vector<std::uint8_t> const& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<char const*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    assert(output.good());
}

void testSlc1AndCbf(std::filesystem::path const& directory) {
    auto const replay = directory / "legacy.slc";
    std::vector<std::uint8_t> bytes;
    double const tps = 240.0;
    std::uint32_t const count = 2;
    std::uint32_t const down = (12u << 4) | (1u << 1) | 1u;
    std::uint32_t const up = (18u << 4) | (1u << 1);
    append(bytes, tps);
    append(bytes, count);
    append(bytes, down);
    append(bytes, up);
    write(replay, bytes);

    std::vector<std::uint8_t> cbf{'S', 'F', 'C', 'B', 'F', '2', '\r', '\n'};
    std::uint32_t const cbfCount = 1;
    std::uint32_t const frame = 12, occurrence = 0;
    std::uint16_t const fraction = 32768;
    std::uint8_t const button = 1, flags = 1;
    append(cbf, cbfCount);
    append(cbf, frame);
    append(cbf, occurrence);
    append(cbf, fraction);
    append(cbf, button);
    append(cbf, flags);
    write(std::filesystem::path(replay.string() + ".cbf"), cbf);

    auto parsed = bridge::readReplay(replay);
    assert(parsed.ok);
    assert(parsed.cbfPrecise);
    assert(parsed.events.size() == 2);
    assert(parsed.events[0].frame == 12 && parsed.events[0].down);
    assert(std::abs(parsed.events[0].fraction - 32768.0f / 65535.0f) < 1e-6f);
    assert(parsed.events[1].frame == 18 && !parsed.events[1].down);
}

void testSlc3(std::filesystem::path const& directory) {
    auto const replay = directory / "current.slc";
    std::vector<std::uint8_t> bytes{'S', 'L', 'C', '3', 'R', 'P', 'L', 'Y'};
    std::uint16_t const metaSize = 64;
    append(bytes, metaSize);
    std::vector<std::uint8_t> meta(metaSize, 0);
    double const tps = 360.0;
    std::memcpy(meta.data(), &tps, sizeof(tps));
    bytes.insert(bytes.end(), meta.begin(), meta.end());

    std::uint32_t const atomId = 1;
    std::uint64_t const atomSize = 12;
    std::uint64_t const actionCount = 2;
    std::uint16_t const sectionHeader = 0x0100; // two one-byte inputs
    append(bytes, atomId);
    append(bytes, atomSize);
    append(bytes, actionCount);
    append(bytes, sectionHeader);
    bytes.push_back(static_cast<std::uint8_t>((5u << 4) | (1u << 2) | 1u));
    bytes.push_back(static_cast<std::uint8_t>((3u << 4) | (1u << 2)));
    bytes.push_back(0xCC);
    write(replay, bytes);

    auto parsed = bridge::readReplay(replay);
    assert(parsed.ok);
    assert(std::abs(parsed.tps - 360.0) < 1e-9);
    assert(parsed.events.size() == 2);
    assert(parsed.events[0].frame == 5 && parsed.events[0].down);
    assert(parsed.events[1].frame == 8 && !parsed.events[1].down);
}

void testProtocol() {
    using namespace silifork::live;
    Writer payload;
    payload.u32(77);
    payload.u16(static_cast<std::uint16_t>(CommandType::RngSetSelected));
    payload.u8(1);
    payload.u8(0);
    payload.i32(9);
    payload.string("macro-a");
    auto stream = encodePacket(MessageType::Command, 4, payload.take());

    Packet packet;
    assert(extractPacket(stream, packet));
    assert(stream.empty());
    Command command;
    assert(decodeCommand(packet.payload, command));
    assert(command.id == 77);
    assert(command.type == CommandType::RngSetSelected);
    assert(command.boolValue && command.intValue == 9);
    assert(command.text == "macro-a");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        auto parsed = bridge::readReplay(argv[1]);
        if (!parsed.ok) {
            std::cerr << parsed.error << '\n';
            return 1;
        }
        std::cout << parsed.events.size() << " events at " << parsed.tps
                  << " TPS, CBF=" << (parsed.cbfPrecise ? "yes" : "no")
                  << '\n';
        return 0;
    }
    auto directory = std::filesystem::temp_directory_path() /
                     "silifork-bridge-format-tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    testSlc1AndCbf(directory);
    testSlc3(directory);
    testProtocol();
    std::filesystem::remove_all(directory, error);
    return 0;
}
