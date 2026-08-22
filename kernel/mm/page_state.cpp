#include <mm/page_state.hpp>

#include <core/debug.hpp>
#include <libk/limits.hpp>
#include <libk/utility.hpp>

namespace kernel::mm {

namespace {

[[nodiscard]] auto generation_matches(
    const PageSlot& slot,
    u64 generation) noexcept -> bool {
    return generation != 0 && slot.request.key.generation == generation;
}

} // namespace

WaitClaim::WaitClaim(WaitClaim&& other) noexcept
    : relation_(libk::exchange(other.relation_, nullptr)),
      generation_(other.generation_),
      owner_(other.owner_),
      publish_(other.publish_),
      result_(other.result_),
      request_(other.request_),
      finalized_(other.finalized_),
      phase_(other.phase_) {
    other.generation_ = 0;
    other.owner_ = nullptr;
    other.publish_ = nullptr;
    other.request_ = nullptr;
    other.finalized_ = false;
}

auto WaitClaim::operator=(WaitClaim&& other) noexcept -> WaitClaim& {
    if (this != &other) {
        KASSERT(relation_ == nullptr);
        relation_ = libk::exchange(other.relation_, nullptr);
        generation_ = other.generation_;
        owner_ = other.owner_;
        publish_ = other.publish_;
        result_ = other.result_;
        request_ = other.request_;
        finalized_ = other.finalized_;
        phase_ = other.phase_;
        other.generation_ = 0;
        other.owner_ = nullptr;
        other.publish_ = nullptr;
        other.request_ = nullptr;
        other.finalized_ = false;
    }
    return *this;
}

WaitClaim::~WaitClaim() noexcept {
    KASSERT(relation_ == nullptr);
}

auto WaitClaim::publish() noexcept -> bool {
    /*luna change: publish only a host-finalized terminal snapshot, reason: a callback may reuse or destroy continuation storage immediately*/
    if (!finalized_ || phase_ != Phase::Claimed || publish_ == nullptr) {
        return false;
    }
    publish_(owner_, result_);
    phase_ = Phase::Published;
    return true;
}

auto WaitClaim::release() noexcept -> bool {
    if (!finalized_ || phase_ != Phase::Published) {
        return false;
    }
    phase_ = Phase::Released;
    return true;
}

auto WaitClaim::finalize() noexcept -> bool {
    /*luna change: finalize and detach the relation before publication, reason: host ownership must close the reuse window before runnable work is exposed*/
    if (relation_ == nullptr || finalized_ || phase_ != Phase::Claimed
        || relation_->generation != generation_
        || relation_->state() != PageWaitState::Publishing) {
        return false;
    }
    relation_->owner = nullptr;
    relation_->publish = nullptr;
    relation_->request = nullptr;
    relation_->observed_progress = 0;
    relation_->state_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(PageWaitState::Detached));
    relation_ = nullptr;
    finalized_ = true;
    return true;
}

void WaitClaim::reset() noexcept {
    KASSERT(relation_ == nullptr && finalized_ && phase_ == Phase::Released);
    relation_ = nullptr;
    generation_ = 0;
    owner_ = nullptr;
    publish_ = nullptr;
    result_ = PageWaitResult::Canceled;
    request_ = nullptr;
    finalized_ = false;
}

auto WaitRelation::claim(
    u64 expected_generation,
    PageWaitResult terminal_result,
    WaitClaim& claim_token,
    PageRequest* request_owner) noexcept -> bool {
    if (!attached() || generation != expected_generation
        || state() != PageWaitState::Attached || publish == nullptr) {
        return false;
    }
    state_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(PageWaitState::Publishing));
    claim_token = WaitClaim{
        *this, generation, owner, publish, terminal_result, request_owner};
    return true;
}

auto PageRequest::reset() noexcept -> bool {
    // A request may be reset only after every relation has claimed its
    // terminal result and drained publisher leases.
    if (!waiters.empty() || publishers != 0) {
        return false;
    }
    key = {};
    first = 0;
    count = 0;
    claim_generation = 0;
    state = PageRequestState::Idle;
    return true;
}

auto PageRequest::begin_publish() noexcept -> bool {
    /*luna change: make Pager admission an explicit request transition, reason: transport slot zero is a valid identity and cannot encode ownership*/
    if (state != PageRequestState::Queued) {
        return false;
    }
    state = PageRequestState::Publishing;
    return true;
}

/*luna change: make PageRequest publication transport-free, reason: Pager owns transport identity while PageRequest only records semantic phase*/
auto PageRequest::publish() noexcept -> bool {
    if (state != PageRequestState::Publishing) {
        return false;
    }
    state = PageRequestState::Published;
    return true;
}

