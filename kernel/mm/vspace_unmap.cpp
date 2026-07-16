#include <mm/vspace.hpp>
#include <mm/vspace_work.hpp>

#include "vspace_internal.hpp"

#include <arch/address_layout.hpp>
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

auto VSpace::make_fragment(
    Mapping& source,
    VirtRange range,
    AccessMask access) noexcept
    -> libk::Expected<Mapping*, VSpaceError> {
    KASSERT(source.range_.contains(range));
    const auto page_offset = source.range_.page_offset(range.base());
    KASSERT(page_offset);
    const ObjectRange object{
        source.object_.first + *page_offset, *range.page_count()};
    auto made = mappings_.create(
        range,
        *source.parent_,
        object,
        access,
        source.ceiling_,
        source.types_,
        *source.authority_);
    if (!made) {
        return libk::unexpected(node_error(made.error()));
    }
    Mapping* const mapping = made.value().object;
    mapping->key_ = MappingKey{made.value().key};
    return libk::expected(mapping);
}

auto VSpace::split_for_unmap(
    Mapping& source,
    VirtRange removed,
    Mapping*& first,
    Mapping*& second) noexcept -> libk::Expected<void, VSpaceError> {
    first = nullptr;
    second = nullptr;
    const VirtAddr source_begin = source.range_.base();
    const VirtAddr source_end = *source.range_.end();
    const VirtAddr removed_begin = removed.base();
    const VirtAddr removed_end = *removed.end();
    if (source_begin < removed_begin) {
        auto left = make_fragment(
            source,
            VirtRange{source_begin, removed_begin.raw() - source_begin.raw()},
            source.access_);
        if (!left) {
            return libk::unexpected(left.error());
        }
        first = left.value();
    }
    if (removed_end < source_end) {
        auto right = make_fragment(
            source,
            VirtRange{removed_end, source_end.raw() - removed_end.raw()},
            source.access_);
        if (!right) {
            if (first != nullptr) {
                mappings_.destroy(*first);
                first = nullptr;
            }
            return libk::unexpected(right.error());
        }
        if (first == nullptr) {
            first = right.value();
        } else {
            second = right.value();
        }
    }
    return libk::expected();
}

auto VSpace::unmap(
    VmContext context,
    cap::VSpaceAuthority where,
    VirtRange range) noexcept -> libk::Expected<VmStatus, VSpaceError> {
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
    return unmap_impl(context, *region, range);
}

auto VSpace::unmap_kernel(
    VmContext context,
    RegionKey region_key,
    VirtRange range) noexcept -> libk::Expected<VmStatus, VSpaceError> {
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
    return unmap_impl(context, *region, range);
}

