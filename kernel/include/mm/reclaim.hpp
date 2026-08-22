#pragma once

#include <core/debug.hpp>
#include <core/types.hpp>
#include <diag/concurrency.hpp>
#include <libk/delegate.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/limits.hpp>
#include <libk/optional.hpp>
#include <libk/utility.hpp>
#include <mm/page_state.hpp>
#include <resource/sponsorship.hpp>
#include <sync/lock.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

class MemoryObject;

/*luna change: retain one explicit pressure lifetime, reason: continuation
  identity already supplies the retry target and unsponsored allocations have
  an empty Reservation*/
struct FrameDemand final : private libk::noncopyable {
    FrameDemand() noexcept = default;
    FrameDemand(FrameDemand&& other) noexcept
        : reservation_(libk::move(other.reservation_)) {
        other.reservation_.reset();
    }
    auto operator=(FrameDemand&& other) noexcept -> FrameDemand& {
        if (this != &other) {
            reset();
            reservation_ = libk::move(other.reservation_);
            other.reservation_.reset();
        }
        return *this;
    }
    ~FrameDemand() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return reservation_.has_value();
    }

    /*luna change: make demand engagement the sole retained-work truth,
      reason: an empty Reservation is valid for unsponsored pressure*/
    void emplace(kernel::resource::Reservation&& reservation) noexcept {
        KASSERT(!reservation_.has_value());
        reservation_.emplace(libk::move(reservation));
    }

    [[nodiscard]] auto take() noexcept
        -> libk::optional<kernel::resource::Reservation> {
        if (!reservation_.has_value()) {
            return {};
        }
        libk::optional<kernel::resource::Reservation> result{
            libk::optional_in_place, libk::move(*reservation_)};
        reservation_.reset();
        return result;
    }

    void reset() noexcept {
        reservation_.reset();
    }

private:
    libk::optional<kernel::resource::Reservation> reservation_{};
};

/*luna change: classify one bounded reclaim pass, reason: scheduler activity
  cannot stand in for a candidate or real frame progress fact*/
enum class ReclaimResult : u8 {
    Idle,
    More,
    Wait,
    Progress,
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
    using Notifier = libk::delegate<
        diag::concurrency::ObservationKey() noexcept>;

    PageReclaimer() noexcept = default;

    void bind_notifier(Notifier notifier) noexcept;
    void unbind_notifier() noexcept;

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
    [[nodiscard]] auto service(usize capacity) noexcept -> ReclaimResult;
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
    /*luna change: delimit one object pass with its cursor anchor, reason:
      terminal exhaustion is valid only after every retained object is idle*/
    using ObjectList = libk::IntrusiveList<
        ReclaimEntry, &ReclaimEntry::hook>;

    [[nodiscard]] auto next_locked(WaitRelation& relation) noexcept
        -> WaitRelation*;

    mutable kernel::sync::SpinLock<kernel::sync::LockClass::Reclaimer>
        lock_{};
    ObjectList objects_{};
    ReclaimEntry* object_cursor_{};
    ReclaimEntry* round_start_{};
    bool round_done_{};
    RelationList relations_{};
    WaitRelation* cursor_{};
    Notifier notifier_{};
};

} // namespace kernel::mm
