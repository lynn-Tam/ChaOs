#pragma once

#include <stdint.h>

namespace myos::vfs {
inline constexpr uint64_t ReadDirectory = 1;
inline constexpr uint64_t WriteDirectory = 2;
} // namespace myos::vfs
