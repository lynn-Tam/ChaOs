#pragma once
#include <array>
#include <base/types.hpp>
#include <uapi/io.h>

namespace io {

struct Bar final { usize address{}; usize size{}; u32 attributes{}; };
struct Fault final { u16 cause{}; u32 requester{}; u64 address{}; };
struct DeviceInfo final {
    u32 version{DEVICE_INFO_VERSION};
    u32 requester{};
    std::array<u32, 64> configuration{};
    std::array<u64, 6> bar_sizes{};
};

static_assert(sizeof(DeviceInfo) == sizeof(DeviceDesc));
static_assert(offsetof(DeviceInfo, configuration) == offsetof(DeviceDesc, configuration));
static_assert(offsetof(DeviceInfo, bar_sizes) == offsetof(DeviceDesc, bar_sizes));

} // namespace io