auto PageRequest::abort_publish() noexcept -> bool {
    if (state != PageRequestState::Publishing) {
        return false;
    }
    state = PageRequestState::Queued;
    return true;
}

/*luna change: permit terminal failure before transport claim, reason: a closed or invalid Pager publish must settle the queued semantic request without inventing a transport identity*/
auto PageRequest::finish(PageWaitResult result) noexcept -> bool {
    if (state != PageRequestState::Queued
        && state != PageRequestState::Publishing
        && state != PageRequestState::Claimed
        && state != PageRequestState::Published) {
        return false;
    }
    state = result == PageWaitResult::Failed
        ? PageRequestState::Failed
        : PageRequestState::Ready;
    return true;
}

auto PageRequest::terminal_result() const noexcept -> PageWaitResult {
    return state == PageRequestState::Failed
        ? PageWaitResult::Failed
        : PageWaitResult::Ready;
}

auto PageRequest::attach(
    WaitRelation& relation,
    void* owner_context,
    WaitRelation::Publish delivery) noexcept -> bool {
    if (!key || relation.attached()
        || relation.generation == libk::numeric_limits<u64>::max()
        || delivery == nullptr) {
        return false;
    }
    relation.owner = owner_context;
    relation.publish = delivery;
    relation.request = this;
    ++relation.generation;
    relation.state_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(PageWaitState::Attached));
    waiters.push_back(relation);
    return true;
}

auto PageRequest::detach(
    WaitRelation& relation,
    u64 expected_generation) noexcept -> bool {
    /*luna change: close cancellation by exact relation generation, reason: the host lock owns O(1) intrusive unlink and prevents a second terminal callback*/
    if (!relation.attached() || relation.request != this
        || relation.generation != expected_generation
        || relation.state() != PageWaitState::Attached
        || !relation.hook_.is_linked()) {
        return false;
    }
    waiters.erase(relation);
    relation.owner = nullptr;
    relation.publish = nullptr;
    relation.request = nullptr;
    relation.observed_progress = 0;
    relation.state_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(PageWaitState::Detached));
    return true;
}

auto PageRequest::claim_waiters(
    WaitClaim* claims,
    usize capacity,
    PageWaitResult result) noexcept -> usize {
    if (claims == nullptr || capacity == 0) {
        return 0;
    }
    usize claimed_count{};
    usize inspected{};
    const usize limit = capacity < waiters.size() ? capacity : waiters.size();
    for (auto it = waiters.begin(); it != waiters.end()
            && inspected < limit;) {
        auto& relation = *it++;
        ++inspected;
        waiters.erase(relation);
        ++publishers;
        if (relation.claim(
                relation.generation,
                result,
                claims[claimed_count],
                this)) {
            ++claimed_count;
        } else {
            --publishers;
            waiters.push_back(relation);
        }
    }
    return claimed_count;
}

auto PageRequest::finish_claim(WaitClaim& claim) noexcept -> bool {
    WaitRelation* const relation = claim.relation();
    if (relation == nullptr || claim.request() != this
        || relation->generation != claim.generation()
        || relation->state() != PageWaitState::Publishing
        || publishers == 0) {
        return false;
    }
    if (!claim.finalize()) {
        return false;
    }
    --publishers;
    return true;
}

auto PageSlot::begin_request(PageKey key, usize first, usize count) noexcept
    -> libk::Expected<PageRequest*, PageStateError> {
    if (!key || count == 0
        || first > libk::numeric_limits<usize>::max() - count
        || state != PageSlotState::Missing) {
        return libk::unexpected(
            count == 0
                    || first > libk::numeric_limits<usize>::max() - count
                ? PageStateError::InvalidRange
                : PageStateError::InvalidTransition);
    }
    if (!request.reset()) {
        return libk::unexpected(PageStateError::Busy);
    }
    request.key = key;
    request.first = first;
    request.count = count;
    request.state = PageRequestState::Queued;
    generation = key.generation;
    state = PageSlotState::Requesting;
    state = PageSlotState::Requested;
    return libk::expected(&request);
}

void PageSlot::cancel_request() noexcept {
    if (state == PageSlotState::Requesting
        || state == PageSlotState::Requested) {
        if (request.reset()) {
            state = PageSlotState::Missing;
        }
    }
}

/*luna change: remove the duplicate page-in claim transition, reason: begin_fill is the sole Requested-to-Filling owner gate*/
auto PageSlot::begin_fill(u64 claim) noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::Requested
        || request.state != PageRequestState::Published || claim == 0) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    request.claim_generation = claim;
    request.state = PageRequestState::Claimed;
    state = PageSlotState::Filling;
    return libk::expected();
}

