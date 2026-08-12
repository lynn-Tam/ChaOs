#include <mm/vspace.hpp>

#include "vspace_internal.hpp"

#include <core/debug.hpp>
#include <libk/utility.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

/*luna change: centralize VSpaceError to FaultKind mapping, reason: Thread and Vproc continuations consume one canonical fault classification*/
auto fault_kind(VSpaceError error) noexcept -> FaultKind {
    switch (error) {
    case VSpaceError::Busy:
        return FaultKind::Busy;
    case VSpaceError::Pressure:
        return FaultKind::Pressure;
    case VSpaceError::OutOfMemory:
        return FaultKind::OutOfMemory;
    case VSpaceError::ResourceExhausted:
    case VSpaceError::QuotaExceeded:
        return FaultKind::ResourceExhausted;
    case VSpaceError::BackingFailed:
        return FaultKind::BackingFailed;
    default:
        return FaultKind::BackingFailed;
    }
}

auto VSpace::fault(
    VmContext context,
    VirtAddr address,
    Access access,
    WaitRelation* relation,
    void* owner,
    WaitRelation::Publish publish) noexcept
    -> libk::Expected<FaultResult, VSpaceError> {
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
        context,
        *mapping,
        page_range.base(),
        object_page,
        relation,
        owner,
        publish);
}

auto VSpace::materialize_fault(
    VmContext context,
    Mapping& mapping,
    VirtAddr page_address,
    usize object_page,
    WaitRelation* relation,
    void* owner,
    WaitRelation::Publish publish) noexcept
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
    MemoryObject* const memory = &authority.memory();
    auto resident = memory->materialize(
        object_page, relation, owner, publish);
    if (!resident) {
        if (resident.error() == MemoryError::Pending) {
            auto pending = fail(FaultKind::Pending);
            /*luna change: preserve the pinned MemoryObject identity after Pending handoff, reason: an early callback may finalize the relation before this function returns*/
            if (pending && relation != nullptr) {
                pending.value().memory = memory;
            }
            return pending;
        }
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
        page_address,
        object_page,
        libk::move(source),
        libk::move(alias).value());
    if (!made) {
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(node_error(made.error()));
    }
    MappedPage* const page = made.value().object;
    /*luna change: arm each materialized page with its authority route,
      reason: MappingAuthority owns the sole VSpace identity*/
    page->authority_ = &authority;
    page->page_mapping_.arm(page, &VSpace::invalidate_page);
    auto linked = authority.memory().bind_mapping(
        page->page_mapping_, object_page);
    if (!linked) {
        /*luna change: clear the failed page route before recycle, reason:
          PageMapping context is valid only while the MappedPage is live*/
        page->authority_ = nullptr;
        pages_.destroy(*page);
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(memory_error(linked.error()));
    }
    auto table_reserve = reserve_tables(page);
    if (!table_reserve) {
        KASSERT(authority.memory().unbind_mapping(page->page_mapping_));
        /*luna change: clear the rejected page route before recycle, reason:
          backing unlink precedes MappedPage destruction*/
        page->authority_ = nullptr;
        pages_.destroy(*page);
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(table_reserve.error());
    }
    TableReserve tables = libk::move(table_reserve).value();

    kernel::sync::IrqLockToken lock{lock_};
    Mapping* const current = mappings_.find(mapping.key_.node);
    if (current != &mapping
        || mapping.state_ != MappingState::Live
        || claim_.region != mapping.parent_
        || claim_.range != VirtRange{page_address, page_size}
        || authority.invalidation_requested_
        || authority.pages_.find(page_address) != nullptr) {
        release_claim();
        lock.restore();
        KASSERT(authority.memory().unbind_mapping(page->page_mapping_));
        /*luna change: clear the failed translation route before recycle,
          reason: no detached page may retain a VSpace callback context*/
        page->authority_ = nullptr;
        pages_.destroy(*page);
        return libk::unexpected(VSpaceError::InvalidState);
    }
    auto mutation = coherence_.begin();
    if (!mutation) {
        release_claim();
        lock.restore();
        KASSERT(authority.memory().unbind_mapping(page->page_mapping_));
        /*luna change: clear the failed shootdown route before recycle,
          reason: callback context ends with the exact page node*/
        page->authority_ = nullptr;
        pages_.destroy(*page);
        return libk::unexpected(VSpaceError::ShootdownUnavailable);
    }
    auto plan = prepare_plan(context, mutation.value().targets());
    if (!plan) {
        mutation.value().abort();
        release_claim();
        lock.restore();
        KASSERT(authority.memory().unbind_mapping(page->page_mapping_));
        /*luna change: clear the failed plan route before recycle, reason:
          page ownership is not retained after transaction abort*/
        page->authority_ = nullptr;
        pages_.destroy(*page);
        return libk::unexpected(plan.error());
    }
    arch::PageEditor editor = arch::PageEditor::user(*root_);
    const auto virtual_page = VPage::from_base(page_address);
    KASSERT(virtual_page);
    auto installed = editor.map(
        *virtual_page, page->page_, *permissions, tables.pages);
    KASSERT(installed);
    commit_tables(tables);
    page->alias_.commit();
    page->source_.reset();
    page->pending_next_ = nullptr;
    authority.pages_.insert(*page);
    pending_kind_ = PendingKind::Map;
    release_claim();
    auto& retire = retire_batch_.emplace(*pmm_);
    auto committed = commit_translation(
        libk::move(mutation).value(),
        libk::move(plan).value(),
        retire,
        mapping.access_.contains(Access::Execute));
    lock.restore();
    if (!committed) {
        return libk::unexpected(committed.error());
    }
    if (committed.value() == VmStatus::Complete) {
        finish_authorities();
    }
    return libk::expected(FaultResult{
        .kind = FaultKind::Materialized,
        .mapping = mapping.key_,
        .object_page = object_page,
        .status = committed.value(),
    });
}

