#include <mm/vspace.hpp>
#include <mm/vspace_work.hpp>

#include "vspace_internal.hpp"

#include <mm/virtual_layout.hpp>
#include <cap/cspace.hpp>
#include <cap/resolved.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_registry.hpp>
#include <libk/limits.hpp>
#include <libk/memory.hpp>
#include <libk/utility.hpp>
#include <object/memory_pool.hpp>
#include <object/vspace_pool.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {
auto VSpace::valid_user_range(VirtRange range) noexcept -> bool {
    const auto end = range.end();
    return range.valid() && !range.empty() && end
        && (range.base().raw() & (page_size - 1)) == 0
        && (range.size() & (page_size - 1)) == 0
        && range.base().raw() >= kernel::mm::layout::LowGuardEnd
        && end->raw() <= kernel::mm::layout::UserEnd;
}

auto VSpace::overlap(AddressRegion& region, VirtRange range) noexcept
    -> LayoutNode* {
    LayoutNode* next = region.children_.lower_bound(range.base());
    if (next != nullptr && next->range_.intersects(range)) {
        return next;
    }
    LayoutNode* previous = next != nullptr
        ? region.children_.previous(*next)
        : region.children_.maximum();
    return previous != nullptr && previous->range_.intersects(range)
        ? previous
        : nullptr;
}

auto VSpace::validate_region(
    RegionKey key,
    VirtRange range) noexcept -> AddressRegion* {
    AddressRegion* const region = regions_.find(key.node);
    return region != nullptr
            && region->key_ == key
            && region->state_ == RegionState::Live
            && region->range_.contains(range)
        ? region
        : nullptr;
}

auto VSpace::validate_region(
    cap::VSpaceAuthority authority,
    VirtRange range) noexcept -> AddressRegion* {
    AddressRegion* const region = validate_region(authority.region, range);
    return region != nullptr && authority.range.contains(range)
        ? region
        : nullptr;
}

auto VSpace::begin_claim(
    AddressRegion& region,
    VirtRange range,
    bool must_be_empty) noexcept -> libk::Expected<void, VSpaceError> {
    if (state_ != VSpaceState::Live || claim_.region != nullptr
        || pending_kind_ != PendingKind::None) {
        return libk::unexpected(VSpaceError::Busy);
    }
    if (!valid_user_range(range) || !region.range_.contains(range)) {
        return libk::unexpected(VSpaceError::InvalidRange);
    }
    if (must_be_empty && overlap(region, range) != nullptr) {
        return libk::unexpected(VSpaceError::Overlap);
    }
    claim_ = RangeClaim{&region, range};
    return libk::expected();
}

void VSpace::release_claim() noexcept {
    claim_ = {};
    if (service_waiting_on_claim_) {
        service_waiting_on_claim_ = false;
        schedule_work();
    }
}

auto VSpace::create_region(
    RegionKey parent_key,
    VirtRange range,
    RegionPolicy policy) noexcept
    -> libk::Expected<RegionKey, VSpaceError> {
    AddressRegion* parent{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        parent = validate_region(parent_key, range);
        if (parent == nullptr) {
            return libk::unexpected(VSpaceError::InvalidRegion);
        }
        if (!parent->policy_.access.contains(policy.access)
            || !parent->policy_.types.contains(policy.types)
            || !valid_access(policy.access)
            || !valid_memory_types(policy.types)) {
            return libk::unexpected(VSpaceError::InvalidAuthority);
        }
        auto claimed = begin_claim(*parent, range, true);
        if (!claimed) {
            return libk::unexpected(claimed.error());
        }
    }

    auto made = regions_.create(range, parent, policy);
    if (!made) {
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(node_error(made.error()));
    }
    AddressRegion* const region = made.value().object;
    region->key_ = RegionKey{made.value().key};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(claim_.region == parent && claim_.range == range);
        KASSERT(overlap(*parent, range) == nullptr);
        parent->children_.insert(*region);
        release_claim();
    }
    return libk::expected(region->key_);
}

