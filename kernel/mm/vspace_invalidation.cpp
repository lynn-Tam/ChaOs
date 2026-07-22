#include <mm/vspace.hpp>
#include <mm/vspace_work.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_registry.hpp>
#include <libk/utility.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

void VSpace::request_invalidation(
    MappingAuthority& authority,
    MemoryWork&& work) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(!authority.memory_work_);
        [[maybe_unused]] auto& retained =
            authority.memory_work_.emplace(libk::move(work));
        authority.invalidation_requested_ = true;
        if (!authority.invalidation_hook_.is_linked()) {
            invalidations_.push_back(authority);
        }
    }
    schedule_work();
}

void VSpace::request_invalidation(
    MappingAuthority& authority,
    cap::GrantWork&& work) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(!authority.grant_work_);
        [[maybe_unused]] auto& retained =
            authority.grant_work_.emplace(libk::move(work));
        authority.invalidation_requested_ = true;
        if (!authority.invalidation_hook_.is_linked()) {
            invalidations_.push_back(authority);
        }
    }
    schedule_work();
}

auto VSpace::start_invalidation(
    VmContext context,
    MappingAuthority& authority,
    PendingKind kind) noexcept
    -> libk::Expected<VmStatus, VSpaceError> {
    kernel::sync::IrqLockToken lock{lock_};
    if (pending_kind_ != PendingKind::None || claim_.region != nullptr) {
        return libk::unexpected(VSpaceError::Busy);
    }
    if (authority.mappings_.empty()) {
        if (authority.invalidation_hook_.is_linked()) {
            invalidations_.erase(authority);
        }
        authority.invalidation_requested_ = false;
        queue_authority(authority);
        lock.restore();
        // Relation detach and sponsored storage refund are external callbacks.
        // The authority was published to pending_authorities_ above, so the
        // unlocked drain can safely finish it without making lock_ reentrant.
        finish_authorities();
        return libk::expected(VmStatus::Complete);
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

    if (authority.invalidation_hook_.is_linked()) {
        invalidations_.erase(authority);
    }
    authority.invalidation_requested_ = false;
    for (auto current = authority.mappings_.begin();
         current != authority.mappings_.end(); ++current) {
        Mapping& mapping = *current;
        invalidate_views(mapping);
        if (mapping.layout_hook_.is_linked()) {
            mapping.parent_->children_.erase(mapping);
        }
        mapping.state_ = MappingState::Invalidating;
        queue_layout(mapping);
    }

    pending_kind_ = kind;
    auto& retire = retire_batch_.emplace(*pmm_);
    arch::PageEditor editor = arch::PageEditor::user(*root_);
    while (!authority.pages_.empty()) {
        MappedPage* const page = authority.pages_.minimum();
        KASSERT(page != nullptr);
        authority.pages_.erase(*page);
        const auto virtual_page = VPage::from_base(page->address_);
        KASSERT(virtual_page);
        auto unmapped = editor.unmap(*virtual_page);
        KASSERT(unmapped);
        while (auto table = unmapped.value().tables.take()) {
            retire_table(retire, libk::move(*table));
        }
        queue_page(*page);
    }

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

auto VSpace::service(VmContext context) noexcept -> VSpaceServiceResult {
    MappingAuthority* next{};
    AddressRegion* retire_root{};
    ShootdownTicket* waiting_ticket{};
    bool settled{};
    bool waiting{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (pending_kind_ != PendingKind::None && !finish_pending()) {
            KASSERT(ticket_);
            waiting_ticket = &*ticket_;
        }
    }
    if (waiting_ticket != nullptr) {
        KASSERT(context.cpus != nullptr);
        switch (retry_shootdowns(*context.cpus, *waiting_ticket)) {
        case ShootdownRetry::Idle:
        case ShootdownRetry::Delivered:
            transport_retries_ = 0;
            return libk::expected(VSpaceServiceState::Waiting);
        case ShootdownRetry::TransportFailure:
            ++transport_retries_;
            if (transport_retries_ >= 8) {
                return libk::unexpected(
                    VSpaceServiceError::InvariantViolation);
            }
            return libk::expected(VSpaceServiceState::Retry);
        }
        __builtin_unreachable();
    }

    // This drains external Memory/Grant relations and sponsored node storage.
    // It owns its short internal lock sections and must be entered unlocked.
    finish_authorities();
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (pending_kind_ != PendingKind::None
            || claim_.region != nullptr) {
            service_waiting_on_claim_ = claim_.region != nullptr;
            waiting = true;
        } else if (!invalidations_.empty()) {
            next = &invalidations_.front();
        } else if (state_ == VSpaceState::Stopping
            && root_region_ != nullptr
            && !root_region_->children_.empty()
            && coherence_.active_cpus().empty()) {
            retire_root = root_region_;
        } else {
            try_finish_retire();
            settled = pending_kind_ == PendingKind::None
                && invalidations_.empty()
                && pending_authorities_ == nullptr
                && state_ != VSpaceState::Stopping;
        }
    }
    complete_cleanup();
    if (waiting || (next == nullptr && retire_root == nullptr)) {
        return libk::expected(settled
            ? VSpaceServiceState::Settled
            : VSpaceServiceState::Waiting);
    }
    if (retire_root != nullptr) {
        auto started = start_region_destroy(
            context, *retire_root, false, PendingKind::Retire);
        if (!started) {
            if (started.error() == VSpaceError::Busy) {
                return libk::expected(VSpaceServiceState::Waiting);
            }
            return libk::unexpected(
                started.error() == VSpaceError::TranslationCorrupt
                    ? VSpaceServiceError::TranslationCorrupt
                    : VSpaceServiceError::ResourceExhausted);
        }
        if (started.value() == VmStatus::Complete) {
            finish_authorities();
        }
        complete_cleanup();
        return libk::expected(
            started.value() == VmStatus::Complete && !pending()
                ? VSpaceServiceState::Settled
                : VSpaceServiceState::Progress);
    }
    auto started = start_invalidation(context, *next);
    if (!started) {
        if (started.error() == VSpaceError::Busy) {
            kernel::sync::IrqLockGuard guard{lock_};
            service_waiting_on_claim_ = claim_.region != nullptr;
            return libk::expected(VSpaceServiceState::Waiting);
        }
        if (started.error() == VSpaceError::BackingFailed) {
            return libk::unexpected(VSpaceServiceError::BackingFailed);
        }
        return libk::unexpected(
            started.error() == VSpaceError::TranslationCorrupt
                ? VSpaceServiceError::TranslationCorrupt
                : VSpaceServiceError::ResourceExhausted);
    }
    if (started.value() == VmStatus::Complete) {
        // Preserve service()'s synchronous-settle contract when the complete
        // translation made its last authority detachable.  The drain itself
        // remains outside lock_.
        finish_authorities();
    }
    complete_cleanup();
    return libk::expected(
        started.value() == VmStatus::Complete && !pending()
            ? VSpaceServiceState::Settled
            : VSpaceServiceState::Progress);
}

auto VSpace::pending() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return claim_.region != nullptr
        || pending_kind_ != PendingKind::None
        || !invalidations_.empty()
        || pending_authorities_ != nullptr
        || state_ == VSpaceState::Stopping;
}