auto VSpace::unmap_impl(
    VmContext context,
    AddressRegion& region,
    VirtRange range) noexcept -> libk::Expected<VmStatus, VSpaceError> {
    Mapping* left_source{};
    Mapping* right_source{};
    bool ipc_conflict{};
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
        ipc_conflict = false;
        while (cursor < limit) {
            if (node == nullptr
                || node->kind_ != LayoutKind::Mapping
                || node->range_.base().raw() > cursor
                || node->range_.end()->raw() <= cursor) {
                return false;
            }
            auto& mapping = static_cast<Mapping&>(*node);
            if (mapping.state_ != MappingState::Live) {
                return false;
            }
            if (!mapping.ipc_relations_.empty()) {
                ipc_conflict = true;
                return false;
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
            return libk::unexpected(ipc_conflict
                ? VSpaceError::Busy
                : VSpaceError::NotMapped);
        }
    }

    Mapping* left_fragment{};
    Mapping* unused{};
    Mapping* right_fragment{};
    if (left_source == right_source) {
        auto split = split_for_unmap(
            *left_source, range, left_fragment, right_fragment);
        if (!split) {
            kernel::sync::IrqLockGuard guard{lock_};
            release_claim();
            return libk::unexpected(split.error());
        }
    } else {
        auto left_split = split_for_unmap(
            *left_source,
            VirtRange{
                range.base(),
                left_source->range_.end()->raw() - range.base().raw()},
            left_fragment,
            unused);
        KASSERT(unused == nullptr);
        if (!left_split) {
            kernel::sync::IrqLockGuard guard{lock_};
            release_claim();
            return libk::unexpected(left_split.error());
        }
        auto right_split = split_for_unmap(
            *right_source,
            VirtRange{
                right_source->range_.base(),
                range.end()->raw() - right_source->range_.base().raw()},
            right_fragment,
            unused);
        KASSERT(unused == nullptr);
        if (!right_split) {
            if (left_fragment != nullptr) {
                mappings_.destroy(*left_fragment);
            }
            kernel::sync::IrqLockGuard guard{lock_};
            release_claim();
            return libk::unexpected(right_split.error());
        }
    }

    auto discard_fragments = [&]() noexcept {
        if (left_fragment != nullptr) {
            mappings_.destroy(*left_fragment);
            left_fragment = nullptr;
        }
        if (right_fragment != nullptr) {
            mappings_.destroy(*right_fragment);
            right_fragment = nullptr;
        }
    };

    const arch::InterruptState interrupts = arch::disable_interrupts();
    lock_.lock();
    if (claim_.region != &region || claim_.range != range
        || !find_coverage()) {
        release_claim();
        lock_.unlock();
        arch::restore_interrupts(interrupts);
        discard_fragments();
        return libk::unexpected(ipc_conflict
            ? VSpaceError::Busy
            : VSpaceError::NotMapped);
    }
    auto mutation = coherence_.begin();
    if (!mutation) {
        release_claim();
        lock_.unlock();
        arch::restore_interrupts(interrupts);
        discard_fragments();
        return libk::unexpected(VSpaceError::ShootdownUnavailable);
    }
    auto plan = prepare_plan(context, mutation.value().targets());
    if (!plan) {
        mutation.value().abort();
        release_claim();
        lock_.unlock();
        arch::restore_interrupts(interrupts);
        discard_fragments();
        return libk::unexpected(plan.error());
    }

    pending_kind_ = PendingKind::Unmap;
    auto& retire = retire_batch_.emplace(*pmm_);
    arch::PageEditor editor = arch::PageEditor::user(*root_);
    KASSERT(pending_layout_ == nullptr);
    LayoutNode* node = region.children_.lower_bound(range.base());
    if (node == nullptr || node->range_.base() > range.base()) {
        node = node != nullptr
            ? region.children_.previous(*node)
            : region.children_.maximum();
    }
    while (node != nullptr && node->range_.base() < *range.end()) {
        LayoutNode* const next = region.children_.next(*node);
        auto& mapping = static_cast<Mapping&>(*node);
        region.children_.erase(mapping);
        mapping.state_ = MappingState::Invalidating;
        queue_layout(mapping);
        node = next;
    }
    auto publish_fragment = [&](Mapping*& fragment) noexcept {
        if (fragment == nullptr) {
            return;
        }
        fragment->state_ = MappingState::Live;
        fragment->authority_->mappings_.push_back(*fragment);
        region.children_.insert(*fragment);
        fragment = nullptr;
    };
    publish_fragment(left_fragment);
    publish_fragment(right_fragment);

    // The layout tree is the semantic owner. Remove every selected mapping
    // there first, then retire its hardware projection and alias claims.
    for (LayoutNode* removed = pending_layout_;
         removed != nullptr;
         removed = removed->pending_next_) {
        KASSERT(removed->kind_ == LayoutKind::Mapping);
        auto& mapping = static_cast<Mapping&>(*removed);
        const VirtAddr first = mapping.range_.base() < range.base()
            ? range.base() : mapping.range_.base();
        const VirtAddr last = *mapping.range_.end() < *range.end()
            ? *mapping.range_.end() : *range.end();
        MappingAuthority& authority = *mapping.authority_;
        MappedPage* page = authority.pages_.lower_bound(first);
        while (page != nullptr && page->address_ < last) {
            MappedPage* const next_page = authority.pages_.next(*page);
            authority.pages_.erase(*page);
            const auto virtual_page = VPage::from_base(page->address_);
            KASSERT(virtual_page);
            auto unmapped = editor.unmap(*virtual_page);
            KASSERT(unmapped);
            while (auto table = unmapped.value().tables.take()) {
                KASSERT(retire.adopt(libk::move(*table)));
            }
            queue_page(*page);
            page = next_page;
        }
    }
    release_claim();

    if (pending_pages_ == nullptr) {
        mutation.value().abort();
        retire_batch_.reset();
        KASSERT(finish_pending());
        lock_.unlock();
        arch::restore_interrupts(interrupts);
        return libk::expected(VmStatus::Complete);
    }
    auto committed = commit_translation(
        libk::move(mutation).value(), libk::move(plan).value(), retire);
    lock_.unlock();
    arch::restore_interrupts(interrupts);
    return committed;
}

} // namespace kernel::mm
