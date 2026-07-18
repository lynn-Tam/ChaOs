#include <mm/vspace.hpp>
#include <mm/vspace_work.hpp>

#include "vspace_internal.hpp"

#include <libk/checked_arithmetic.hpp>
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
auto VSpace::prepare_plan(
    VmContext context,
    const kernel::CpuSet& targets) noexcept
    -> libk::Expected<ShootdownPlan, VSpaceError> {
    auto plan = context.cpus != nullptr
        ? ShootdownPlan::prepare(*context.cpus, context.local, targets)
        : ShootdownPlan::local(context.local, targets);
    return plan
        ? libk::Expected<ShootdownPlan, VSpaceError>{
              libk::expected(libk::move(plan).value())}
        : libk::Expected<ShootdownPlan, VSpaceError>{
              libk::unexpected(VSpaceError::ShootdownUnavailable)};
}

auto VSpace::reserve_tables(MappedPage* pages) noexcept
    -> libk::Expected<TableReserve, VSpaceError> {
    arch::PageEditor::Plan plan{};
    for (MappedPage* page = pages; page != nullptr;
         page = page->pending_next_) {
        const auto virtual_page = VPage::from_base(page->address_);
        if (!virtual_page || !plan.include(*virtual_page)) {
            return libk::unexpected(VSpaceError::TranslationCorrupt);
        }
    }
    const usize count = plan.table_pages();
    kernel::resource::Charge charge{};
    if (sponsor_ != nullptr && count != 0) {
        const auto bytes = libk::checked_multiply<u64>(
            static_cast<u64>(count), static_cast<u64>(page_size));
        if (!bytes) {
            return libk::unexpected(VSpaceError::ResourceExhausted);
        }
        auto acquired = sponsor_->acquire(kernel::resource::Budget{
            .memory = bytes.value(),
        });
        if (!acquired) {
            return libk::unexpected(VSpaceError::ResourceExhausted);
        }
        charge = libk::move(acquired).value();
    }

    OwnedPageGroup reserve = pmm_->make_page_group();
    if (!reserve.try_extend(count)) {
        return libk::unexpected(VSpaceError::OutOfMemory);
    }
    return libk::expected(TableReserve{
        libk::move(charge),
        libk::move(reserve),
    });
}

void VSpace::commit_tables(TableReserve& reserve) noexcept {
    if (reserve.charge) {
        const kernel::resource::Budget charged = reserve.charge.budget();
        KASSERT(charged.caps == 0 && charged.memory % page_size == 0);
        const usize total = static_cast<usize>(charged.memory / page_size);
        KASSERT(total >= reserve.pages.page_count());
        const usize consumed = total - reserve.pages.page_count();
        if (consumed != 0) {
            table_charge_.merge(reserve.charge.split(
                kernel::resource::Budget{
                    .memory = static_cast<u64>(consumed) * page_size,
                }));
        }
    }
    // Unconsumed prepared pages return to PMM before their capacity token.
    reserve.pages.reset();
    reserve.charge.reset();
}

void VSpace::retire_table(
    RetireBatch& retire,
    OwnedPage&& page) noexcept {
    if (!table_charge_) {
        KASSERT(retire.adopt(libk::move(page)));
        return;
    }
    auto charge = table_charge_.split(kernel::resource::Budget{
        .memory = page_size,
    });
    KASSERT(retire.adopt(libk::move(page), libk::move(charge)));
}

auto VSpace::map_kernel(
    VmContext context,
    RegionKey region_key,
    MapRequest request,
    object::ObjectRef&& memory_ref,
    MemoryObject& memory,
    cap::MemoryAuthority authority) noexcept
    -> libk::Expected<MapResult, VSpaceError> {
    AddressRegion* region{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        region = validate_region(region_key, request.virtual_range);
        if (region == nullptr) {
            return libk::unexpected(VSpaceError::InvalidRegion);
        }
        auto claimed = begin_claim(*region, request.virtual_range, true);
        if (!claimed) {
            return libk::unexpected(claimed.error());
        }
    }
    return map_impl(
        context,
        *region,
        request,
        libk::move(memory_ref),
        memory,
        authority,
        region->policy_.access,
        region->policy_.types,
        nullptr);
}

