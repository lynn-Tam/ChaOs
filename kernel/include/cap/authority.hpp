#pragma once

#include <cap/rights.hpp>
#include <libk/variant.hpp>
#include <mm/addr.hpp>
#include <mm/object_range.hpp>
#include <mm/permissions.hpp>
#include <mm/vm_key.hpp>
#include <resource/sponsorship.hpp>

namespace kernel::mm {
class VSpace;
}

namespace kernel {
class Vproc;
namespace ipc {
class Tunnel;
}
}

namespace kernel::cap {

struct MemoryAuthority final {
    kernel::mm::ObjectRange range{};
    kernel::mm::AccessMask access{};
    kernel::mm::MemoryTypes types{};

    [[nodiscard]] friend constexpr auto operator==(
        MemoryAuthority, MemoryAuthority) noexcept -> bool = default;
};

struct VSpaceAuthority final {
    kernel::mm::RegionKey region{};
    kernel::mm::VirtRange range{};
    kernel::mm::AccessMask access{};
    kernel::mm::MemoryTypes types{};

    [[nodiscard]] friend constexpr auto operator==(
        VSpaceAuthority, VSpaceAuthority) noexcept -> bool = default;
};

struct ResourcePoolAuthority final {
    kernel::resource::Budget budget{};
    u64 object_kinds{};

    [[nodiscard]] friend constexpr auto operator==(
        ResourcePoolAuthority, ResourcePoolAuthority) noexcept
        -> bool = default;
};

struct NotificationAuthority final {
    u64 badge{};

    [[nodiscard]] friend constexpr auto operator==(
        NotificationAuthority, NotificationAuthority) noexcept
        -> bool = default;
};

using AuthorityData = libk::variant<
    libk::monostate,
    MemoryAuthority,
    VSpaceAuthority,
    ResourcePoolAuthority,
    NotificationAuthority>;

struct GrantCeiling final {
    Rights rights{};
    AuthorityData data{};
};

struct CapView final {
    Rights rights{};
    AuthorityData data{};
};

struct EffectiveAuthority final {
    Rights rights{};
    AuthorityData data{};

    [[nodiscard]] auto ceiling() const noexcept -> GrantCeiling {
        return GrantCeiling{rights, data};
    }
};

// Proof carried only across the VSpace-owned create-region transaction. It
// authorizes one resource-identity change that generic duplicate/delegate
// must reject; GrantGraph still validates all immutable proof fields.
class RegionDerivation final {
public:
    [[nodiscard]] auto parent() const noexcept -> kernel::mm::RegionKey {
        return parent_;
    }
    [[nodiscard]] auto child() const noexcept -> kernel::mm::RegionKey {
        return child_;
    }
    [[nodiscard]] auto range() const noexcept -> kernel::mm::VirtRange {
        return range_;
    }

private:
    friend class kernel::mm::VSpace;
    friend class GrantGraph;

    constexpr RegionDerivation(
        kernel::mm::RegionKey parent,
        kernel::mm::RegionKey child,
        kernel::mm::VirtRange range) noexcept
        : parent_(parent), child_(child), range_(range) {}

    kernel::mm::RegionKey parent_{};
    kernel::mm::RegionKey child_{};
    kernel::mm::VirtRange range_{};
};

// Closed proof for the one semantic derivation that changes a Tunnel grant
// from receiver-issued Connect authority into source-bound Tx authority.
// Generic GrantGraph::derive remains strict rights attenuation.
class TunnelConnectProof final {
public:
    [[nodiscard]] auto tunnel() const noexcept -> const kernel::ipc::Tunnel* {
        return tunnel_;
    }
    [[nodiscard]] auto source() const noexcept -> const kernel::Vproc* {
        return source_;
    }
    [[nodiscard]] auto claim() const noexcept -> u64 { return claim_; }

private:
    friend class kernel::ipc::Tunnel;
    friend class GrantGraph;

    constexpr TunnelConnectProof(
        const kernel::ipc::Tunnel& tunnel,
        const kernel::Vproc& source,
        u64 claim) noexcept
        : tunnel_(&tunnel), source_(&source), claim_(claim) {}

    const kernel::ipc::Tunnel* tunnel_{};
    const kernel::Vproc* source_{};
    u64 claim_{};
};

} // namespace kernel::cap
