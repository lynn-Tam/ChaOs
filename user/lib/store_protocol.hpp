#pragma once

#include <user/lib/io_session.hpp>

namespace myos::store {

inline constexpr size_t VolumeIdSize = 16;

[[nodiscard]] inline auto parse_volume_id(const char* text, size_t size,
    uint8_t (&id)[VolumeIdSize]) noexcept -> bool {
    if (text == nullptr || size != VolumeIdSize * 2) return false;
    auto digit = [](char c) noexcept -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < VolumeIdSize; ++i) {
        const int high = digit(text[2 * i]);
        const int low = digit(text[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        id[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

enum class Control : uint64_t { List = 16, Open, Sync, Close, Format, Remove, Mkdir, Rename, DeviceId, VolumeId };
// List replies pack nonempty names separated by NUL; value resumes after the
// final name, or is zero at end of directory.
template<class Visit>
[[nodiscard]] auto each_name(const io::ControlMessage& reply, Visit visit) noexcept -> bool {
    if (reply.size > sizeof(reply.data)) return false;
    size_t start{};
    while (start < reply.size) {
        size_t end = start;
        while (end < reply.size && reply.data[end] != '\0') ++end;
        if (end == start) return false;
        visit(reply.data + start, end - start);
        start = end + 1;
    }
    return start == 0 || start == reply.size + 1;
}
enum OpenFlags : uint64_t { Read = 1, Write = 2, Create = 4, Truncate = 8, Exclusive = 16 };
inline constexpr uint64_t ReadDirectory = 1;
inline constexpr uint64_t WriteDirectory = 2;
inline constexpr uint64_t AdminDirectory = 3;

} // namespace myos::store
