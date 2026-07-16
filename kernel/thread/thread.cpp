#include <thread/thread.hpp>

#include <arch/cpu.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <libk/utility.hpp>
#include <sched/dispatcher.hpp>
#include <thread/wait.hpp>

namespace kernel {

Thread::Thread(
    KernelStack&& home_stack,
    ExecutionBinding&& execution,
    KernelStart start,
    Kind kind) noexcept
    : home_stack_(libk::move(home_stack)),
      execution_(libk::move(execution)),
      start_(start),
      kind_(kind) {
    KASSERT(home_stack_.base() != 0);
    KASSERT(home_stack_.size() != 0);
    KASSERT((home_stack_.top() & 0xfU) == 0);
    KASSERT(execution_.kernel_bound());
    KASSERT(start.entry != nullptr);
    arch::prepare_context(context_, {
        .stack_top = home_stack_.top(),
        .entry = &Thread::start,
        .argument = this,
    });
}

Thread::Thread(
    KernelStack&& home_stack,
    ExecutionBinding&& execution,
    UserStart start,
    Kind kind) noexcept
    : home_stack_(libk::move(home_stack)),
      execution_(libk::move(execution)),
      start_(start),
      kind_(kind) {
    KASSERT(home_stack_.base() != 0);
    KASSERT(home_stack_.size() != 0);
    KASSERT((home_stack_.top() & 0xfU) == 0);
    KASSERT(!idle());
    KASSERT(execution_.user_bound());
    const auto kernel_stack_top = arch::prepare_user_stack(
        home_stack_.top(), start);
    KASSERT(kernel_stack_top);
    arch::prepare_context(context_, {
        .stack_top = *kernel_stack_top,
        .entry = &Thread::start,
        .argument = this,
    });
}

Thread::~Thread() noexcept {
    KASSERT(state_ != State::Running);
    KASSERT(binding_ == nullptr);
    KASSERT(wait_ == nullptr);
}

auto Thread::home_stack_base() const noexcept -> usize {
    return home_stack_.base();
}

auto Thread::home_stack_top() const noexcept -> usize {
    return home_stack_.top();
}

auto Thread::rebind(ExecutionBinding&& binding) noexcept
    -> libk::Expected<void, RebindError> {
    if (state_ != State::Prepared || binding_ != nullptr) {
        return libk::unexpected(RebindError::Active);
    }
    const bool kernel_start = libk::holds_alternative<KernelStart>(start_);
    if (kernel_start != binding.kernel_bound() || (idle() && !kernel_start)) {
        return libk::unexpected(RebindError::Incompatible);
    }
    execution_ = libk::move(binding);
    return libk::expected();
}

auto Thread::begin_wait(
    WaitRelation& relation,
    CpuRegistry& cpus) noexcept -> bool {
    if (wait_ != nullptr || relation.attached() || binding_ == nullptr) {
        return false;
    }
    relation.attach(*this, cpus);
    wait_ = &relation;
    return true;
}

auto Thread::wait_ready() const noexcept -> bool {
    return wait_ != nullptr && wait_->complete();
}

void Thread::resume_wait(arch::TrapContext& trap) noexcept {
    WaitRelation* const relation = libk::exchange(wait_, nullptr);
    KASSERT(relation != nullptr && relation->complete());
    relation->resume(trap);
}

void Thread::cancel_wait() noexcept {
    WaitRelation* const relation = libk::exchange(wait_, nullptr);
    KASSERT(relation != nullptr && !relation->complete());
    relation->cancel();
}

[[noreturn]] void Thread::start(void* argument) noexcept {
    auto* const thread = static_cast<Thread*>(argument);
    KASSERT(thread != nullptr);
    KASSERT(thread->state_ == Thread::State::Running);

    CpuLocal& cpu = current_cpu();
    KASSERT(cpu.dispatcher() != nullptr);
    cpu.dispatcher()->on_context_enter();

    volatile byte stack_marker{};
    KASSERT(thread->home_stack_.contains(
        reinterpret_cast<usize>(&stack_marker)));

    if (auto* const kernel_start = libk::get_if<KernelStart>(&thread->start_)) {
        const KernelStart start = *kernel_start;
        thread->start_ = KernelStart{};
        KASSERT(start.entry != nullptr);
        start.entry(start.argument);
        KASSERT(!thread->idle());
        sched::exit_current();
    }

    KASSERT(libk::holds_alternative<UserStart>(thread->start_));
    thread->start_ = UserStart{};
    arch::resume_user(thread->home_stack_.top());
}

} // namespace kernel
