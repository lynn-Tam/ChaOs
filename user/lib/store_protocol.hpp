#pragma once

#include <user/lib/io_session.hpp>

namespace myos::store {

inline constexpr size_t VolumeIdSize = 16;
enum class Control : uint64_t { List = 16, Open, Sync, Close, Format, Remove, Mkdir, Rename, DeviceId, VolumeId };
enum OpenFlags : uint64_t { Read = 1, Write = 2, Create = 4, Truncate = 8, Exclusive = 16 };
inline constexpr uint64_t ReadDirectory = 1;
inline constexpr uint64_t WriteDirectory = 2;
inline constexpr uint64_t AdminDirectory = 3;

} // namespace myos::store
