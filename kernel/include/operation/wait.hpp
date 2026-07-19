#pragma once

#include <libk/delegate.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>
#include <libk/sync/ticket_spin_lock.hpp>

namespace arch {
class TrapContext;
}

namespace kernel {

class CpuRegistry;

namespace sched {
class Binding;
}

namespace operation {

class Completion;

// One blocking edge owned by a kernel-managed continuation. Thread owns the
// base edge; an execution Frame may override it while an Activation is active.
// The operation still owns Completion and the dispatcher still owns run state.
class Wait final : private libk::noncopyable_nonmovable {
public:
    using Notifier = libk::delegate<void() noexcept>;

    Wait() noexcept = default;
    ~Wait() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool;
    [[nodiscard]] auto ready() const noexcept -> bool;
    [[nodiscard]] auto begin(
        Completion& completion,
        CpuRegistry& cpus,
        sched::Binding& binding) noexcept -> bool;
    // Transfers wake delivery from the schedulable Binding to a retained
    // continuation. A false result means completion won the race and the
    // current frame must remain materialized.
    [[nodiscard]] auto park(Notifier notifier) noexcept -> bool;
    void materialize(sched::Binding& binding) noexcept;
    void finish(arch::TrapContext& trap) noexcept;
    [[nodiscard]] auto cancel() noexcept -> bool;

private:
    friend class Completion;

    void wake() noexcept;

    Completion* completion_{};
    CpuRegistry* cpus_{};
    sched::Binding* binding_{};
    Notifier notifier_{};
    mutable libk::TicketSpinLock lock_{};
    libk::Atomic<bool> ready_{};
};

} // namespace operation
} // namespace kernel