auto VSpace::create_region(
    kernel::cap::Resolved<VSpace>& source,
    kernel::cap::CSpace& destination,
    VirtRange range,
    RegionPolicy policy,
    kernel::cap::Rights child_rights) noexcept
    -> libk::Expected<RegionCapResult, VSpaceError> {
    if (&source.object() != this) {
        return libk::unexpected(VSpaceError::InvalidAuthority);
    }
    const cap::EffectiveAuthority effective = source.authority();
    const auto* const parent_authority =
        libk::get_if<cap::VSpaceAuthority>(&effective.data);
    if (parent_authority == nullptr
        || !effective.rights.contains(cap::Right::CreateRegion)
        || !effective.rights.contains(child_rights)
        || !parent_authority->range.contains(range)
        || !parent_authority->access.contains(policy.access)
        || !parent_authority->types.contains(policy.types)) {
        return libk::unexpected(VSpaceError::InvalidAuthority);
    }
    auto reserved = destination.reserve();
    if (!reserved) {
        return libk::unexpected(
            reserved.error() == cap::CSpaceError::OutOfMemory
                ? VSpaceError::OutOfMemory
                : VSpaceError::Busy);
    }
    cap::CSpace::Reservation slot = libk::move(reserved).value();

    AddressRegion* parent{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        parent = validate_region(*parent_authority, range);
        if (parent == nullptr) {
            return libk::unexpected(VSpaceError::InvalidRegion);
        }
        auto claimed = begin_claim(*parent, range, true);
        if (!claimed) {
            return libk::unexpected(claimed.error());
        }
    }
    auto made = regions_.create(range, parent, policy);
    if (!made) {
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(node_error(made.error()));
    }
    AddressRegion* const child = made.value().object;
    child->key_ = RegionKey{made.value().key};
    const cap::VSpaceAuthority child_authority{
        .region = child->key_,
        .range = range,
        .access = policy.access,
        .types = policy.types,
    };
    const cap::GrantCeiling ceiling{child_rights, child_authority};
    const cap::CapView child_view{child_rights, child_authority};
    auto composed = cap::compose(
        object::ObjectKind::VSpace, ceiling, child_view);
    if (!composed) {
        regions_.destroy(*child);
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(VSpaceError::InvalidAuthority);
    }
    auto target = source.reference();
    if (!target) {
        regions_.destroy(*child);
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(VSpaceError::InvalidState);
    }
    auto grant_charge = source.source().reserve_grant();
    if (!grant_charge) {
        regions_.destroy(*child);
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(VSpaceError::ResourceExhausted);
    }
    auto grant = source.derive_region(
        libk::move(grant_charge).value(),
        libk::move(target).value(),
        ceiling,
        cap::RegionDerivation{
            parent->key_, child->key_, child->range_});
    if (!grant) {
        regions_.destroy(*child);
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(VSpaceError::GrantUnavailable);
    }
    cap::GrantRef child_grant = libk::move(grant).value();
    auto child_operation = child_grant.acquire();
    if (!child_operation) {
        child_grant.reset();
        regions_.destroy(*child);
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(VSpaceError::GrantUnavailable);
    }

    // Cross-root publication uses one fixed lock order. Readers of either
    // root cannot observe a published Region without its capability slot, or
    // a slot naming a Region that is still unpublished.
    kernel::sync::IrqLockToken cspace_lock{destination.lock_};
    kernel::sync::IrqLockToken vspace_lock{lock_};
    KASSERT(claim_.region == parent && claim_.range == range);
    KASSERT(overlap(*parent, range) == nullptr);
    auto installed = destination.commit_locked(
        slot, libk::move(child_grant), child_view);
    if (installed) {
        parent->children_.insert(*child);
    }
    release_claim();
    vspace_lock.restore();
    cspace_lock.restore();
    if (!installed) {
        child_grant.reset();
        regions_.destroy(*child);
        return libk::unexpected(VSpaceError::Busy);
    }
    return libk::expected(RegionCapResult{
        child->key_, installed.value()});
}

auto VSpace::reserve(RegionKey parent_key, VirtRange range) noexcept
    -> libk::Expected<void, VSpaceError> {
    AddressRegion* parent{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        parent = validate_region(parent_key, range);
        if (parent == nullptr) {
            return libk::unexpected(VSpaceError::InvalidRegion);
        }
        auto claimed = begin_claim(*parent, range, true);
        if (!claimed) {
            return libk::unexpected(claimed.error());
        }
    }
    auto made = reservations_.create(range, *parent);
    if (!made) {
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(node_error(made.error()));
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        parent->children_.insert(*made.value().object);
        release_claim();
    }
    return libk::expected();
}

auto VSpace::guard(RegionKey parent_key, VirtRange range) noexcept
    -> libk::Expected<void, VSpaceError> {
    AddressRegion* parent{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        parent = validate_region(parent_key, range);
        if (parent == nullptr) {
            return libk::unexpected(VSpaceError::InvalidRegion);
        }
        auto claimed = begin_claim(*parent, range, true);
        if (!claimed) {
            return libk::unexpected(claimed.error());
        }
    }
    auto made = guards_.create(range, *parent);
    if (!made) {
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(node_error(made.error()));
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        parent->children_.insert(*made.value().object);
        release_claim();
    }
    return libk::expected();
}

