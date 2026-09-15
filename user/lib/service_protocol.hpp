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

// Spawn carries a sequence of NUL-terminated arguments in data[0..size).
// Wait optionally carries an absolute clock deadline (uint64_t); a timeout or
// CancelWait removes the wait only. Stop terminates; the first terminal result wins.
// argv[0] selects the package. A successful reply carries the task token in id.
enum class Process : uint64_t { Spawn = 1, Wait, Stop, CancelWait };

} // namespace myos::service
