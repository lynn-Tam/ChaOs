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
class Channel;
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

// Endpoint capabilities carry the identity and admission limits presented to
// the service. `fixed` describes the badge bits already chosen by an ancestor;
// a fully fixed badge is callable, while a partial view is minting authority.
// Descendants may only fix more bits and reduce transfer capacity.
struct EndpointAuthority final {
    u64 badge{};
    u64 fixed{};
    usize cap_limit{};

    [[nodiscard]] constexpr auto callable() const noexcept -> bool {
        return fixed == ~u64{};
    }

    [[nodiscard]] friend constexpr auto operator==(
        EndpointAuthority, EndpointAuthority) noexcept -> bool = default;
};

enum class ChannelSide : u8 {
    A,
    B,
    Any,
};

struct ChannelAuthority final {
    ChannelSide side{ChannelSide::A};
    u64 badge{};
    // zero means an unbound side-root; all bits set means an exact badge.
    u64 fixed{};

    [[nodiscard]] constexpr auto unbound() const noexcept -> bool {
        return fixed == 0 && badge == 0;
    }
    [[nodiscard]] constexpr auto exact() const noexcept -> bool {
        return fixed == ~u64{} && badge != 0;
    }

    [[nodiscard]] friend constexpr auto operator==(
        ChannelAuthority, ChannelAuthority) noexcept -> bool = default;
};

struct PagerAuthority final {
    u64 backing_key{};
    usize max_pages{};

    [[nodiscard]] friend constexpr auto operator==(
        PagerAuthority, PagerAuthority) noexcept -> bool = default;
};

struct IrqAuthority final {
    u32 source{};
    bool level{};

    [[nodiscard]] friend constexpr auto operator==(
        IrqAuthority, IrqAuthority) noexcept -> bool = default;
};

using AuthorityData = libk::variant<
    libk::monostate,
    MemoryAuthority,
    VSpaceAuthority,
    ResourcePoolAuthority,
    NotificationAuthority,
    EndpointAuthority,
    ChannelAuthority,
    PagerAuthority,
    IrqAuthority>;

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

class ChannelBadgeDerivation final {
public:
    [[nodiscard]] auto channel() const noexcept -> const kernel::ipc::Channel* {
        return channel_;
    }
    [[nodiscard]] constexpr auto side() const noexcept -> ChannelSide {
        return side_;
    }
    [[nodiscard]] constexpr auto badge() const noexcept -> u64 { return badge_; }

private:
    friend class kernel::ipc::Channel;
    friend class GrantGraph;

    constexpr ChannelBadgeDerivation(
        const kernel::ipc::Channel& channel,
        ChannelSide side,
        u64 badge) noexcept
        : channel_(&channel), side_(side), badge_(badge) {}

    const kernel::ipc::Channel* channel_{};
    ChannelSide side_{ChannelSide::A};
    u64 badge_{};
};

} // namespace kernel::cap
