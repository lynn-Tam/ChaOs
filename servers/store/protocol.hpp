#pragma once

#include <stdint.h>
#include <stddef.h>
#include <libk/parse.hpp>

namespace sys::store {

inline constexpr size_t VolumeIdSize = 16;

[[nodiscard]] inline auto parse_volume_id(const char* text, size_t size,
    uint8_t (&id)[VolumeIdSize]) noexcept -> bool {
    if (text == nullptr || size != VolumeIdSize * 2) return false;
    for (size_t i = 0; i < VolumeIdSize; ++i) {
        const auto byte = libk::parse<uint8_t>({text + 2 * i, 2}, 16);
        if (!byte) return false;
        id[i] = *byte;
    }
    return true;
}

enum class Control : uint64_t { List = 16, Format = 20, Remove, Mkdir, Rename, DeviceId, VolumeId, Open, Sync };
// List packs NUL-separated names; value is the next cursor, or zero at EOF.
enum OpenFlags : uint64_t { Read = 1, Write = 2, Create = 4, Truncate = 8, Exclusive = 16, Append = 32 };
inline constexpr uint64_t ReadDirectory = 1;
inline constexpr uint64_t WriteDirectory = 2;
inline constexpr uint64_t AdminDirectory = 3;

} // namespace sys::store

