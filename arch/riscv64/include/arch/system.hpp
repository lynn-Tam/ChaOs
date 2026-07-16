#pragma once

#include <core/types.hpp>

namespace arch {

enum class HaltReason : u8 {
    Panic,
    PeerStop,
    Fatal,
};

enum class HaltAction : u8 {
    Shutdown,
    Reboot,
};

[[noreturn]] void halt_current_cpu(HaltReason reason) noexcept;
[[noreturn]] void halt_system(HaltAction action, HaltReason reason) noexcept;

} // namespace arch
