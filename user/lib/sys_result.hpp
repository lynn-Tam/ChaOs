#pragma once

#include <uapi/status.h>
#include <uapi/types.h>

namespace myos {

// The userspace syscall ABI returns one status and two word-sized values.
// Keep this representation below deployment so raw syscall consumers do not
// inherit policy, wire parsers or mapping ownership.
struct SysResult final {
    myos_status_t status{};
    myos_word_t value{};
    myos_word_t value2{};
};

} // namespace myos