auto VSpace::map(
    VmContext context,
    cap::VSpaceAuthority where,
    MapRequest request,
    kernel::cap::Resolved<MemoryObject>& memory) noexcept
    -> libk::Expected<MapResult, VSpaceError> {
    const cap::EffectiveAuthority effective = memory.authority();
    const auto* const memory_authority =
        libk::get_if<cap::MemoryAuthority>(&effective.data);
    if (memory_authority == nullptr
        || !effective.rights.contains(cap::Right::Map)) {
        return libk::unexpected(VSpaceError::InvalidAuthority);
    }
    auto reference = memory.reference();
    if (!reference) {
        return libk::unexpected(VSpaceError::InvalidState);
    }

    AddressRegion* region{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        region = validate_region(where, request.virtual_range);
        if (region == nullptr) {
            return libk::unexpected(VSpaceError::InvalidRegion);
        }
        if (!where.access.contains(request.access)
            || !where.types.intersect(memory_authority->types).raw()) {
            return libk::unexpected(VSpaceError::InvalidAuthority);
        }
        auto claimed = begin_claim(*region, request.virtual_range, true);
        if (!claimed) {
            return libk::unexpected(claimed.error());
        }
    }
    return map_impl(
        context,
        *region,
        request,
        libk::move(reference).value(),
        memory.object(),
        *memory_authority,
        where.access,
        where.types,
        &memory);
}

