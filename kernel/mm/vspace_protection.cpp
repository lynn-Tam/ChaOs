#include <mm/vspace.hpp>
#include <mm/vspace_work.hpp>

#include "vspace_internal.hpp"

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

auto VSpace::protect(
    VmContext context,
    cap::VSpaceAuthority where,
    VirtRange range,
    AccessMask access) noexcept
    -> libk::Expected<VmStatus, VSpaceError> {
    if (!where.access.contains(access)) {
        return libk::unexpected(VSpaceError::InvalidAuthority);
    }
    AddressRegion* region{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        region = validate_region(where, range);
        if (region == nullptr) {
            return libk::unexpected(VSpaceError::InvalidRegion);
        }
        auto claimed = begin_claim(*region, range, false);
        if (!claimed) {
            return libk::unexpected(claimed.error());
        }
    }
    return protect_impl(context, *region, range, access);
}

auto VSpace::protect_kernel(
    VmContext context,
    RegionKey region_key,
    VirtRange range,
    AccessMask access) noexcept
    -> libk::Expected<VmStatus, VSpaceError> {
    AddressRegion* region{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        region = validate_region(region_key, range);
        if (region == nullptr) {
            return libk::unexpected(VSpaceError::InvalidRegion);
        }
        auto claimed = begin_claim(*region, range, false);
        if (!claimed) {
            return libk::unexpected(claimed.error());
        }
    }
    return protect_impl(context, *region, range, access);
}