void VSpace::dismantle_region(
    AddressRegion& target,
    arch::PageEditor& editor,
    RetireBatch& retire,
    bool remove_root) noexcept {
    AddressRegion* current = &target;
    for (;;) {
        LayoutNode* const child = current->children_.minimum();
        if (child != nullptr && child->kind_ == LayoutKind::Region) {
            auto& nested = static_cast<AddressRegion&>(*child);
            nested.state_ = RegionState::Retiring;
            current = &nested;
            continue;
        }
        if (child != nullptr) {
            current->children_.erase(*child);
            if (child->kind_ == LayoutKind::Mapping) {
                auto& mapping = static_cast<Mapping&>(*child);
                invalidate_views(mapping);
                MappingAuthority& authority = *mapping.authority_;
                MappedPage* page = authority.pages_.lower_bound(
                    mapping.range_.base());
                while (page != nullptr
                    && page->address_ < *mapping.range_.end()) {
                    MappedPage* const next = authority.pages_.next(*page);
                    authority.pages_.erase(*page);
                    const auto virtual_page = VPage::from_base(page->address_);
                    KASSERT(virtual_page);
                    auto unmapped = editor.unmap(*virtual_page);
                    KASSERT(unmapped);
                    while (auto table = unmapped.value().tables.take()) {
                        retire_table(retire, libk::move(*table));
                    }
                    queue_page(*page);
                    page = next;
                }
                mapping.state_ = MappingState::Invalidating;
            }
            queue_layout(*child);
            continue;
        }
        if (current == &target) {
            if (remove_root) {
                AddressRegion* const parent = current->parent_;
                KASSERT(parent != nullptr);
                parent->children_.erase(*current);
                current->state_ = RegionState::Dead;
                queue_layout(*current);
            }
            return;
        }
        AddressRegion* const parent = current->parent_;
        KASSERT(parent != nullptr);
        parent->children_.erase(*current);
        current->state_ = RegionState::Dead;
        queue_layout(*current);
        current = parent;
    }
}

auto VSpace::start_region_destroy(
    VmContext context,
    AddressRegion& region,
    bool remove_root,
    PendingKind kind) noexcept
    -> libk::Expected<VmStatus, VSpaceError> {
    kernel::sync::IrqLockToken lock{lock_};
    const bool permitted_state = state_ == VSpaceState::Live
        || (state_ == VSpaceState::Stopping && !remove_root);
    if (!permitted_state
        || pending_kind_ != PendingKind::None
        || claim_.region != nullptr
        || region.state_ == RegionState::Dead
        || (remove_root && &region == root_region_)) {
        return libk::unexpected(VSpaceError::Busy);
    }
    auto mutation = coherence_.begin();
    if (!mutation) {
        return libk::unexpected(VSpaceError::ShootdownUnavailable);
    }
    auto plan = prepare_plan(context, mutation.value().targets());
    if (!plan) {
        mutation.value().abort();
        return libk::unexpected(plan.error());
    }
    region.state_ = RegionState::Retiring;
    pending_kind_ = kind;
    auto& retire = retire_batch_.emplace(*pmm_);
    arch::PageEditor editor = arch::PageEditor::user(*root_);
    dismantle_region(region, editor, retire, remove_root);
    if (pending_pages_ == nullptr) {
        mutation.value().abort();
        retire_batch_.reset();
        KASSERT(finish_pending());
        lock.restore();
        finish_authorities();
        return libk::expected(VmStatus::Complete);
    }
    auto committed = commit_translation(
        libk::move(mutation).value(), libk::move(plan).value(), retire);
    lock.restore();
    if (committed && committed.value() == VmStatus::Complete) {
        finish_authorities();
    }
    return committed;
}

auto VSpace::destroy_region(
    VmContext context,
    RegionKey key) noexcept -> libk::Expected<VmStatus, VSpaceError> {
    AddressRegion* region{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        region = regions_.find(key.node);
        if (state_ != VSpaceState::Live
            || region == nullptr
            || region->key_ != key
            || region == root_region_
            || region->state_ != RegionState::Live) {
            return libk::unexpected(VSpaceError::InvalidRegion);
        }
    }
    return start_region_destroy(
        context, *region, true, PendingKind::DestroyRegion);
}

} // namespace kernel::mm
