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

/*luna change: route one claimed PageMapping through the existing VSpace
  executor, reason: exact PTE removal must stay with its layout owner*/
void VSpace::invalidate_page(void* context, MemoryWork&& work) noexcept {
    auto& page = *static_cast<MappedPage*>(context);
    KASSERT(page.authority_ != nullptr && page.authority_->owner_ != nullptr);
    page.authority_->owner_->request_page_invalidation(
        page, libk::move(work));
}

/*luna change: retain one exact page token on its authority, reason: the
  existing invalidation list is the sole bounded VSpace work membership*/
void VSpace::request_page_invalidation(
    MappedPage& page,
    MemoryWork&& work) noexcept {
    MappingAuthority* const authority = page.authority_;
    KASSERT(authority != nullptr && authority->owner_ == this);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(!page.reclaim_work_);
        if (authority->pages_.find(page.address_) == &page) {
            KASSERT(authority->reclaim_page_ == nullptr);
            [[maybe_unused]] auto& retained =
                page.reclaim_work_.emplace(libk::move(work));
            authority->reclaim_page_ = &page;
            if (!authority->invalidation_hook_.is_linked()) {
                invalidations_.push_back(*authority);
            }
        } else {
            KASSERT(pending_kind_ != PendingKind::None);
            KASSERT(authority->reclaim_page_ == nullptr
                || authority->reclaim_page_ == &page);
            [[maybe_unused]] auto& retained =
                page.reclaim_work_.emplace(libk::move(work));
            authority->reclaim_page_ = &page;
            if (!authority->invalidation_requested_
                && authority->invalidation_hook_.is_linked()) {
                invalidations_.erase(*authority);
            }
        }
    }
    schedule_work();
}

/*luna change: remove only the claimed page PTE, reason: MappingAuthority and
  layout remain live for later rematerialization*/
