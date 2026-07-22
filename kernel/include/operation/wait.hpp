#pragma once

#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>
#include <sync/lock.hpp>

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
    Wait() noexcept = default;
    ~Wait() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool;
    [[nodiscard]] auto ready() const noexcept -> bool;
    [[nodiscard]] auto begin(
        Completion& completion,
        CpuRegistry& cpus,
        sched::Binding& binding) noexcept -> bool;
    void finish(arch::TrapContext& trap) noexcept;
    [[nodiscard]] auto cancel() noexcept -> bool;

private:
    friend class Completion;

    void wake() noexcept;

    Completion* completion_{};
    CpuRegistry* cpus_{};
    sched::Binding* binding_{};
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::Wait> lock_{};
    libk::Atomic<bool> ready_{};
};

} // namespace operation
} // namespace kernel
