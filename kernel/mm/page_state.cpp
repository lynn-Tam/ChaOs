#include <mm/page_state.hpp>

#include <libk/limits.hpp>

namespace kernel::mm {

namespace {

[[nodiscard]] auto generation_matches(
    const PageSlot& slot,
    u64 generation) noexcept -> bool {
    return generation != 0 && slot.request.key.generation == generation;
}

} // namespace

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
    request = PageRequest{
        .key = key,
        .first = first,
        .count = count,
    };
    generation = key.generation;
    state = PageSlotState::Requesting;
    state = PageSlotState::Requested;
    request.published = true;
    return libk::expected(&request);
}

auto PageSlot::claim_request(u64 claim) noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::Requested || !generation_matches(*this, generation)
        || claim == 0) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    request.claim_generation = claim;
    request.claimed = true;
    state = PageSlotState::Filling;
    return libk::expected();
}

void PageSlot::cancel_request() noexcept {
    if (state == PageSlotState::Requesting
        || state == PageSlotState::Requested) {
        request = {};
        state = PageSlotState::Missing;
    }
}

auto PageSlot::begin_fill(u64 claim) noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::Requested || !request.published
        || request.claimed || claim == 0) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    request.claim_generation = claim;
    request.claimed = true;
    state = PageSlotState::Filling;
    return libk::expected();
}

auto PageSlot::supply(u64 claim, u64 content) noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::Filling || request.claim_generation != claim
        || claim == 0 || content == 0) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    content_epoch = content;
    request = {};
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
    request.failed = true;
    request = {};
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
    state = PageSlotState::Missing;
    return libk::expected();
}

auto PageSlot::mark_dirty(u64 epoch) noexcept
    -> libk::Expected<void, PageStateError> {
    if ((state != PageSlotState::ResidentClean
            && state != PageSlotState::ResidentDirty
            && state != PageSlotState::WritebackQueued
            && state != PageSlotState::WritebackActive)
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
    -> libk::Expected<u64, PageStateError> {
    if (state != PageSlotState::ResidentDirty || dirty_epoch == 0) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    writeback_epoch = dirty_epoch;
    state = PageSlotState::WritebackQueued;
    return libk::expected(writeback_epoch);
}

auto PageSlot::begin_writeback(u64 epoch) noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::WritebackQueued || epoch != writeback_epoch) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    state = PageSlotState::WritebackActive;
    return libk::expected();
}

auto PageSlot::complete_writeback(u64 epoch, bool clean) noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::WritebackActive || epoch != writeback_epoch) {
        return libk::unexpected(PageStateError::StaleGeneration);
    }
    if (clean && dirty_epoch == epoch) {
        state = PageSlotState::ResidentClean;
    } else {
        state = PageSlotState::ResidentDirty;
    }
    return libk::expected();
}

auto PageSlot::begin_evict() noexcept
    -> libk::Expected<void, PageStateError> {
    if (state != PageSlotState::ResidentClean) {
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
    request = {};
    state = PageSlotState::Missing;
    return libk::expected();
}

auto PageSlot::detach() noexcept
    -> libk::Expected<void, PageStateError> {
    if (state == PageSlotState::Released || state == PageSlotState::Detaching) {
        return libk::unexpected(PageStateError::InvalidTransition);
    }
    state = PageSlotState::Detaching;
    request = {};
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
