#pragma once

#include <user/lib/io_session.hpp>

namespace myos::files {

// The session control Channel authenticates file handles. Open returns a
// session-local handle in value and the byte size as eight little-endian bytes.
// List returns newline-separated short names and a next cursor (zero at end).
enum class Control : uint64_t { List = 16, Open, Close, Map };
inline constexpr size_t HandleCount = 32;
// Directory sender badges identify policy, independent of the client-minted
// private session endpoint. Opening a session freezes this authority ceiling.
inline constexpr uint64_t ReadDirectory = 1;
inline constexpr uint64_t ExecuteDirectory = 3;

inline auto file_size(const io::ControlMessage& reply) noexcept -> uint64_t {
    uint64_t value{};
    for (size_t i = 0; i < 8; ++i) value |= uint64_t{static_cast<uint8_t>(reply.data[i])} << (i * 8);
    return value;
}

} // namespace myos::files