auto PageSlot::supply(u64 claim, u64 content) noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::Filling
        || request.state != PageRequestState::Claimed
        || request.claim_generation != claim
        || claim == 0 || content == 0) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    content_epoch = content;
    if (!request.finish(PageWaitResult::Ready)) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    state = PageSlotState::ResidentClean;
    return libk::expected();
}

auto PageSlot::fail(u64 claim) noexcept
    -> libk::Expected<void, PageStateError> {
    if ((state != PageSlotState::Requested && state != PageSlotState::Filling)
        || !generation_matches(*this, generation)
        || (state == PageSlotState::Filling
            && request.claim_generation != claim)) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    if (!request.finish(PageWaitResult::Failed)) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    state = PageSlotState::Failed;
    return libk::expected();
}

auto PageSlot::retry() noexcept -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::Failed) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    if (generation == libk::numeric_limits<u64>::max()) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    ++generation;
    request.key.generation = generation;
    /*luna change: clear intent when a new PageKey starts, reason: reclaim
      obligations belong to the retired generation only*/
    reclaim_intent = false;
    state = PageSlotState::Missing;
    return libk::expected();
}

auto PageSlot::mark_dirty(u64 epoch) noexcept
    -> libk::Expected<void, PageStateError> {
    /*luna change: preserve terminal writeback failure while advancing dirty
      epoch, reason: later A/D fold updates canonical usage without inventing
      a retryable ResidentDirty state*/
    if ((state != PageSlotState::ResidentClean
            && state != PageSlotState::ResidentDirty
            && state != PageSlotState::WritebackQueued
            && state != PageSlotState::WritebackPublishing
            && state != PageSlotState::WritebackPublished
            && state != PageSlotState::WritebackActive
            && state != PageSlotState::WritebackCompleting
            && state != PageSlotState::WritebackFailed)
        || epoch == 0) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    if (epoch < dirty_epoch) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    dirty_epoch = epoch;
    if (state == PageSlotState::ResidentClean
        || state == PageSlotState::ResidentDirty) {
        state = PageSlotState::ResidentDirty;
    }
    return libk::expected();
}

auto PageSlot::queue_writeback() noexcept
    -> libk::Expected<WritebackKey, PageStateError> {
    if (state != PageSlotState::ResidentDirty || dirty_epoch == 0) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    if (writeback.generation == libk::numeric_limits<u64>::max()) {
        return libk::unexpected(PageStateError::GenerationExhausted);
    }
    ++writeback.generation;
    writeback.dirty_epoch = dirty_epoch;
    writeback.delivery_generation = 0;
    writeback.claim_generation = 0;
    writeback.retained = true;
    state = PageSlotState::WritebackQueued;
    return libk::expected(WritebackKey{
        .page = request.key,
        .generation = writeback.generation,
        .dirty_epoch = writeback.dirty_epoch,
    });
}

auto PageSlot::begin_writeback_publish(WritebackKey key) noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::WritebackQueued || !writeback.retained
        || key.page != request.key || key.generation != writeback.generation
        || key.dirty_epoch != writeback.dirty_epoch) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    state = PageSlotState::WritebackPublishing;
    return libk::expected();
}

auto PageSlot::publish_writeback(
    WritebackKey key,
    u64 delivery_generation) noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::WritebackPublishing
        || delivery_generation == 0 || key.page != request.key
        || key.generation != writeback.generation
        || key.dirty_epoch != writeback.dirty_epoch) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    writeback.delivery_generation = delivery_generation;
    state = PageSlotState::WritebackPublished;
    return libk::expected();
}

auto PageSlot::abort_writeback_publish(WritebackKey key) noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::WritebackPublishing
        || key.page != request.key || key.generation != writeback.generation
        || key.dirty_epoch != writeback.dirty_epoch) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    state = PageSlotState::WritebackQueued;
    return libk::expected();
}

auto PageSlot::claim_writeback(
    WritebackKey key,
    u64 delivery_generation,
    u64 claim_generation) noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::WritebackPublished
        || writeback.delivery_generation != delivery_generation
        || key.page != request.key || key.generation != writeback.generation
        || key.dirty_epoch != writeback.dirty_epoch
        || claim_generation == 0) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    writeback.claim_generation = claim_generation;
    state = PageSlotState::WritebackActive;
    return libk::expected();
}

auto PageSlot::requeue_writeback(
    WritebackKey key,
    u64 delivery_generation,
    u64 claim_generation) noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::WritebackActive
        || writeback.delivery_generation != delivery_generation
        || writeback.claim_generation != claim_generation
        || key.page != request.key || key.generation != writeback.generation
        || key.dirty_epoch != writeback.dirty_epoch) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    writeback.claim_generation = 0;
    state = PageSlotState::WritebackPublished;
    return libk::expected();
}

