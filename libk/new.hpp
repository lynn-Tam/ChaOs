#pragma once

#include <stddef.h>

[[nodiscard]] inline auto operator new(
    size_t,
    void* location) noexcept -> void* {
    return location;
}

[[nodiscard]] inline auto operator new[](
    size_t,
    void* location) noexcept -> void* {
    return location;
}

inline void operator delete(void*, void*) noexcept {}
inline void operator delete[](void*, void*) noexcept {}