auto VSpace::sample_usage(
    VmContext context,
    VirtAddr address,
    bool clear) noexcept -> libk::Expected<PageUsage, VSpaceError> {
    const usize aligned = address.raw() & ~(page_size - 1);
    const VirtRange page_range{VirtAddr{aligned}, page_size};
    if (!valid_user_range(page_range)) {
        return libk::unexpected(VSpaceError::InvalidRange);
    }

    kernel::sync::IrqLockToken lock{lock_};
    auto fail = [&](VSpaceError error)
        -> libk::Expected<PageUsage, VSpaceError> {
        lock.restore();
        return libk::unexpected(error);
    };
    if (state_ != VSpaceState::Live
        || pending_kind_ != PendingKind::None
        || claim_.region != nullptr) {
        return fail(VSpaceError::Busy);
    }

    Mapping* mapping{};
    AddressRegion* region = root_region_;
    while (region != nullptr) {
        LayoutNode* node = region->children_.lower_bound(page_range.base());
        if (node == nullptr || node->range_.base() > page_range.base()) {
            node = node != nullptr
                ? region->children_.previous(*node)
                : region->children_.maximum();
        }
        if (node == nullptr || !node->range_.contains(page_range)) {
            return fail(VSpaceError::NotMapped);
        }
        if (node->kind_ == LayoutKind::Region) {
            region = static_cast<AddressRegion*>(node);
            continue;
        }
        if (node->kind_ != LayoutKind::Mapping) {
            return fail(VSpaceError::NotMapped);
        }
        mapping = static_cast<Mapping*>(node);
        break;
    }
    if (mapping == nullptr || mapping->state_ != MappingState::Live) {
        return fail(VSpaceError::NotMapped);
    }
    const auto offset = mapping->range_.page_offset(page_range.base());
    if (!offset) {
        return fail(VSpaceError::InvalidRange);
    }
    const usize object_page = mapping->object_.first + *offset;
    MappingAuthority& authority = *mapping->authority_;
    MappedPage* const mapped = authority.pages_.find(page_range.base());
    if (mapped == nullptr) {
        return fail(VSpaceError::NotMapped);
    }
    const auto virtual_page = VPage::from_base(page_range.base());
    if (!virtual_page) {
        return fail(VSpaceError::InvalidRange);
    }
    arch::PageEditor editor = arch::PageEditor::user(*root_);
    const auto observed = editor.usage(*virtual_page);
    if (!observed) {
        return fail(VSpaceError::NotMapped);
    }
    const PageUsage usage{
        .accessed = observed.value().accessed,
        .dirty = observed.value().dirty,
    };
    auto folded = authority.memory().observe_usage(
        object_page, usage.accessed, usage.dirty);
    if (!folded) {
        return fail(memory_error(folded.error()));
    }
    if (!clear) {
        lock.restore();
        return libk::expected(usage);
    }

    auto mutation = coherence_.begin();
    if (!mutation) {
        return fail(VSpaceError::ShootdownUnavailable);
    }
    auto plan = prepare_plan(context, mutation.value().targets());
    if (!plan) {
        mutation.value().abort();
        return fail(plan.error());
    }
    const auto cleared = editor.clear_usage(*virtual_page, false, false);
    if (!cleared) {
        mutation.value().abort();
        return fail(VSpaceError::TranslationCorrupt);
    }
    pending_kind_ = PendingKind::Protect;
    auto& retire = retire_batch_.emplace(*pmm_);
    auto committed = commit_translation(
        libk::move(mutation).value(),
        libk::move(plan).value(),
        retire);
    lock.restore();
    if (!committed) {
        return libk::unexpected(committed.error());
    }
    if (committed.value() != VmStatus::Complete) {
        return libk::unexpected(VSpaceError::Busy);
    }
    return libk::expected(usage);
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
