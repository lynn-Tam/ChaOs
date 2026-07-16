#pragma once

#include <arch/interrupt.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <libk/noncopyable.hpp>
#include <sched/dispatcher.hpp>

namespace kernel::sched {

// Defers dispatch without masking interrupt delivery. Timer/IPI handlers may
// still account and set pending work. The final guard release commits pending
// dispatch at a safe depth-zero point while preserving the caller's IRQ state.
class PreemptGuard final : private libk::noncopyable_nonmovable {
public:
    PreemptGuard() noexcept {
        const arch::InterruptState interrupts = arch::disable_interrupts();
        dispatcher_ = current_cpu().dispatcher();
        KASSERT(dispatcher_ != nullptr);
        dispatcher_->disable_preemption();
        arch::restore_interrupts(interrupts);
    }

    ~PreemptGuard() noexcept {
        const arch::InterruptState interrupts = arch::disable_interrupts();
        dispatcher_->enable_preemption();
        arch::restore_interrupts(interrupts);
    }

private:
    CpuDispatcher* dispatcher_{};
};

} // namespace kernel::sched
