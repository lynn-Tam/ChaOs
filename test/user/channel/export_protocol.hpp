#pragma once

#include <user/abi/startup.hpp>

namespace channel_test {
inline constexpr myos::bootstrap::Import Provider{"provider.channel", 0x50524f56,
    MYOS_OBJECT_KIND_CHANNEL};
}
