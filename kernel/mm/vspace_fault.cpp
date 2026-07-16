#include <mm/vspace.hpp>

#include "vspace_internal.hpp"

#include <core/debug.hpp>
#include <libk/utility.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

auto VSpace::fault(
    VmContext context,
    VirtAddr address,
    Access access) noexcept -> libk::Expected<FaultResult, VSpaceError> {
    const usize aligned = address.raw() & ~(page_size - 1);
    const VirtRange page_range{VirtAddr{aligned}, page_size};
    if (!valid_user_range(page_range)) {
        return libk::expected(FaultResult{.kind = FaultKind::NoMapping});
    }

    Mapping* mapping{};
    usize object_page{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != VSpaceState::Live
            || pending_kind_ != PendingKind::None
            || claim_.region != nullptr) {
            return libk::expected(FaultResult{.kind = FaultKind::Busy});
        }
        AddressRegion* region = root_region_;
        while (region != nullptr) {
            LayoutNode* node = region->children_.lower_bound(page_range.base());
            if (node == nullptr || node->range_.base() > page_range.base()) {
                node = node != nullptr
                    ? region->children_.previous(*node)
                    : region->children_.maximum();
            }
            if (node == nullptr || !node->range_.contains(page_range)) {
                return libk::expected(
                    FaultResult{.kind = FaultKind::NoMapping});
            }
            if (node->kind_ == LayoutKind::Region) {
                region = static_cast<AddressRegion*>(node);
                continue;
            }
            if (node->kind_ == LayoutKind::Guard) {
                return libk::expected(FaultResult{.kind = FaultKind::Guard});
            }
            if (node->kind_ != LayoutKind::Mapping) {
                return libk::expected(
                    FaultResult{.kind = FaultKind::NoMapping});
            }
            mapping = static_cast<Mapping*>(node);
            if (mapping->state_ != MappingState::Live
                || !mapping->access_.contains(access)) {
                return libk::expected(
                    FaultResult{.kind = FaultKind::AccessDenied});
            }
            const auto mapping_offset =
                mapping->range_.page_offset(page_range.base());
            KASSERT(mapping_offset);
            object_page = mapping->object_.first + *mapping_offset;
            if (mapping->authority_->pages_.find(page_range.base()) != nullptr) {
                return libk::expected(FaultResult{
                    .kind = FaultKind::Ready,
                    .mapping = mapping->key_,
                    .object_page = object_page,
                });
            }
            auto claimed = begin_claim(*region, page_range, false);
            if (!claimed) {
                return libk::expected(FaultResult{.kind = FaultKind::Busy});
            }
            break;
        }
    }
    KASSERT(mapping != nullptr);
    return materialize_fault(
        context, *mapping, page_range.base(), object_page);
}

