#include <thread/thread.hpp>

#include <fault/observation.hpp>
#include <pager/pager.hpp>

#include <arch/cpu.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <libk/utility.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <sync/irq_lock_guard.hpp>
#include <operation/completion.hpp>

namespace kernel {

auto Thread::observe_terminal(
    ipc::Notification& notification,
    u64 badge) noexcept -> bool {
    if (terminal_observation_ != nullptr) {
        return false;
    }
    auto* const observation = fault::allocate_observation();
    if (observation == nullptr
        || !observation->bind(terminal_, notification, badge)) {
        if (observation != nullptr) {
            fault::release_observation(*observation);
        }
        return false;
    }
    terminal_observation_ = observation;
    return true;
}

void Thread::clear_terminal_observation() noexcept {
    auto* const observation = libk::exchange(terminal_observation_, nullptr);
    if (observation != nullptr) {
        observation->reset();
        fault::release_observation(*observation);
    }
}

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
}

Thread::~Thread() noexcept {
    clear_terminal_observation();
    KASSERT(execution_.state_ != State::Running);
    KASSERT(execution_.scheduler_binding_ == nullptr);
    KASSERT(!wait_.attached());
    KASSERT(active_ == nullptr);
    KASSERT(stops_.empty() && execution_.home_ == nullptr);
}

auto Thread::home_stack_base() const noexcept -> usize {
    return execution_.stack_base();
}

auto Thread::home_stack_top() const noexcept -> usize {
    return execution_.stack_top();
}

auto Thread::current_stack_base() const noexcept -> usize {
    return active_ != nullptr ? active_->stack().base() : execution_.stack_base();
}

auto Thread::current_stack_top() const noexcept -> usize {
    return active_ != nullptr ? active_->stack().top() : execution_.stack_top();
}

auto Thread::contains_stack(usize address) const noexcept -> bool {
    if (execution_.contains(address)) {
        return true;
    }
    for (execution::Frame* frame = active_; frame != nullptr;
         frame = frame->previous()) {
        if (frame->stack().contains(address)) {
            return true;
        }
    }
    return false;
}

auto Thread::effective_binding() noexcept -> ExecutionBinding& {
    return active_ != nullptr ? active_->binding() : execution_.binding();
}

auto Thread::effective_binding() const noexcept -> const ExecutionBinding& {
    return active_ != nullptr ? active_->binding() : execution_.binding();
}

auto Thread::ipc_buffer() noexcept -> ipc::Buffer* {
    return active_ != nullptr ? active_->ipc_buffer() : execution_.ipc_buffer();
}

auto Thread::ipc_buffer() const noexcept -> const ipc::Buffer* {
    return active_ != nullptr ? active_->ipc_buffer() : execution_.ipc_buffer();
}

auto Thread::current_wait() noexcept -> operation::Wait& {
    return active_ != nullptr ? active_->wait() : wait_;
}

auto Thread::current_wait() const noexcept -> const operation::Wait& {
    return active_ != nullptr ? active_->wait() : wait_;
}

auto Thread::frame_depth() const noexcept -> usize {
    usize result{};
    for (execution::Frame* frame = active_; frame != nullptr;
         frame = frame->previous()) {
        ++result;
    }
    return result;
}

auto Thread::cancel_pending() const noexcept -> bool {
    for (execution::Frame* frame = active_; frame != nullptr;
         frame = frame->previous()) {
        if (frame->cancel_pending()) {
            return true;
        }
    }
    return false;
}

void Thread::push(execution::Frame& frame) noexcept {
    KASSERT(frame.previous_ == nullptr);
    frame.previous_ = active_;
    active_ = &frame;
}

void Thread::pop(execution::Frame& frame) noexcept {
    KASSERT(active_ == &frame);
    active_ = frame.previous_;
    frame.previous_ = nullptr;
}

auto Thread::binding_before(
    const execution::Frame& frame) noexcept -> ExecutionBinding& {
    KASSERT(active_ == &frame);
    return frame.previous_ != nullptr
        ? frame.previous_->binding() : execution_.binding();
}

auto Thread::ipc_before(
    const execution::Frame& frame) noexcept -> ipc::Buffer* {
    KASSERT(active_ == &frame);
    return frame.previous_ != nullptr
        ? frame.previous_->ipc_buffer() : execution_.ipc_buffer();
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
    return current_wait().begin(
        relation, cpus, *execution_.scheduler_binding_);
}

auto Thread::wait_ready() const noexcept -> bool {
    return current_wait().ready();
}

auto Thread::resume_wait(arch::TrapContext& trap) noexcept -> bool {
    return current_wait().finish(trap);
}

void Thread::cancel_wait() noexcept {
    KASSERT(current_wait().cancel());
}

void Thread::release_pager_claims() noexcept {
    for (usize ticket = 0; ticket < pager::claims_per_execution; ++ticket) {
        const pager::ClaimIndex::Entry entry = pager_claims_.entries[ticket];
        if (entry.pager == nullptr) {
            continue;
        }
        /*luna change: invalidate through the exact requeue owner edge,
          reason: terminal claim release must never become a page terminal
          winner and the entry clears regardless of the outcome*/
        static_cast<void>(entry.pager->invalidate_claim(entry));
        pager_claims_.clear(ticket);
    }
}

auto Thread::prepare_retire() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{stop_lock_};
    if (!pager_claims_.empty()) {
        return false;
    }
    return (execution_.state_ == State::Prepared
            || execution_.state_ == State::Exited)
        && execution_.scheduler_binding_ == nullptr && !current_wait().attached()
        && active_ == nullptr
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
        KASSERT(request.started_ && request.target_ == &execution_);
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
                execution_.set_state(State::Exited);
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

void Thread::finish_terminal(
    fault::Reason reason,
    myos_status_t status) noexcept {
    {
        kernel::sync::IrqLockGuard guard{stop_lock_};
        KASSERT(execution_.state_ == State::Exited
            && execution_.scheduler_binding_ == nullptr);
        execution_.home_ = nullptr;
        stopped_ = true;
    }
    /*luna change: settle service claims at the stop terminal, reason: a
      stopped worker must not hold a Pager claim hostage against graceful
      close*/
    release_pager_claims();
    authority_.target_stopped();
    execution_.binding().detach_user();
    static_cast<void>(terminal_.claim(
        reason, status));

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

void Thread::finish_stop() noexcept {
    finish_terminal(fault::Reason::Stop, MYOS_STATUS_CANCELED);
}

void Thread::finish_exit(myos_status_t status) noexcept {
    finish_terminal(
        status == MYOS_STATUS_OK
            ? fault::Reason::NormalExit : fault::Reason::ExitFailure,
        status);
}

[[noreturn]] void Thread::start(void* argument) noexcept {
    auto* const thread = static_cast<Thread*>(argument);
    KASSERT(thread != nullptr);
    KASSERT(thread->execution_.state_ == Thread::State::Running);

    CpuLocal& cpu = current_cpu();
    KASSERT(cpu.dispatcher() != nullptr);
    cpu.dispatcher()->on_context_enter();

    volatile byte stack_marker{};
    KASSERT(thread->contains_stack(
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
    arch::resume_user(thread->current_stack_top());
}

} // namespace kernel
