#pragma once

#include <core/types.hpp>
#include <diag/concurrency.hpp>
#include <libk/delegate.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <mm/memory_object.hpp>
#include <sync/lock.hpp>

namespace kernel::mm {

/*luna change: add one bounded MemoryObject executor index, reason: retained page publication and waiter drain need a shared production service without a second page state machine*/
// Bounded executor index for MemoryObject retained page work. PageRequest and
// PageSlot remain the semantic state; this list only records actionable work.
class MemoryExecutor final : private libk::noncopyable_nonmovable {
    using Queue = libk::IntrusiveList<MemoryObject, &MemoryObject::work_hook_>;

public:
    using Notifier = libk::delegate<
        diag::concurrency::ObservationKey() noexcept>;

    MemoryExecutor() noexcept = default;
    ~MemoryExecutor() noexcept;

    [[nodiscard]] auto submit(MemoryObject& object) noexcept -> bool;
    [[nodiscard]] auto run(usize budget) noexcept -> MemoryServiceBatch;
    [[nodiscard]] auto pending() const noexcept -> bool;

    void bind_notifier(Notifier notifier) noexcept;
    void unbind_notifier() noexcept;

private:
    friend class MemoryObject;

    [[nodiscard]] auto take() noexcept -> MemoryObject*;
    [[nodiscard]] auto withdraw(MemoryObject& object) noexcept -> bool;

    mutable kernel::sync::SpinLock<kernel::sync::LockClass::MemoryWork>
        lock_{};
    Queue queue_{};
    /*luna change: retain one executor-local current runner identity, reason: submit/complete handoff must preserve a concurrent kick without adding object pending truth*/
    MemoryObject* runner_{};
    /*luna change: remove the write-only executor epoch, reason: MemoryExecutor has no batch, observation, or control consumer for a separate epoch*/
    Notifier notifier_{};
};

} // namespace kernel::mm
