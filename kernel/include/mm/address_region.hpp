#pragma once

#include <core/debug.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/noncopyable.hpp>
#include <mm/addr.hpp>
#include <mm/user_view.hpp>
#include <mm/object_range.hpp>
#include <mm/permissions.hpp>
#include <mm/vm_key.hpp>

namespace kernel::mm {

class VSpace;
class MappingAuthority;
struct LayoutCompare;

enum class LayoutKind : u8 {
    Region,
    Mapping,
    Reserved,
    Guard,
};

enum class RegionState : u8 {
    Live,
    Retiring,
    Dead,
};

enum class MappingState : u8 {
    Preparing,
    Live,
    Protecting,
    Invalidating,
    Detached,
};

struct RegionPolicy final {
    AccessMask access{};
    MemoryTypes types{};
};

class AddressRegion;

class LayoutNode : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto kind() const noexcept -> LayoutKind { return kind_; }
    [[nodiscard]] auto range() const noexcept -> VirtRange { return range_; }
    [[nodiscard]] auto parent() const noexcept -> AddressRegion* {
        return parent_;
    }

    // Membership storage only. LayoutTree owns all mutation and VSpace owns
    // the node lifetime; exposing the hook avoids making layout identity a
    // second wrapper object.
    libk::IntrusiveTreeHook layout_hook_{};

protected:
    LayoutNode(
        LayoutKind kind,
        VirtRange range,
        AddressRegion* parent) noexcept
        : kind_(kind), range_(range), parent_(parent) {}
    ~LayoutNode() noexcept = default;

private:
    friend class VSpace;
    friend class AddressRegion;

    LayoutKind kind_;
    VirtRange range_;
    AddressRegion* parent_{};
    LayoutNode* pending_next_{};
};

struct LayoutCompare final {
    [[nodiscard]] constexpr auto operator()(
        const LayoutNode& lhs,
        const LayoutNode& rhs) const noexcept -> bool {
        return lhs.range().base() < rhs.range().base();
    }
    [[nodiscard]] constexpr auto operator()(
        VirtAddr lhs,
        const LayoutNode& rhs) const noexcept -> bool {
        return lhs < rhs.range().base();
    }
    [[nodiscard]] constexpr auto operator()(
        const LayoutNode& lhs,
        VirtAddr rhs) const noexcept -> bool {
        return lhs.range().base() < rhs;
    }
};

using LayoutTree = libk::IntrusiveTree<
    LayoutNode, &LayoutNode::layout_hook_, LayoutCompare>;

class AddressRegion final : public LayoutNode {
public:
    AddressRegion(
        VirtRange range,
        AddressRegion* parent,
        RegionPolicy policy) noexcept
        : LayoutNode(LayoutKind::Region, range, parent), policy_(policy) {}
    ~AddressRegion() noexcept { KASSERT(children_.empty()); }

    [[nodiscard]] auto key() const noexcept -> RegionKey { return key_; }
    [[nodiscard]] auto policy() const noexcept -> RegionPolicy {
        return policy_;
    }
    [[nodiscard]] auto state() const noexcept -> RegionState { return state_; }

private:
    friend class VSpace;

    RegionKey key_{};
    RegionPolicy policy_{};
    RegionState state_{RegionState::Live};
    LayoutTree children_{};
};

class Mapping final : public LayoutNode {
    using UserViewRelations = libk::IntrusiveList<
        UserViewRelation, &UserViewRelation::mapping_hook_>;
public:
    Mapping(
        VirtRange range,
        AddressRegion& parent,
        ObjectRange object,
        AccessMask access,
        AccessMask ceiling,
        MemoryTypes types,
        MappingAuthority& authority) noexcept
        : LayoutNode(LayoutKind::Mapping, range, &parent),
          object_(object),
          access_(access),
          ceiling_(ceiling),
          types_(types),
          authority_(&authority) {}
    ~Mapping() noexcept;

    [[nodiscard]] auto key() const noexcept -> MappingKey { return key_; }
    [[nodiscard]] auto object_range() const noexcept -> ObjectRange {
        return object_;
    }
    [[nodiscard]] auto access() const noexcept -> AccessMask { return access_; }
    [[nodiscard]] auto ceiling() const noexcept -> AccessMask {
        return ceiling_;
    }
    [[nodiscard]] auto types() const noexcept -> MemoryTypes { return types_; }
    [[nodiscard]] auto state() const noexcept -> MappingState { return state_; }

private:
    friend class VSpace;
    friend class MappingAuthority;

    MappingKey key_{};
    ObjectRange object_{};
    AccessMask access_{};
    AccessMask ceiling_{};
    MemoryTypes types_{};
    MappingAuthority* authority_{};
    MappingState state_{MappingState::Preparing};
    libk::IntrusiveListHook authority_hook_{};
    UserViewRelations views_{};
};

class ReservedLeaf final : public LayoutNode {
public:
    ReservedLeaf(VirtRange range, AddressRegion& parent) noexcept
        : LayoutNode(LayoutKind::Reserved, range, &parent) {}
};

class Guard final : public LayoutNode {
public:
    Guard(VirtRange range, AddressRegion& parent) noexcept
        : LayoutNode(LayoutKind::Guard, range, &parent) {}
};

} // namespace kernel::mm
