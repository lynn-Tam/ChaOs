#pragma once

#include <arch/interrupt.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <libk/noncopyable.hpp>
#include <sched/dispatcher.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::sched {

// Defers dispatch without masking interrupt delivery. Timer/IPI handlers may
// still account and set pending work. The final guard release commits pending
// dispatch at a safe depth-zero point while preserving the caller's IRQ state.
class PreemptGuard final : private libk::noncopyable_nonmovable {
public:
    PreemptGuard() noexcept {
        kernel::sync::IrqToken irq{};
        dispatcher_ = current_cpu().dispatcher();
        KASSERT(dispatcher_ != nullptr);
        dispatcher_->disable_preemption();
    }

    ~PreemptGuard() noexcept {
        kernel::sync::IrqToken irq{};
        dispatcher_->enable_preemption();
    }

private:
    CpuDispatcher* dispatcher_{};
};

} // namespace kernel::sched
