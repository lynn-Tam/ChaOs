#include <thread/thread.hpp>

#include <arch/cpu.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <libk/utility.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <sync/irq_lock_guard.hpp>
#include <operation/completion.hpp>

namespace kernel {

Thread::Thread(
    KernelStack&& home_stack,
    ExecutionBinding&& execution,
    KernelStart start,
    Kind kind) noexcept
    : execution_(libk::move(home_stack), libk::move(execution)),
      authority_(*this),
      start_(start),
      kind_(kind) {
    KASSERT(execution_.binding().kernel_bound());
    KASSERT(start.entry != nullptr);
    execution_.prepare(&Thread::start, this, execution_.stack_top());
    execution_.bind_wait(wait_);
}

Thread::Thread(
    KernelStack&& home_stack,
    ExecutionBinding&& execution,
    UserStart start,
    Kind kind) noexcept
    : Thread(
          kernel::resource::Charge{},
          libk::move(home_stack),
          libk::move(execution),
          start,
          kind) {}

Thread::Thread(
    kernel::resource::Charge&& stack_charge,
    KernelStack&& home_stack,
    ExecutionBinding&& execution,
    UserStart start,
    Kind kind) noexcept
    : execution_(
          libk::move(stack_charge),
          libk::move(home_stack),
          libk::move(execution)),
      authority_(*this),
      start_(start),
      kind_(kind) {
    KASSERT(!idle());
    KASSERT(execution_.binding().user_bound());
    const auto kernel_stack_top = arch::prepare_user_stack(
        execution_.stack_top(), start);
    KASSERT(kernel_stack_top);
    execution_.prepare(&Thread::start, this, *kernel_stack_top);
    execution_.bind_wait(wait_);
}

Thread::~Thread() noexcept {
    KASSERT(execution_.state_ != State::Running);
    KASSERT(execution_.scheduler_binding_ == nullptr);
    KASSERT(!wait_.attached());
    KASSERT(stops_.empty() && execution_.home_ == nullptr);
}

auto Thread::home_stack_base() const noexcept -> usize {
    return execution_.stack_base();
}

auto Thread::home_stack_top() const noexcept -> usize {
    return execution_.stack_top();
}

auto Thread::authorize(
    const cap::Resolved<kernel::mm::VSpace>& vspace,
    const cap::Resolved<cap::CSpace>& cspace) noexcept
    -> libk::Expected<void, cap::GrantError> {
    if (execution_.state_ != State::Prepared
        || execution_.scheduler_binding_ != nullptr
        || !execution_.binding().user_bound()) {
        return libk::unexpected(cap::GrantError::InvalidState);
    }
    return authority_.attach(vspace, cspace);
}

auto Thread::begin_wait(
    operation::Completion& relation,
    CpuRegistry& cpus) noexcept -> bool {
    if (execution_.scheduler_binding_ == nullptr) {
        return false;
    }
    return wait_.begin(relation, cpus, *execution_.scheduler_binding_);
}

auto Thread::wait_ready() const noexcept -> bool {
    return wait_.ready();
}

void Thread::resume_wait(arch::TrapContext& trap) noexcept {
    wait_.finish(trap);
}

void Thread::cancel_wait() noexcept {
    KASSERT(wait_.cancel());
}

auto Thread::prepare_retire() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{stop_lock_};
    return (execution_.state_ == State::Prepared
            || execution_.state_ == State::Exited)
        && execution_.scheduler_binding_ == nullptr && !wait_.attached()
        && execution_.home_ == nullptr && !authority_.active()
        && (execution_.binding().kernel_bound()
            || execution_.binding().detached());
}

void Thread::request_stop(execution::Stop& request) noexcept {
    sched::CpuDispatcher* home{};
    sched::SchedulingContext* context{};
    bool finish{};
    bool initiate{};
    {
        kernel::sync::IrqLockGuard guard{stop_lock_};
        auto** const target = libk::get_if<Thread*>(&request.target_);
        KASSERT(request.started_ && target != nullptr && *target == this);
        stops_.push_back(request);
        if (stopped_) {
            stops_.erase(request);
            finish = true;
        } else if (!stop_requested_) {
            stop_requested_ = true;
            initiate = true;
            home = execution_.home_;
            if (home == nullptr) {
                KASSERT(execution_.state_ == State::Prepared
                    || execution_.state_ == State::Exited);
                execution_.state_ = State::Exited;
                if (execution_.scheduler_binding_ == nullptr) {
                    stopped_ = true;
                    stops_.erase(request);
                    finish = true;
                } else {
                    context = &execution_.scheduler_binding_->context();
                }
            }
        }
    }
    if (finish) {
        request.finish(*this);
    } else if (!initiate) {
        return;
    } else if (context != nullptr) {
        KASSERT(context->unbind());
        finish_stop();
    } else {
        KASSERT(home != nullptr);
        home->request_stop(*this);
    }
}

void Thread::finish_stop() noexcept {
    {
        kernel::sync::IrqLockGuard guard{stop_lock_};
        KASSERT(execution_.state_ == State::Exited
            && execution_.scheduler_binding_ == nullptr);
        execution_.home_ = nullptr;
        stopped_ = true;
    }
    authority_.target_stopped();
    execution_.binding().detach_user();

    for (;;) {
        execution::Stop* request{};
        {
            kernel::sync::IrqLockGuard guard{stop_lock_};
            KASSERT(execution_.state_ == State::Exited
                && execution_.scheduler_binding_ == nullptr);
            if (stops_.empty()) {
                return;
            }
            request = &stops_.pop_front();
        }
        request->finish(*this);
    }
}

[[noreturn]] void Thread::start(void* argument) noexcept {
    auto* const thread = static_cast<Thread*>(argument);
    KASSERT(thread != nullptr);
    KASSERT(thread->execution_.state_ == Thread::State::Running);

    CpuLocal& cpu = current_cpu();
    KASSERT(cpu.dispatcher() != nullptr);
    cpu.dispatcher()->on_context_enter();

    volatile byte stack_marker{};
    KASSERT(thread->execution_.contains(
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
    arch::resume_user(thread->execution_.stack_top());
}

} // namespace kernel
