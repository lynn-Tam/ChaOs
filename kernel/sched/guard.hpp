#pragma once

#include <cpu.hpp>
#include <libk/assert.hpp>
#include <cpu/cpu.hpp>
#include <libk/noncopyable.hpp>
#include <sched/dispatcher.hpp>
#include <sync.hpp>

namespace sched {

// Defers dispatch without masking interrupt delivery. Timer/IPI handlers may
// still account and set pending work. The final guard release commits pending
// dispatch at a safe depth-zero point while preserving the caller's IRQ state.
class PreemptGuard final : private libk::noncopyable_nonmovable {
public:
    PreemptGuard() noexcept {
        sync::Irq irq{};
        dispatcher_ = &current_cpu().dispatcher();
        libk_assert(dispatcher_ != nullptr);
        dispatcher_->disable_preemption();
    }

    ~PreemptGuard() noexcept {
        sync::Irq irq{};
        dispatcher_->enable_preemption();
    }

private:
    Dispatcher* dispatcher_{};
};

} // namespace sched
