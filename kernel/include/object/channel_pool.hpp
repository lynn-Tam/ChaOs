#pragma once

#include <ipc/channel.hpp>
#include <libk/memory.hpp>
#include <object/object_pool.hpp>

namespace kernel::object {

template<>
struct ObjectTraits<kernel::ipc::Channel> final {
    static constexpr ObjectKind kind = ObjectKind::Channel;

    static void retire(
        kernel::ipc::Channel& channel,
        ObjectCleanup&& cleanup) noexcept {
        channel.retire(libk::move(cleanup));
    }
    static void destroy(kernel::ipc::Channel& channel) noexcept {
        libk::destroy_at(&channel);
    }
    static void bind_sponsor(
        kernel::ipc::Channel& channel,
        kernel::resource::Sponsorship& sponsor) noexcept {
        channel.bind_sponsor(sponsor);
    }
};

using ChannelPool = ObjectPool<kernel::ipc::Channel>;
using ChannelPending = ChannelPool::Pending;
using ChannelHold = ChannelPool::Hold;
using ChannelPin = ChannelPool::Pin;

} // namespace kernel::object
