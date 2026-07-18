#pragma once

#include <execution/vproc.hpp>
#include <libk/memory.hpp>
#include <object/object_pool.hpp>
#include <object/object_traits.hpp>

namespace kernel::object {

template<>
struct ObjectTraits<kernel::Vproc> final {
    static constexpr ObjectKind kind = ObjectKind::Vproc;

    [[nodiscard]] static auto prepare_retire(
        const kernel::Vproc& vproc) noexcept -> bool {
        return vproc.prepare_retire();
    }
    static void retire([[maybe_unused]] kernel::Vproc& vproc) noexcept {}
    static void destroy(kernel::Vproc& vproc) noexcept {
        libk::destroy_at(&vproc);
    }
};

using VprocPool = ObjectPool<kernel::Vproc>;
using VprocPending = VprocPool::Pending;
using VprocHold = VprocPool::Hold;
using VprocPin = VprocPool::Pin;

} // namespace kernel::object
