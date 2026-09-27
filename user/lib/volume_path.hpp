#pragma once

namespace myos::volume_path {

// The boot mount is read-only and flat; all other paths belong to Store.
[[nodiscard]] inline auto boot_name(const char* path) noexcept -> const char* {
    if (path == nullptr || path[0] != '/' || path[1] != 'b' || path[2] != 'o'
        || path[3] != 'o' || path[4] != 't') return nullptr;
    if (path[5] == '\0') return path + 5;
    return path[5] == '/' ? path + 6 : nullptr;
}

} // namespace myos::volume_path
