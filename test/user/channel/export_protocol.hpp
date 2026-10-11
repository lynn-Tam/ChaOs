#pragma once

#include <sys/protocol.hpp>

#include <sys/start.hpp>

namespace channel_test {
inline constexpr boot::Import Provider{"provider.channel", 0x50524f56,
    ObjChannel};
}