auto VSpace::start_page_invalidation(
    VmContext context,
    MappingAuthority& authority) noexcept
    -> libk::Expected<VmStatus, VSpaceError> {
    kernel::sync::IrqLockToken lock{lock_};
    if (pending_kind_ != PendingKind::None || claim_.region != nullptr
        || authority.reclaim_page_ == nullptr) {
        return libk::unexpected(VSpaceError::Busy);
    }
    MappedPage& page = *authority.reclaim_page_;
    if (authority.pages_.find(page.address_) != &page) {
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
    if (authority.invalidation_hook_.is_linked()) {
        invalidations_.erase(authority);
    }
    authority.pages_.erase(page);
    const auto virtual_page = VPage::from_base(page.address_);
    KASSERT(virtual_page);
    auto& retire = retire_batch_.emplace(*pmm_);
    arch::PageEditor editor = arch::PageEditor::user(*root_);
    auto unmapped = editor.unmap(*virtual_page);
    KASSERT(unmapped);
    while (auto table = unmapped.value().tables.take()) {
        retire_table(retire, libk::move(*table));
    }
    pending_kind_ = PendingKind::PageInvalidate;
    queue_page(page);
    auto committed = commit_translation(
        libk::move(mutation).value(), libk::move(plan).value(), retire);
    lock.restore();
    return committed;
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
    ensure_observation();
    const auto key = diag::concurrency::ObservationKey{
        observation_key_.load<libk::MemoryOrder::Acquire>()};
    auto observation = diag::concurrency::ObservationLease::borrow(key);
    if (observation) {
        // The phase is intentionally coarse here.  pending_kind_ belongs to
        // the VSpace state guarded below; publishing it needs the same
        // snapshot as the other service fields.
        observation.attempt(
            0,
            diag::concurrency::WaitKind::VSpaceWork,
            diag::concurrency::NodeRef::external(reinterpret_cast<u64>(this)));
    }
    const auto traced = [this](VSpaceServiceState result) noexcept
        -> VSpaceServiceResult {
        publish_observation(result);
        return libk::expected(result);
    };
    MappingAuthority* next{};
    bool retire_root{};
    ShootdownTicket* waiting_ticket{};
    bool settled{};
    bool waiting{};
    /*luna change: select exact-page work from relation membership, reason:
      no convenience reclaim flag may become a second invalidation truth*/
    bool page_invalidate{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (pending_kind_ != PendingKind::None && !finish_pending()) {
            if (ticket_) {
                waiting_ticket = &*ticket_;
            } else {
                waiting = true;
            }
        }
    }
    if (waiting_ticket != nullptr) {
        KASSERT(context.cpus != nullptr);
        switch (retry_shootdowns(*context.cpus, *waiting_ticket)) {
        case ShootdownRetry::Idle:
        case ShootdownRetry::Delivered:
            transport_retries_ = 0;
            return traced(VSpaceServiceState::Waiting);
        case ShootdownRetry::TransportFailure:
            ++transport_retries_;
            if (transport_retries_ >= 8) {
                publish_observation(VSpaceServiceState::Retry);
                return libk::unexpected(
                    VSpaceServiceError::InvariantViolation);
            }
            return traced(VSpaceServiceState::Retry);
        }
        __builtin_unreachable();
    }
    if (waiting) {
        /*luna change: keep pending authority storage alive until the claim
          publisher wakes this service, reason: finish_authorities cannot run
          while pending_pages_ still owns the MappedPage*/
        return traced(VSpaceServiceState::Waiting);
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
            page_invalidate = next->reclaim_page_ != nullptr
                && !next->invalidation_requested_;
        } else if (state_ == VSpaceState::Stopping
            && root_region_ != nullptr
            && !root_region_->children_.empty()
            && coherence_.active_cpus().empty()) {
            // The root is resolved again by start_region_destroy() under the
            // VSpace lock.  Do not carry its address through this unlocked
            // service handoff.
            retire_root = true;
        } else {
            try_finish_retire();
            settled = pending_kind_ == PendingKind::None
                && invalidations_.empty()
                && pending_authorities_ == nullptr
                && state_ != VSpaceState::Stopping;
        }
    }
    complete_cleanup();
    if (waiting || (next == nullptr && !retire_root)) {
        return traced(settled
            ? VSpaceServiceState::Settled
            : VSpaceServiceState::Waiting);
    }
    if (retire_root) {
        auto started = start_region_destroy(
            context, RegionKey{}, false, PendingKind::Retire, true);
        if (!started) {
            if (started.error() == VSpaceError::Busy) {
                return traced(VSpaceServiceState::Waiting);
            }
            publish_observation(VSpaceServiceState::Retry);
            return libk::unexpected(
                started.error() == VSpaceError::TranslationCorrupt
                    ? VSpaceServiceError::TranslationCorrupt
                    : VSpaceServiceError::ResourceExhausted);
        }
        if (started.value() == VmStatus::Complete) {
            finish_authorities();
        }
        complete_cleanup();
        return traced(
            started.value() == VmStatus::Complete && !pending()
                ? VSpaceServiceState::Settled
                : VSpaceServiceState::Progress);
    }
    auto started = page_invalidate
        ? start_page_invalidation(context, *next)
        : start_invalidation(context, *next);
    if (!started) {
        if (started.error() == VSpaceError::Busy) {
            kernel::sync::IrqLockGuard guard{lock_};
            service_waiting_on_claim_ = claim_.region != nullptr;
            return traced(VSpaceServiceState::Waiting);
        }
        if (started.error() == VSpaceError::BackingFailed) {
            publish_observation(VSpaceServiceState::Retry);
            return libk::unexpected(VSpaceServiceError::BackingFailed);
        }
        publish_observation(VSpaceServiceState::Retry);
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
    return traced(
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
    ensure_observation();
    const auto key = diag::concurrency::ObservationKey{
        observation_key_.load<libk::MemoryOrder::Acquire>()};
    auto observation = diag::concurrency::ObservationLease::borrow(key);
    observation.touch();
    work_->submit(*this);
}

void VSpace::ensure_observation() noexcept {
    bool expected = false;
    if (!observation_reserved_.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, true)) {
        return;
    }
    // VSpace storage can be recycled after a previous terminal generation.
    // The reservation gate is local ownership; the published key is reset
    // independently before this attempt so capacity loss cannot expose stale
    // identity.
    observation_key_.store<libk::MemoryOrder::Relaxed>(0);
    auto observation = diag::concurrency::ObservationLease::reserve(
        diag::concurrency::RecordKind::VSpaceWork,
        reinterpret_cast<u64>(this),
        1,
        diag::concurrency::Expectation::InternalFinite);
    if (!observation) {
        // Capacity is optional diagnostic state.  A failed first attempt must
        // not become a permanent one-shot decision; a later schedule after a
        // slot is released may still establish the observation.
        observation_reserved_.store<libk::MemoryOrder::Release>(false);
        observation_key_.store<libk::MemoryOrder::Release>(0);
        return;
    }
    observation.watch(true);
    observation_key_.store<libk::MemoryOrder::Release>(
        observation.detach_key().raw);
}

void VSpace::publish_observation(VSpaceServiceState result) noexcept {
    const auto key = diag::concurrency::ObservationKey{
        observation_key_.load<libk::MemoryOrder::Acquire>()};
    auto observation = diag::concurrency::ObservationLease::borrow(key);
    if (!observation) {
        return;
    }
    VSpaceState state{};
    PendingKind pending{};
    bool claim{};
    bool invalidations{};
    bool authorities{};
    usize active{};
    usize retries{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        state = state_;
        pending = pending_kind_;
        claim = claim_.region != nullptr;
        invalidations = !invalidations_.empty();
        authorities = pending_authorities_ != nullptr;
        active = coherence_.active_cpus().size();
        retries = transport_retries_;
    }
    using Wait = diag::concurrency::WaitKind;
    const Wait wait = pending != PendingKind::None
        ? (active != 0 ? Wait::VSpaceShootdown : Wait::VSpaceWork)
        : claim
            ? Wait::VSpaceClaim
            : authorities
                ? Wait::VSpaceAuthorityDrain
                : active != 0
                    ? Wait::VSpaceActiveCpus
                    : (invalidations ? Wait::VSpaceWork : Wait::None);
    u64 stamp = static_cast<u64>(state);
    stamp = stamp * 17 + static_cast<u64>(pending);
    stamp = stamp * 17 + claim;
    stamp = stamp * 17 + invalidations;
    stamp = stamp * 17 + authorities;
    stamp = stamp * 17 + active;
    stamp = stamp * 17 + retries;
    diag::concurrency::ObservationBatch update{
        .phase = static_cast<u32>(result),
        .semantic_stamp = stamp,
        .wait = wait,
        .driver = diag::concurrency::NodeRef::external(
            reinterpret_cast<u64>(this)),
        .blocker = active == 0
            ? diag::concurrency::NodeRef{}
            : diag::concurrency::NodeRef::external(
                  reinterpret_cast<u64>(this), active),
        .site = diag::concurrency::SourceSite::current(),
        .detail_mask = 0xfU,
        .update_progress = true,
        .update_watched = true,
        .watched = state != VSpaceState::Quiescent};
    update.detail[0] = active;
    update.detail[1] = retries;
    update.detail[2] = static_cast<u64>(pending);
    update.detail[3] = claim || invalidations || authorities;
    observation.publish(update);
    if (state == VSpaceState::Quiescent) {
        const auto terminal = diag::concurrency::ObservationKey{
            observation_key_.exchange<libk::MemoryOrder::AcqRel>(0)};
        if (terminal) {
            auto finished =
                diag::concurrency::ObservationLease::borrow(terminal);
            finished.finish(static_cast<u32>(VSpaceServiceState::Settled));
        }
    } else {
        observation.watch(true);
    }
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
