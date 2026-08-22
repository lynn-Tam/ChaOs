#include <mm/reclaim.hpp>

#include <mm/memory_object.hpp>


namespace kernel::mm {

/*luna change: register only live Pager-backed objects in the derived index,
  reason: reclaim policy must not create a second page-state owner*/
auto PageReclaimer::register_memory(MemoryObject& object) noexcept -> bool {
    Notifier notifier{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (object.reclaim_entry_.hook.is_linked()
            || object.reclaim_entry_.object != nullptr) {
            return false;
        }
        object.reclaim_entry_.object = &object;
        objects_.push_back(object.reclaim_entry_);
        /*luna change: invalidate an unfinished object pass on membership change,
          reason: a derived index mutation invalidates any terminal conclusion*/
        round_start_ = nullptr;
        round_done_ = false;
        if (object_cursor_ == nullptr) {
            object_cursor_ = &object.reclaim_entry_;
        }
        notifier = relations_.empty() ? Notifier{} : notifier_;
    }
    /*luna change: wake retained pressure after object admission, reason:
      membership invalidation must not strand a new bounded round*/
    if (notifier) {
        static_cast<void>(notifier());
    }
    return true;
}

/*luna change: withdraw the derived object hook before backing teardown,
  reason: retire closes admission first and service pins keep the object alive*/
auto PageReclaimer::withdraw(MemoryObject& object) noexcept -> bool {
    Notifier notifier{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        ReclaimEntry& entry = object.reclaim_entry_;
        if (!entry.hook.is_linked()) {
            return false;
        }
        if (object_cursor_ == &entry) {
            auto it = objects_.iterator_to(entry);
            ++it;
            object_cursor_ = it == objects_.end()
                ? (objects_.empty() ? nullptr : &objects_.front())
                : &*it;
        }
        objects_.erase(entry);
        entry.object = nullptr;
        /*luna change: discard the object-round anchor before withdrawal,
          reason: an in-flight foreign reclaim result cannot prove this index*/
        round_start_ = nullptr;
        round_done_ = false;
        if (objects_.empty()) {
            object_cursor_ = nullptr;
        }
        notifier = relations_.empty() ? Notifier{} : notifier_;
    }
    /*luna change: wake retained pressure after object withdrawal, reason:
      retire invalidates round facts while relations still need service*/
    if (notifier) {
        static_cast<void>(notifier());
    }
    return true;
}

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
    Notifier notifier{};
    {
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
        /*luna change: restart object arbitration for a new pressure relation,
          reason: retained work changes the set covered by an exhaustion pass*/
        round_start_ = nullptr;
        round_done_ = false;
        if (cursor_ == nullptr) {
            cursor_ = &relation;
        }
        notifier = notifier_;
    }
    /*luna change: wake the existing reclaimer executor after relation admission,
      reason: the intrusive relation index is canonical while the callback only
      supplies scheduler wake credit*/
    if (notifier) {
        static_cast<void>(notifier());
    }
    return true;
}

void PageReclaimer::bind_notifier(Notifier notifier) noexcept {
    KASSERT(notifier);
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(!notifier_);
    notifier_ = notifier;
}

void PageReclaimer::unbind_notifier() noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    notifier_.reset();
}

auto PageReclaimer::release(
    WaitRelation& relation,
    u64 expected_generation) noexcept -> bool {
    Notifier notifier{};
    {
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
        /*luna change: invalidate terminal arbitration on external release,
          reason: relation membership is part of the bounded OOM conclusion*/
        round_start_ = nullptr;
        round_done_ = false;
        relation.owner = nullptr;
        relation.publish = nullptr;
        relation.request = nullptr;
        relation.observed_progress = 0;
        relation.state_.store<libk::MemoryOrder::Release>(
            static_cast<u8>(PageWaitState::Detached));
        notifier = relations_.empty() ? Notifier{} : notifier_;
    }
    /*luna change: wake surviving pressure after external release, reason:
      clearing a round anchor must not leave retained relations asleep*/
    if (notifier) {
        static_cast<void>(notifier());
    }
    return true;
}

