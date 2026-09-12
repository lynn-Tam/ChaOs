#pragma once

#include <io/device.hpp>
#include <object/object_pool.hpp>

namespace kernel::object {

template<>
struct ObjectTraits<kernel::io::Device> final {
    static constexpr ObjectKind kind = ObjectKind::Device;
    static void retire(kernel::io::Device& device) noexcept { device.retire(); }
    static void destroy(kernel::io::Device& device) noexcept {
        libk::destroy_at(&device);
    }
};

using DevicePool = ObjectPool<kernel::io::Device>;
using DevicePending = DevicePool::Pending;
using DeviceHold = DevicePool::Hold;
using DevicePin = DevicePool::Pin;

} // namespace kernel::object
