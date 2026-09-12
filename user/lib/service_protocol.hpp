#pragma once

#include <stdint.h>

namespace myos::service {

inline constexpr uint64_t EventsBadge = 2;

struct Message final {
    uint64_t operation{};
    uint64_t id{};
    int64_t status{};
    uint64_t size{};
    char data[96]{};
};

enum class Process : uint64_t { Spawn = 1, Wait, Stop };

} // namespace myos::service