/*luna change: keep one object on More and close only at cursor return, reason:
  a full round must cover every retained backing without a second queue*/
auto PageReclaimer::service(usize capacity) noexcept -> ReclaimResult {
    if (capacity == 0) {
        return ReclaimResult::Idle;
    }
    MemoryObject* object{};
    ReclaimEntry* start{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        /*luna change: admit candidate work only for retained pressure,
          reason: the derived object index must not drive relation-free scans*/
        if (relations_.empty()) {
            return ReclaimResult::Idle;
        }
        if (objects_.empty()) {
            /*luna change: conclude an empty object pass explicitly, reason:
              retained pressure with no registered backing has no candidate
              work and must not leave the round indeterminate*/
            round_start_ = nullptr;
            round_done_ = true;
            return ReclaimResult::Idle;
        }
        if (object_cursor_ == nullptr) {
            object_cursor_ = &objects_.front();
        }
        if (round_done_) {
            return ReclaimResult::Idle;
        }
        if (round_start_ == nullptr) {
            round_start_ = object_cursor_;
        }
        start = round_start_;
        ReclaimEntry& entry = *object_cursor_;
        object = entry.object;
        if (object == nullptr || !object->try_reclaim_pin()) {
            auto next = objects_.iterator_to(entry);
            ++next;
            object_cursor_ = next == objects_.end()
                ? &objects_.front() : &*next;
            round_done_ = object_cursor_ == start;
            /*luna change: skip an unpinnable entry within the same pass,
              reason: retire races must advance the bounded object cursor
              instead of sleeping on one stale relation*/
            return round_done_ ? ReclaimResult::Idle : ReclaimResult::More;
        }
    }
    const ReclaimResult result = object->reclaim(capacity);
    object->finish_reclaim_pin();
    bool done{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (round_start_ != start || start == nullptr) {
            return ReclaimResult::Idle;
        }
        switch (result) {
        case ReclaimResult::More:
            return ReclaimResult::More;
        case ReclaimResult::Wait:
        case ReclaimResult::Progress:
            round_start_ = nullptr;
            round_done_ = false;
            return result;
        case ReclaimResult::Idle:
            break;
        }
        ReclaimEntry& entry = *object_cursor_;
        auto next = objects_.iterator_to(entry);
        ++next;
        object_cursor_ = next == objects_.end()
            ? &objects_.front() : &*next;
        round_done_ = object_cursor_ == start;
        done = round_done_;
        /*luna change: report an unfinished object round as More, reason:
          an idle backing only advances the cursor and cannot conclude OOM*/
    }
    return done ? ReclaimResult::Idle : ReclaimResult::More;
}

auto PageReclaimer::wake(
    u64 frame_progress,
    WaitClaim* ready,
    usize capacity) noexcept -> usize {
    if (ready == nullptr || capacity == 0) {
        return 0;
    }
    usize count{};
    usize inspected{};
    kernel::sync::IrqLockGuard guard{lock_};
    const usize limit = capacity < relations_.size()
        ? capacity
        : relations_.size();
    if (cursor_ == nullptr && !relations_.empty()) {
        cursor_ = &*relations_.begin();
    }
    /*luna change: arbitrate Ready before terminal OOM under one lock,
      reason: PMM generation is the only real progress while internal claims
      do not invalidate the object-round conclusion*/
    while (cursor_ != nullptr && inspected < limit && !relations_.empty()) {
        WaitRelation& relation = *cursor_;
        WaitRelation* const next = next_locked(relation);
        cursor_ = next == &relation ? nullptr : next;
        ++inspected;
        if (relation.state() == PageWaitState::Attached) {
            const PageWaitResult result =
                relation.observed_progress < frame_progress
                    ? PageWaitResult::Ready
                    : round_done_
                        ? PageWaitResult::OutOfMemory
                        : PageWaitResult::Canceled;
            if (result == PageWaitResult::Canceled) {
                continue;
            }
            relation.observed_progress = frame_progress;
            relations_.erase(relation);
            if (relation.claim(
                relation.generation,
                result,
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
