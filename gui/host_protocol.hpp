#pragma once
#include <gui/protocol.hpp>
#include <bit>
#include <string.h>
#include <span>

// Local, coherent shared pixels plus an ordered control stream. A new Hello
// must be acknowledged after old windows are destroyed, before offsets recur.
// Ack flags identify the original command; input sequence is monotonic per epoch.
enum : uint16_t { gh_hello = 1, gh_window, gh_remove, gh_present, gh_ack, gh_input };
enum : uint32_t { gh_visible = 1 << 16, gh_activate = 1 << 17 };
struct gui_host_packet {
    uint32_t magic{0x31495547};
    uint16_t version{1}, type{};
    uint64_t epoch{}, window{}, sequence{}, offset{};
    uint32_t width{}, height{}, stride{}, flags{};
    gui_rect damage;
    gui_limits limits;
    gui_event event;
    char title[128]{};
    auto valid() const -> bool { return magic == 0x31495547 && version == 1 && type >= gh_hello && type <= gh_input; }
};
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(gui_host_packet) == 280);

// Port reconnect may leave the tail of an old DMA buffer in the byte stream.
// Recover framing before applying the epoch barrier; never interpret that tail.
struct gui_host_reader {
    gui_host_packet packet;
    size_t size{};
    auto tail() -> std::span<char> { return {reinterpret_cast<char*>(&packet) + size, sizeof(packet) - size}; }
    auto advance(size_t bytes) -> bool {
        size += bytes;
        if (size < 8) return false;
        if (packet.valid()) { if (size != sizeof(packet)) return false; size = 0; return true; }
        auto* data = reinterpret_cast<char*>(&packet);
        size_t offset = 1;
        for (; offset + 8 <= size; ++offset) {
            uint64_t header; ::memcpy(&header, data + offset, 8);
            const auto type = header >> 48;
            if (uint32_t(header) == 0x31495547 && uint16_t(header >> 32) == 1 && type >= gh_hello && type <= gh_input) break;
        }
        size -= offset; ::memmove(data, data + offset, size); return false;
    }
};
