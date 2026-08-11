#pragma once

#include <core/types.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/limits.hpp>
#include <mm/page_state.hpp>
#include <sync/lock.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

// Kernel-private retained pressure work.  It is executor-owned and bounded;
// no ObjectStore object or diagnostic counter can become its source of truth.
class PageReclaimer final {
public:
    static constexpr usize pass_budget = 8;

    PageReclaimer() noexcept = default;

    [[nodiscard]] auto retain(
        WaitRelation& relation,
        u64 observed_progress,
        void* owner,
        WaitRelation::Publish publish) noexcept -> bool;
    [[nodiscard]] auto release(
        WaitRelation& relation,
        u64 expected_generation) noexcept -> bool;
    /*luna change: report only retained relations as pressure work, reason: finalized claims have no publisher lease after host unlink*/
    [[nodiscard]] auto pending() const noexcept -> usize {
        kernel::sync::IrqLockGuard guard{lock_};
        return relations_.size();
    }
    [[nodiscard]] auto wake(
        u64 frame_progress,
        WaitClaim* ready,
        usize capacity) noexcept -> usize;
private:
    using RelationList = libk::IntrusiveList<WaitRelation, &WaitRelation::hook_>;

    [[nodiscard]] auto next_locked(WaitRelation& relation) noexcept
        -> WaitRelation*;

    mutable kernel::sync::SpinLock<kernel::sync::LockClass::Reclaimer>
        lock_{};
    RelationList relations_{};
    WaitRelation* cursor_{};
};

} // namespace kernel::mm
