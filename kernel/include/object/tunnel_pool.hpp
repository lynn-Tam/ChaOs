#pragma once

#include <ipc/tunnel.hpp>
#include <libk/memory.hpp>
#include <object/object_pool.hpp>

namespace kernel::object {

template<>
struct ObjectTraits<kernel::ipc::Tunnel> final {
    static constexpr ObjectKind kind = ObjectKind::Tunnel;

    static void retire(
        kernel::ipc::Tunnel& tunnel,
        ObjectCleanup&& cleanup) noexcept {
        tunnel.retire(libk::move(cleanup));
    }
    static void destroy(kernel::ipc::Tunnel& tunnel) noexcept {
        libk::destroy_at(&tunnel);
    }
};

using TunnelPool = ObjectPool<kernel::ipc::Tunnel>;
using TunnelPending = TunnelPool::Pending;
using TunnelHold = TunnelPool::Hold;
using TunnelPin = TunnelPool::Pin;

} // namespace kernel::object
