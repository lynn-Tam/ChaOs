#pragma once

#include <core/debug.hpp>
#include <core/types.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/limits.hpp>
#include <libk/utility.hpp>
#include <mm/page_state.hpp>
#include <resource/sponsorship.hpp>
#include <sync/lock.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

class MemoryObject;

/*luna change: keep one complete-or-empty demand with exchange-clearing moves,
  reason: continuation pins and progress snapshots own remaining identity*/
struct FrameDemand final : private libk::noncopyable {
    FrameDemand() noexcept = default;
    FrameDemand(FrameDemand&& other) noexcept
        : reservation_(libk::move(other.reservation_)),
          page_(libk::exchange(other.page_, {})) {
        KASSERT(invariant());
    }
    auto operator=(FrameDemand&& other) noexcept -> FrameDemand& {
        if (this != &other) {
            KASSERT(invariant());
            KASSERT(other.invariant());
            reset();
            reservation_ = libk::move(other.reservation_);
            page_ = libk::exchange(other.page_, {});
            KASSERT(invariant());
        }
        return *this;
    }
    ~FrameDemand() noexcept {
        KASSERT(invariant());
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        KASSERT(invariant());
        return static_cast<bool>(reservation_);
    }

    /*luna change: enforce one empty-or-complete demand boundary, reason:
      pressure retry must never expose a half-owned reservation/key pair*/
    [[nodiscard]] static auto make(
        kernel::resource::Reservation&& reservation,
        PageKey page) noexcept -> FrameDemand {
        KASSERT(static_cast<bool>(reservation) && static_cast<bool>(page));
        return FrameDemand{libk::move(reservation), page};
    }

    void reset() noexcept {
        KASSERT(invariant());
        reservation_.reset();
        page_ = {};
    }

private:
    [[nodiscard]] auto invariant() const noexcept -> bool {
        return static_cast<bool>(reservation_)
            == static_cast<bool>(page_);
    }

    FrameDemand(
        kernel::resource::Reservation&& reservation,
        PageKey page) noexcept
        : reservation_(libk::move(reservation)), page_(page) {
        KASSERT(static_cast<bool>(reservation_)
            && static_cast<bool>(page_));
    }

    kernel::resource::Reservation reservation_{};
    PageKey page_{};
};

/*luna change: retain only the incomplete-type relation link, reason: the
  reclaimer owns the intrusive index while MemoryObject owns policy state*/
struct ReclaimEntry final {
    libk::IntrusiveListHook hook{};
    MemoryObject* object{};
};

// Kernel-private retained pressure work.  It is executor-owned and bounded;
// no ObjectStore object or diagnostic counter can become its source of truth.
class PageReclaimer final {
public:
    static constexpr usize pass_budget = 8;

    PageReclaimer() noexcept = default;

    [[nodiscard]] auto register_memory(MemoryObject& object) noexcept -> bool;
    [[nodiscard]] auto withdraw(MemoryObject& object) noexcept -> bool;

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
    /*luna change: keep one round-robin object relation beside pressure
      relations, reason: candidate admission must remain a derived index*/
    using ObjectList = libk::IntrusiveList<
        ReclaimEntry, &ReclaimEntry::hook>;

    [[nodiscard]] auto next_locked(WaitRelation& relation) noexcept
        -> WaitRelation*;

    mutable kernel::sync::SpinLock<kernel::sync::LockClass::Reclaimer>
        lock_{};
    ObjectList objects_{};
    ReclaimEntry* object_cursor_{};
    RelationList relations_{};
    WaitRelation* cursor_{};
};

} // namespace kernel::mm
