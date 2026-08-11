#include <mm/reclaim.hpp>

namespace kernel::mm {

auto PageReclaimer::next_locked(WaitRelation& relation) noexcept
    -> WaitRelation* {
    auto it = relations_.iterator_to(relation);
    ++it;
    return it == relations_.end() ? &relations_.front() : &*it;
}

auto PageReclaimer::retain(
    WaitRelation& relation,
    u64 observed_progress,
    void* owner,
    WaitRelation::Publish publish) noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    if (relation.attached() || publish == nullptr
        || relation.generation == libk::numeric_limits<u64>::max()) {
        return false;
    }
    ++relation.generation;
    relation.owner = owner;
    relation.publish = publish;
    relation.observed_progress = observed_progress;
    relation.state_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(PageWaitState::Attached));
    relations_.push_back(relation);
    if (cursor_ == nullptr) {
        cursor_ = &relation;
    }
    return true;
}

auto PageReclaimer::release(
    WaitRelation& relation,
    u64 expected_generation) noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    if (!relation.attached() || relation.generation != expected_generation) {
        return false;
    }
    if (relation.state() != PageWaitState::Attached
        || !relation.hook_.is_linked()) {
        return false;
    }
    if (cursor_ == &relation) {
        WaitRelation* const next = next_locked(relation);
        cursor_ = next == &relation ? nullptr : next;
    }
    relations_.erase(relation);
    relation.owner = nullptr;
    relation.publish = nullptr;
    relation.request = nullptr;
    relation.observed_progress = 0;
    relation.state_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(PageWaitState::Detached));
    return true;
}

auto PageReclaimer::wake(
    u64 frame_progress,
    WaitClaim* ready,
    usize capacity) noexcept -> usize {
    if (ready == nullptr || capacity == 0 || frame_progress == 0) {
        return 0;
    }
    kernel::sync::IrqLockGuard guard{lock_};
    usize count{};
    usize inspected{};
    const usize limit = capacity < relations_.size()
        ? capacity
        : relations_.size();
    if (cursor_ == nullptr && !relations_.empty()) {
        cursor_ = &*relations_.begin();
    }
    /*luna change: finalize reclaimed relations under the reclaimer lock before callbacks, reason: progress publication must not race continuation reuse*/
    while (cursor_ != nullptr && inspected < limit && !relations_.empty()) {
        WaitRelation& relation = *cursor_;
        WaitRelation* const next = next_locked(relation);
        cursor_ = next == &relation ? nullptr : next;
        ++inspected;
        if (relation.state() == PageWaitState::Attached
            && relation.observed_progress < frame_progress) {
            relation.observed_progress = frame_progress;
            relations_.erase(relation);
            if (relation.claim(
                relation.generation,
                PageWaitResult::Ready,
                ready[count])) {
                if (ready[count].finalize()) {
                    ++count;
                } else {
                    relation.owner = nullptr;
                    relation.publish = nullptr;
                    relation.request = nullptr;
                    relation.observed_progress = 0;
                    relation.state_.store<libk::MemoryOrder::Release>(
                        static_cast<u8>(PageWaitState::Detached));
                }
            } else {
                relations_.insert(relations_.begin(), relation);
            }
        }
    }
    return count;
}

} // namespace kernel::mm
