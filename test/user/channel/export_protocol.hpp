#pragma once

#include <sys/start.hpp>

namespace channel_test {
inline constexpr boot::Import Provider{"provider.channel", 0x50524f56,
    OBJECT_KIND_CHANNEL};
}