void VSpace::translation_ready() noexcept {
    schedule_work();
}

void VSpace::schedule_work() noexcept {
    KASSERT(work_ != nullptr);
    work_->submit(*this);
}

auto VSpace::work_ready() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    if (claim_.region != nullptr) {
        return false;
    }
    if (pending_kind_ != PendingKind::None) {
        return ticket_ && ticket_->complete();
    }
    return !invalidations_.empty()
        || pending_authorities_ != nullptr
        || (state_ == VSpaceState::Stopping
            && coherence_.active_cpus().empty());
}

void VSpace::try_finish_retire() noexcept {
    if (state_ != VSpaceState::Stopping
        || pending_kind_ != PendingKind::None
        || claim_.region != nullptr
        || !invalidations_.empty()
        || pending_authorities_ != nullptr
        || !coherence_.active_cpus().empty()
        || root_region_ == nullptr
        || !root_region_->children_.empty()) {
        return;
    }
    AddressRegion* const root_region = root_region_;
    root_region_ = nullptr;
    root_region->state_ = RegionState::Dead;
    regions_.destroy(*root_region);
    release_root();
    work_open_.store<libk::MemoryOrder::Release>(false);
    state_ = VSpaceState::Quiescent;
}

void VSpace::complete_cleanup() noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != VSpaceState::Quiescent || !cleanup_) {
            return;
        }
        KASSERT(!work_open_.load<libk::MemoryOrder::Acquire>());
    }
    KASSERT(work_ != nullptr);
    work_->withdraw(*this);

    object::ObjectCleanup cleanup{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!cleanup_) {
            return;
        }
        KASSERT(state_ == VSpaceState::Quiescent);
        cleanup = libk::move(*cleanup_);
        cleanup_.reset();
    }
    cleanup.complete();
}

} // namespace kernel::mm