auto VSpace::protect_impl(
    VmContext context,
    AddressRegion& region,
    VirtRange range,
    AccessMask access) noexcept
    -> libk::Expected<VmStatus, VSpaceError> {
    const bool invalid_access = !valid_access(access)
        || (access.contains(Access::Write)
            && access.contains(Access::Execute));
    if (invalid_access || !region.policy_.access.contains(access)) {
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(invalid_access
            ? VSpaceError::InvalidAccess
            : VSpaceError::InvalidAuthority);
    }

    Mapping* left_source{};
    Mapping* right_source{};
    bool view_conflict{};
    bool unsupported_type{};
    auto find_coverage = [&]() noexcept -> bool {
        LayoutNode* node = region.children_.lower_bound(range.base());
        if (node == nullptr || node->range_.base() > range.base()) {
            node = node != nullptr
                ? region.children_.previous(*node)
                : region.children_.maximum();
        }
        usize cursor = range.base().raw();
        const usize limit = range.end()->raw();
        left_source = nullptr;
        right_source = nullptr;
        view_conflict = false;
        unsupported_type = false;
        while (cursor < limit) {
            if (node == nullptr
                || node->kind_ != LayoutKind::Mapping
                || node->range_.base().raw() > cursor
                || node->range_.end()->raw() <= cursor) {
                return false;
            }
            auto& mapping = static_cast<Mapping&>(*node);
            if (mapping.state_ != MappingState::Live
                || !mapping.ceiling_.contains(access)) {
                return false;
            }
            if (!mapping.views_.empty()) {
                view_conflict = true;
                return false;
            }
            MappingAuthority& authority = *mapping.authority_;
            const VirtAddr first = mapping.range_.base() < range.base()
                ? range.base() : mapping.range_.base();
            const VirtAddr last = *mapping.range_.end() < *range.end()
                ? *mapping.range_.end() : *range.end();
            for (MappedPage* page = authority.pages_.lower_bound(first);
                 page != nullptr && page->address_ < last;
                 page = authority.pages_.next(*page)) {
                if (!page->access_.contains(access)) {
                    return false;
                }
                if (!arch::PageEditor::user_permissions(
                        access, page->type_)) {
                    unsupported_type = true;
                    return false;
                }
            }
            if (left_source == nullptr) {
                left_source = &mapping;
            }
            right_source = &mapping;
            cursor = node->range_.end()->raw() < limit
                ? node->range_.end()->raw()
                : limit;
            node = cursor < limit ? region.children_.next(*node) : nullptr;
        }
        return true;
    };
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (claim_.region != &region || claim_.range != range
            || !find_coverage()) {
            release_claim();
            return libk::unexpected(
                view_conflict ? VSpaceError::Busy
                : unsupported_type ? VSpaceError::UnsupportedMemoryType
                : VSpaceError::InvalidAccess);
        }
    }

    Mapping* fragments[4]{};
    bool selected[4]{};
    usize fragment_count{};
    auto append_fragment = [&](Mapping& source, VirtRange fragment,
                               AccessMask fragment_access, bool is_selected)
        -> libk::Expected<void, VSpaceError> {
        KASSERT(fragment_count < 4);
        auto made = make_fragment(source, fragment, fragment_access);
        if (!made) {
            return libk::unexpected(made.error());
        }
        fragments[fragment_count] = made.value();
        selected[fragment_count] = is_selected;
        ++fragment_count;
        return libk::expected();
    };
    auto split_boundary = [&](Mapping& source, VirtRange changed)
        -> libk::Expected<void, VSpaceError> {
        if (source.range_ == changed) {
            return libk::expected();
        }
        if (source.range_.base() < changed.base()) {
            auto made = append_fragment(
                source,
                VirtRange{
                    source.range_.base(),
                    changed.base().raw() - source.range_.base().raw()},
                source.access_,
                false);
            if (!made) {
                return made;
            }
        }
        auto middle = append_fragment(source, changed, access, true);
        if (!middle) {
            return middle;
        }
        if (*changed.end() < *source.range_.end()) {
            return append_fragment(
                source,
                VirtRange{
                    *changed.end(),
                    source.range_.end()->raw() - changed.end()->raw()},
                source.access_,
                false);
        }
        return libk::expected();
    };
    auto discard_fragments = [&]() noexcept {
        for (usize index = 0; index < fragment_count; ++index) {
            mappings_.destroy(*fragments[index]);
        }
        fragment_count = 0;
    };

    if (left_source == right_source) {
        auto split = split_boundary(*left_source, range);
        if (!split) {
            discard_fragments();
            kernel::sync::IrqLockGuard guard{lock_};
            release_claim();
            return libk::unexpected(split.error());
        }
    } else {
        const VirtRange left_changed{
            range.base(),
            left_source->range_.end()->raw() - range.base().raw()};
        const VirtRange right_changed{
            right_source->range_.base(),
            range.end()->raw() - right_source->range_.base().raw()};
        auto split = split_boundary(*left_source, left_changed);
        if (split) {
            split = split_boundary(*right_source, right_changed);
        }
        if (!split) {
            discard_fragments();
            kernel::sync::IrqLockGuard guard{lock_};
            release_claim();
            return libk::unexpected(split.error());
        }
    }

    kernel::sync::IrqLockToken lock{lock_};
    if (claim_.region != &region || claim_.range != range
        || !find_coverage()) {
        release_claim();
        lock.restore();
        discard_fragments();
        return libk::unexpected(view_conflict
            ? VSpaceError::Busy
            : unsupported_type ? VSpaceError::UnsupportedMemoryType
            : VSpaceError::InvalidAccess);
    }
    auto mutation = coherence_.begin();
    if (!mutation) {
        release_claim();
        lock.restore();
        discard_fragments();
        return libk::unexpected(VSpaceError::ShootdownUnavailable);
    }
    auto plan = prepare_plan(context, mutation.value().targets());
    if (!plan) {
        mutation.value().abort();
        release_claim();
        lock.restore();
        discard_fragments();
        return libk::unexpected(plan.error());
    }

    pending_kind_ = PendingKind::Protect;
    auto& retire = retire_batch_.emplace(*pmm_);
    arch::PageEditor editor = arch::PageEditor::user(*root_);
    LayoutNode* node = region.children_.lower_bound(range.base());
    if (node == nullptr || node->range_.base() > range.base()) {
        node = node != nullptr
            ? region.children_.previous(*node)
            : region.children_.maximum();
    }
    while (node != nullptr && node->range_.base() < *range.end()) {
        LayoutNode* const next = region.children_.next(*node);
        auto& mapping = static_cast<Mapping&>(*node);
        const bool boundary_replaced =
            (&mapping == left_source && mapping.range_.base() < range.base())
            || (&mapping == right_source
                && *mapping.range_.end() > *range.end());
        if (boundary_replaced) {
            region.children_.erase(mapping);
            mapping.state_ = MappingState::Invalidating;
            queue_layout(mapping);
        } else {
            mapping.access_ = access;
            mapping.state_ = MappingState::Protecting;
            mapping.pending_next_ = pending_protected_;
            pending_protected_ = &mapping;
        }
        node = next;
    }
    for (usize index = 0; index < fragment_count; ++index) {
        Mapping* const fragment = fragments[index];
        fragment->authority_->mappings_.push_back(*fragment);
        region.children_.insert(*fragment);
        if (selected[index]) {
            fragment->state_ = MappingState::Protecting;
            fragment->pending_next_ = pending_protected_;
            pending_protected_ = fragment;
        } else {
            fragment->state_ = MappingState::Live;
        }
        fragments[index] = nullptr;
    }
    fragment_count = 0;

    // Publish the canonical access policy before widening any hardware PTE.
    // Remote CPUs may retain an older, narrower translation until shootdown,
    // but the projection never grants authority absent from the layout model.
    bool changed_pte{};
    node = region.children_.lower_bound(range.base());
    if (node == nullptr || node->range_.base() > range.base()) {
        node = node != nullptr
            ? region.children_.previous(*node)
            : region.children_.maximum();
    }
    while (node != nullptr && node->range_.base() < *range.end()) {
        auto& mapping = static_cast<Mapping&>(*node);
        MappingAuthority& authority = *mapping.authority_;
        const VirtAddr first = mapping.range_.base() < range.base()
            ? range.base() : mapping.range_.base();
        const VirtAddr last = *mapping.range_.end() < *range.end()
            ? *mapping.range_.end() : *range.end();
        for (MappedPage* page = authority.pages_.lower_bound(first);
             page != nullptr && page->address_ < last;
             page = authority.pages_.next(*page)) {
            const auto virtual_page = VPage::from_base(page->address_);
            KASSERT(virtual_page);
            const auto permissions = arch::PageEditor::user_permissions(
                access, page->type_);
            KASSERT(permissions);
            auto changed = editor.protect(*virtual_page, *permissions);
            KASSERT(changed);
            changed_pte = true;
        }
        node = region.children_.next(*node);
    }
    release_claim();

    if (!changed_pte) {
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

} // namespace kernel::mm
