#pragma once

#include <user/lib/io_session.hpp>

namespace myos::files {

// The session control Channel authenticates file handles. Open returns a
// session-local handle in value and the byte size as eight little-endian bytes.
// List returns newline-separated short names and a next cursor (zero at end).
enum class Control : uint64_t { List = 16, Open, Close };
inline constexpr size_t HandleCount = 32;

inline auto file_size(const io::ControlMessage& reply) noexcept -> uint64_t {
    uint64_t value{};
    for (size_t i = 0; i < 8; ++i) value |= uint64_t{static_cast<uint8_t>(reply.data[i])} << (i * 8);
    return value;
}

} // namespace myos::files
