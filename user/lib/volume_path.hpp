#pragma once

#include <stddef.h>

namespace myos::volume_path {

// The boot mount is read-only and flat. Only this exact component selects it.
[[nodiscard]] inline auto boot_name(const char* path) noexcept -> const char* {
    if (path == nullptr) return nullptr;
    constexpr char prefix[] = "/boot";
    for (size_t i = 0; i < 5; ++i) if (path[i] != prefix[i]) return nullptr;
    if (path[5] == '\0') return path + 5;
    return path[5] == '/' ? path + 6 : nullptr;
}

} // namespace myos::volume_path
