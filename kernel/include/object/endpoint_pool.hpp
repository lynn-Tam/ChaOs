#pragma once

#include <ipc/endpoint.hpp>
#include <libk/memory.hpp>
#include <object/object_pool.hpp>

namespace kernel::object {

template<>
struct ObjectTraits<kernel::ipc::Endpoint> final {
    static constexpr ObjectKind kind = ObjectKind::Endpoint;

    static void retire(
        kernel::ipc::Endpoint& endpoint,
        ObjectCleanup&& cleanup) noexcept {
        endpoint.retire(libk::move(cleanup));
    }
    static void bind_sponsor(
        kernel::ipc::Endpoint& endpoint,
        kernel::resource::Sponsorship& sponsor) noexcept {
        endpoint.bind_sponsor(sponsor);
    }
    static void destroy(kernel::ipc::Endpoint& endpoint) noexcept {
        libk::destroy_at(&endpoint);
    }
};

using EndpointPool = ObjectPool<kernel::ipc::Endpoint>;
using EndpointPending = EndpointPool::Pending;
using EndpointHold = EndpointPool::Hold;
using EndpointPin = EndpointPool::Pin;

} // namespace kernel::object
