#pragma once

#include <libk/intrusive_list.hpp>

namespace kernel::ipc {

class Tunnel;

// Non-owning index node embedded in Tunnel. Tunnel's endpoint ObjectHolds keep
// both Vproc payloads stable while this relation is linked; Vproc uses the node
// only to find and invalidate relations during lane teardown.
struct TunnelLink final {
    explicit TunnelLink(Tunnel& owner) noexcept : tunnel(&owner) {}

    libk::IntrusiveListHook hook{};
    Tunnel* tunnel{};
};

} // namespace kernel::ipc