auto PageSlot::begin_writeback_complete(
    WritebackKey key,
    u64 delivery_generation,
    u64 claim_generation) noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::WritebackActive
        || writeback.delivery_generation != delivery_generation
        || writeback.claim_generation != claim_generation
        || key.page != request.key || key.generation != writeback.generation
        || key.dirty_epoch != writeback.dirty_epoch) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    state = PageSlotState::WritebackCompleting;
    return libk::expected();
}

auto PageSlot::fail_writeback(
    WritebackKey key,
    u64 delivery_generation,
    u64 claim_generation,
    WritebackFailure failure) noexcept
    -> libk::Expected<void, PageStateError> {
    if (key.page != request.key || key.generation != writeback.generation
        || key.dirty_epoch != writeback.dirty_epoch) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    switch (state) {
    case PageSlotState::WritebackQueued:
    case PageSlotState::WritebackPublishing:
        if (delivery_generation != 0 || claim_generation != 0) {
            return libk::unexpected(PageStateError::StaleGeneration);
        }
        break;
    case PageSlotState::WritebackPublished:
        if (delivery_generation == 0
            || writeback.delivery_generation != delivery_generation
            || claim_generation != 0) {
            return libk::unexpected(PageStateError::StaleGeneration);
        }
        break;
    case PageSlotState::WritebackActive:
    case PageSlotState::WritebackCompleting:
        if (delivery_generation == 0 || claim_generation == 0
            || writeback.delivery_generation != delivery_generation
            || writeback.claim_generation != claim_generation) {
            return libk::unexpected(PageStateError::StaleGeneration);
        }
        break;
    default:
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    writeback.failure = failure;
    state = PageSlotState::WritebackFailed;
    return libk::expected();
}

auto PageSlot::complete_writeback(
    WritebackKey key,
    u64 delivery_generation,
    u64 claim_generation) noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::WritebackCompleting
        || writeback.delivery_generation != delivery_generation
        || writeback.claim_generation != claim_generation
        || key.page != request.key || key.generation != writeback.generation
        || key.dirty_epoch != writeback.dirty_epoch) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    if (dirty_epoch == key.dirty_epoch) {
        state = PageSlotState::ResidentClean;
        writeback.retained = false;
    } else {
        state = PageSlotState::ResidentDirty;
        writeback.retained = dirty_epoch != 0;
    }
    return libk::expected();
}

/*luna change: make reclaim intent a PageSlot transition, reason: queue or
  diagnostic membership cannot represent an obligation that spans passes*/
auto PageSlot::retain_reclaim() noexcept
    -> libk::Expected<void, PageStateError> {
    switch (state) {
    case PageSlotState::ResidentClean:
    case PageSlotState::ResidentDirty:
    case PageSlotState::WritebackQueued:
    case PageSlotState::WritebackPublishing:
    case PageSlotState::WritebackPublished:
    case PageSlotState::WritebackActive:
    case PageSlotState::WritebackCompleting:
    case PageSlotState::WritebackFailed:
        reclaim_intent = true;
        return libk::expected();
    default:
        return libk::unexpected(PageStateError::InvalidTransition);
    }
}

auto PageSlot::begin_evict() noexcept
    -> libk::Expected<void, PageStateError> {
    /*luna change: require an explicit retained intent before clean eviction,
      reason: queue or diagnostic membership cannot authorize page loss*/
    if (state != PageSlotState::ResidentClean) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    if (!reclaim_intent) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    state = PageSlotState::Evicting;
    return libk::expected();
}

auto PageSlot::finish_evict() noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::Evicting) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    if (!request.reset()) {
        return libk::unexpected(PageStateError::Busy);
    }
    writeback.reset();
    /*luna change: retire reclaim intent with the exact missing transition,
      reason: a new generation must not inherit a stale eviction obligation*/
    reclaim_intent = false;
    state = PageSlotState::Missing;
    return libk::expected();
}

auto PageSlot::detach() noexcept
    -> libk::Expected<void, PageStateError> {
    if (state == PageSlotState::Released || state == PageSlotState::Detaching) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    if (!request.reset()) {
        return libk::unexpected(PageStateError::Busy);
    }
    state = PageSlotState::Detaching;
    writeback.reset();
    /*luna change: retire reclaim intent on detach, reason: terminal teardown
      owns the slot transition instead of any queue-derived flag*/
    reclaim_intent = false;
    return libk::expected();
}

auto PageSlot::release() noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::Detaching) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    state = PageSlotState::Released;
    return libk::expected();
}

} // namespace kernel::mm
