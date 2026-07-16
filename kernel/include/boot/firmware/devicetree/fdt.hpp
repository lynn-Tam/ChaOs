#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libk/byte_reader.hpp>

namespace kernel::boot::fdt {

inline constexpr uint32_t magic = 0xd00dfeed;
inline constexpr uint32_t FDT_BEGIN_NODE = 0x00000001;
inline constexpr uint32_t FDT_END_NODE = 0x00000002;
inline constexpr uint32_t FDT_PROP = 0x00000003;
inline constexpr uint32_t FDT_NOP = 0x00000004;
inline constexpr uint32_t FDT_END = 0x00000009;

using StrView = libk::StrView;
using ByteSpan = libk::ByteSpan;
using ByteReader = libk::ByteReader;

struct FDT_View {
    const uint8_t* base{};
    size_t size{};
    const uint8_t* dt_struct{};
    size_t dt_struct_size{};
    const char* dt_strings{};
    size_t dt_strings_size{};
    const uint8_t* mem_rsvmap{};
    size_t mem_rsvmap_size{};
};

[[nodiscard]] auto init_view(FDT_View& out, const void* dtb) -> bool;

template<typename Visitor>
[[nodiscard]] auto visit_memory_reservations(
    const FDT_View& view,
    Visitor&& visitor) -> bool {
    ByteReader reader{view.mem_rsvmap, view.mem_rsvmap_size};
    for (;;) {
        uint64_t address{};
        uint64_t size{};
        if (!reader.read_be64(address) || !reader.read_be64(size)) {
            return false;
        }
        if (address == 0 && size == 0) {
            return true;
        }
        if (!visitor(address, size)) {
            return false;
        }
    }
}

template<typename Collector>
[[nodiscard]] auto walk_view(const FDT_View& view, Collector&& collector) -> bool {
    ByteReader reader{view.dt_struct, view.dt_struct_size};
    int depth = -1;

    for (;;) {
        uint32_t token{};
        if (!reader.read_be32(token)) {
            return false;
        }

        switch (token) {
        case FDT_BEGIN_NODE: {
            StrView node_name{};
            if (!reader.read_cstr(node_name) || !reader.align(4)) {
                return false;
            }
            ++depth;
            if (!collector.begin_node(node_name, depth)) {
                return false;
            }
            break;
        }
        case FDT_PROP: {
            uint32_t length{};
            uint32_t name_offset{};
            if (!reader.read_be32(length)
                || !reader.read_be32(name_offset)
                || name_offset >= view.dt_strings_size) {
                return false;
            }

            ByteReader name_reader{
                reinterpret_cast<const uint8_t*>(view.dt_strings + name_offset),
                view.dt_strings_size - name_offset,
            };
            StrView property_name{};
            ByteSpan value{};
            if (!name_reader.read_cstr(property_name)
                || !reader.take_bytes(length, value)
                || !reader.align(4)
                || !collector.prop(property_name, value, depth)) {
                return false;
            }
            break;
        }
        case FDT_END_NODE:
            if (depth < 0 || !collector.end_node(depth)) {
                return false;
            }
            --depth;
            break;
        case FDT_NOP:
            break;
        case FDT_END:
            return depth == -1;
        default:
            return false;
        }
    }
}

} // namespace kernel::boot::fdt