auto VSpace::materialize_fault(
    VmContext context,
    Mapping& mapping,
    VirtAddr page_address,
    usize object_page) noexcept
    -> libk::Expected<FaultResult, VSpaceError> {
    MappingAuthority& authority = *mapping.authority_;
    auto fail = [&](FaultKind kind)
        -> libk::Expected<FaultResult, VSpaceError> {
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::expected(FaultResult{
            .kind = kind,
            .mapping = mapping.key_,
            .object_page = object_page,
        });
    };
    auto resident = authority.memory().materialize(object_page);
    if (!resident) {
        if (resident.error() == MemoryError::Busy) {
            return fail(FaultKind::Busy);
        }
        if (resident.error() == MemoryError::BackingFailed
            || resident.error() == MemoryError::NotBacked) {
            return fail(FaultKind::BackingFailed);
        }
        {
            kernel::sync::IrqLockGuard guard{lock_};
            release_claim();
        }
        return libk::unexpected(memory_error(resident.error()));
    }
    PageLease source = libk::move(resident).value();
    const MemoryPage physical = source.page();
    if (!physical.access.contains(mapping.access_)
        || !mapping.types_.contains(physical.type)) {
        return fail(FaultKind::AccessDenied);
    }
    const auto permissions = arch::PageEditor::user_permissions(
        mapping.access_, physical.type);
    if (!permissions) {
        {
            kernel::sync::IrqLockGuard guard{lock_};
            release_claim();
        }
        return libk::unexpected(VSpaceError::UnsupportedMemoryType);
    }
    auto alias = kernel_->aliases().acquire(physical.page, physical.type);
    if (!alias) {
        {
            kernel::sync::IrqLockGuard guard{lock_};
            release_claim();
        }
        return libk::unexpected(alias.error() == AliasError::ConflictingType
            ? VSpaceError::AliasConflict
            : VSpaceError::OutOfMemory);
    }
    auto made = pages_.create(
        page_address, libk::move(source), libk::move(alias).value());
    if (!made) {
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(node_error(made.error()));
    }
    MappedPage* const page = made.value().object;
    auto table_reserve = reserve_tables(page);
    if (!table_reserve) {
        pages_.destroy(*page);
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(table_reserve.error());
    }
    OwnedPageGroup tables = libk::move(table_reserve).value();

    const arch::InterruptState interrupts = arch::disable_interrupts();
    lock_.lock();
    Mapping* const current = mappings_.find(mapping.key_.node);
    if (current != &mapping
        || mapping.state_ != MappingState::Live
        || claim_.region != mapping.parent_
        || claim_.range != VirtRange{page_address, page_size}
        || authority.invalidation_requested_
        || authority.pages_.find(page_address) != nullptr) {
        release_claim();
        lock_.unlock();
        arch::restore_interrupts(interrupts);
        pages_.destroy(*page);
        return libk::unexpected(VSpaceError::InvalidState);
    }
    auto mutation = coherence_.begin();
    if (!mutation) {
        release_claim();
        lock_.unlock();
        arch::restore_interrupts(interrupts);
        pages_.destroy(*page);
        return libk::unexpected(VSpaceError::ShootdownUnavailable);
    }
    auto plan = prepare_plan(context, mutation.value().targets());
    if (!plan) {
        mutation.value().abort();
        release_claim();
        lock_.unlock();
        arch::restore_interrupts(interrupts);
        pages_.destroy(*page);
        return libk::unexpected(plan.error());
    }
    arch::PageEditor editor = arch::PageEditor::user(*root_);
    const auto virtual_page = VPage::from_base(page_address);
    KASSERT(virtual_page);
    auto installed = editor.map(
        *virtual_page, page->page_, *permissions, tables);
    KASSERT(installed);
    tables.reset();
    page->alias_.commit();
    page->source_.reset();
    page->pending_next_ = nullptr;
    authority.pages_.insert(*page);
    pending_kind_ = PendingKind::Map;
    release_claim();
    auto& retire = retire_batch_.emplace(*pmm_);
    auto committed = commit_translation(
        libk::move(mutation).value(), libk::move(plan).value(), retire);
    lock_.unlock();
    arch::restore_interrupts(interrupts);
    if (!committed) {
        return libk::unexpected(committed.error());
    }
    return libk::expected(FaultResult{
        .kind = FaultKind::Materialized,
        .mapping = mapping.key_,
        .object_page = object_page,
        .status = committed.value(),
    });
}

auto VSpace::inspect(MappingKey key) const noexcept
    -> libk::Expected<MappingInfo, VSpaceError> {
    kernel::sync::IrqLockGuard guard{lock_};
    Mapping* const mapping = const_cast<NodePool<Mapping>&>(mappings_)
        .find(key.node);
    if (mapping == nullptr || mapping->key_ != key
        || mapping->state_ == MappingState::Detached) {
        return libk::unexpected(VSpaceError::InvalidMapping);
    }
    return libk::expected(MappingInfo{
        .key = mapping->key_,
        .region = mapping->parent_->key_,
        .range = mapping->range_,
        .object = mapping->object_,
        .access = mapping->access_,
        .ceiling = mapping->ceiling_,
        .types = mapping->types_,
        .state = mapping->state_,
        .source = mapping->authority_->source_,
    });
}

} // namespace kernel::mm