auto VSpace::map_impl(
    VmContext context,
    AddressRegion& region,
    MapRequest request,
    object::ObjectRef&& memory_ref,
    MemoryObject& memory,
    cap::MemoryAuthority memory_authority,
    AccessMask vspace_access,
    MemoryTypes vspace_types,
    kernel::cap::Resolved<MemoryObject>* capability) noexcept
    -> libk::Expected<MapResult, VSpaceError> {
    const auto page_count = request.virtual_range.page_count();
    KASSERT(page_count);
    const usize count = *page_count;
    const MemoryTypes types = region.policy_.types
        .intersect(vspace_types)
        .intersect(memory_authority.types);
    const AccessMask ceiling = region.policy_.access
        .intersect(vspace_access)
        .intersect(memory_authority.access);
    const bool invalid_access = !valid_access(request.access)
        || (request.access.contains(Access::Write)
            && request.access.contains(Access::Execute));
    if (invalid_access) {
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(VSpaceError::InvalidAccess);
    }
    if (!memory_ref
        || memory_ref.kind() != object::ObjectKind::MemoryObject
        || count == 0
        || request.object.page_count != count
        || !request.object.within(memory.page_count())
        || !memory_authority.range.contains(request.object)
        || !ceiling.contains(request.access)
        || types.empty()) {
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(VSpaceError::InvalidAuthority);
    }

    const AuthoritySource source = capability != nullptr
        ? AuthoritySource::Capability
        : AuthoritySource::Kernel;
    auto authority_entry = authorities_.create(
        *this,
        libk::move(memory_ref),
        memory,
        memory_authority,
        request.access,
        source);
    if (!authority_entry) {
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(node_error(authority_entry.error()));
    }
    MappingAuthority* const authority = authority_entry.value().object;

    auto discard_authority = [&]() noexcept {
        {
            kernel::sync::IrqLockGuard guard{lock_};
            if (authority->invalidation_hook_.is_linked()) {
                invalidations_.erase(*authority);
            }
            authority->invalidation_requested_ = false;
        }
        KASSERT(authority->mappings_.empty());
        static_cast<void>(authority->detach_relations());
        KASSERT(authority->relations_released());
        authorities_.destroy(*authority);
    };

    auto memory_attached = authority->attach_memory();
    if (!memory_attached) {
        {
            kernel::sync::IrqLockGuard guard{lock_};
            release_claim();
        }
        // attach() failed before relation publication.
        authority->relations_detached_ = true;
        discard_authority();
        return libk::unexpected(memory_error(memory_attached.error()));
    }
    if (capability != nullptr) {
        auto& attachment = authority->grant_attachment_.emplace(
            authority, MappingAuthority::grant_ops_);
        auto grant_attached = capability->attach(attachment);
        if (!grant_attached) {
            {
                kernel::sync::IrqLockGuard guard{lock_};
                release_claim();
            }
            discard_authority();
            return libk::unexpected(VSpaceError::GrantUnavailable);
        }
    }

    auto mapping_entry = mappings_.create(
        request.virtual_range,
        region,
        request.object,
        request.access,
        ceiling,
        types,
        *authority);
    if (!mapping_entry) {
        {
            kernel::sync::IrqLockGuard guard{lock_};
            release_claim();
        }
        discard_authority();
        return libk::unexpected(node_error(mapping_entry.error()));
    }
    Mapping* const mapping = mapping_entry.value().object;
    mapping->key_ = MappingKey{mapping_entry.value().key};

    MappedPage* prepared_head{};
    MappedPage* prepared_tail{};
    auto discard_pages = [&]() noexcept {
        while (prepared_head != nullptr) {
            MappedPage* const page = prepared_head;
            prepared_head = page->pending_next_;
            page->pending_next_ = nullptr;
            pages_.destroy(*page);
        }
    };
    auto fail = [&](VSpaceError error)
        -> libk::Expected<MapResult, VSpaceError> {
        discard_pages();
        mappings_.destroy(*mapping);
        {
            kernel::sync::IrqLockGuard guard{lock_};
            release_claim();
        }
        discard_authority();
        return libk::unexpected(error);
    };

    for (usize index = 0; index < count; ++index) {
        const usize object_page = request.object.first + index;
        auto content = memory.query(object_page);
        if (!content) {
            return fail(memory_error(content.error()));
        }
        if (content.value() == ContentState::Zero) {
            continue;
        }
        if (content.value() == ContentState::Busy) {
            return fail(VSpaceError::Busy);
        }
        if (content.value() == ContentState::Failed) {
            return fail(VSpaceError::BackingFailed);
        }
        auto resident = memory.materialize(object_page);
        if (!resident) {
            return fail(memory_error(resident.error()));
        }
        PageLease source_page = libk::move(resident).value();
        const MemoryPage physical = source_page.page();
        if (!physical.access.contains(request.access)
            || !types.contains(physical.type)) {
            return fail(VSpaceError::InvalidAuthority);
        }
        if (!arch::PageEditor::user_permissions(
                request.access, physical.type)) {
            return fail(VSpaceError::UnsupportedMemoryType);
        }
        auto alias = kernel_->aliases().acquire(physical.page, physical.type);
        if (!alias) {
            return fail(alias.error() == AliasError::ConflictingType
                ? VSpaceError::AliasConflict
                : VSpaceError::OutOfMemory);
        }
        const VirtAddr address{
            request.virtual_range.base().raw() + index * page_size};
        auto page_entry = pages_.create(
            address,
            libk::move(source_page),
            libk::move(alias).value());
        if (!page_entry) {
            return fail(node_error(page_entry.error()));
        }
        MappedPage* const page = page_entry.value().object;
        if (prepared_tail != nullptr) {
            prepared_tail->pending_next_ = page;
        } else {
            prepared_head = page;
        }
        prepared_tail = page;
    }

    auto table_reserve = reserve_tables(prepared_head);
    if (!table_reserve) {
        return fail(table_reserve.error());
    }
    TableReserve tables = libk::move(table_reserve).value();

    const arch::InterruptState interrupts = arch::disable_interrupts();
    lock_.lock();
    if (state_ != VSpaceState::Live
        || claim_.region != &region
        || claim_.range != request.virtual_range
        || authority->invalidation_requested_
        || overlap(region, request.virtual_range) != nullptr) {
        lock_.unlock();
        arch::restore_interrupts(interrupts);
        return fail(VSpaceError::InvalidState);
    }

    if (prepared_head == nullptr) {
        mapping->state_ = MappingState::Live;
        authority->mappings_.push_back(*mapping);
        region.children_.insert(*mapping);
        release_claim();
        const MappingKey key = mapping->key_;
        lock_.unlock();
        arch::restore_interrupts(interrupts);
        return libk::expected(MapResult{key, VmStatus::Complete});
    }

    auto mutation = coherence_.begin();
    if (!mutation) {
        lock_.unlock();
        arch::restore_interrupts(interrupts);
        return fail(VSpaceError::ShootdownUnavailable);
    }
    auto plan = prepare_plan(context, mutation.value().targets());
    if (!plan) {
        mutation.value().abort();
        lock_.unlock();
        arch::restore_interrupts(interrupts);
        return fail(plan.error());
    }

    mapping->state_ = MappingState::Live;
    authority->mappings_.push_back(*mapping);
    region.children_.insert(*mapping);
    arch::PageEditor editor = arch::PageEditor::user(*root_);
    while (prepared_head != nullptr) {
        MappedPage* const page = prepared_head;
        prepared_head = page->pending_next_;
        page->pending_next_ = nullptr;
        const auto virtual_page = VPage::from_base(page->address_);
        KASSERT(virtual_page);
        const auto permissions = arch::PageEditor::user_permissions(
            request.access, page->type_);
        KASSERT(permissions);
        auto installed = editor.map(
            *virtual_page,
            page->page_,
            *permissions,
            tables.pages);
        KASSERT(installed);
        page->alias_.commit();
        page->source_.reset();
        authority->pages_.insert(*page);
    }
    commit_tables(tables);
    pending_kind_ = PendingKind::Map;
    release_claim();
    auto& retire = retire_batch_.emplace(*pmm_);
    auto committed = commit_translation(
        libk::move(mutation).value(),
        libk::move(plan).value(),
        retire,
        request.access.contains(Access::Execute));
    lock_.unlock();
    arch::restore_interrupts(interrupts);
    if (!committed) {
        return libk::unexpected(committed.error());
    }
    if (committed.value() == VmStatus::Complete) {
        finish_authorities();
    }
    return libk::expected(MapResult{mapping->key_, committed.value()});
}

} // namespace kernel::mm
