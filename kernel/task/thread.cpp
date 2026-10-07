#include <expected>
#include <task/thread.hpp>
#include <ipc/endpoint.hpp>

#include <mm/pager.hpp>
#include <mm/mem.hpp>
#include <trace.hpp>
#include <cap/cspace.hpp>
#include <mm/vspace.hpp>
#include <mm/kspace.hpp>

#include <cpu.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>
#include <cpu/local.hpp>
#include <utility>
#include <sched/sc.hpp>
#include <sched/dispatcher.hpp>
#include <sync.hpp>
#include <wait.hpp>

Thread::Thread(
    mm::Stack&& home_stack,
    Env&& roots,
    KernelStart start,
    Kind kind) noexcept
    : stack_(std::move(home_stack)), env_(std::move(roots)),
      roots_{{{*this}, {*this}}},
      revoke_(Stop::Notifier::bind<&Thread::close_grants>(*this)),
      start_(start),
      kind_(kind) {
    libk_assert(env_.kernel_bound());
    libk_assert(start.entry != nullptr);
    prepare(stack_.top());
}

Thread::Thread(
    mm::Stack&& home_stack,
    Env&& roots,
    UserStart start,
    Kind kind) noexcept
    : Thread(
          resource::Charge{},
          std::move(home_stack),
          std::move(roots),
          start,
          kind) {}

Thread::Thread(
    resource::Charge&& stack_charge,
    mm::Stack&& home_stack,
    Env&& roots,
    UserStart start,
    Kind kind) noexcept
    : stack_charge_(std::move(stack_charge)),
      stack_(std::move(home_stack)), env_(std::move(roots)),
      roots_{{{*this}, {*this}}},
      revoke_(Stop::Notifier::bind<&Thread::close_grants>(*this)),
      start_(start),
      kind_(kind) {
    libk_assert(!idle());
    libk_assert(env_.user_bound());
    const auto kernel_stack_top = arch::prepare_user_stack(
        stack_.top(), start);
    libk_assert(kernel_stack_top);
    libk_assert(!stack_charge_ || stack_charge_.amount() == resource::budget{
        .memory = mm::Stack::StackBytes});
    prepare(*kernel_stack_top);
}

Thread::~Thread() noexcept {
    libk_assert(!cleanup_ && grants_ == nullptr && !attaching_ && publishers_ == 0);
    libk_assert(!revoke_.started() || revoke_.complete());
    libk_assert(state_ != State::Running);
    libk_assert(sc_ == nullptr);
    libk_assert(!wait_.attached());
    libk_assert(active_ == nullptr && retired_ == nullptr && !in_kernel_);
    libk_assert(stops_.empty() && home_ == nullptr);
}

auto Thread::home_stack_base() const noexcept -> usize {
    return stack_.base();
}

auto Thread::home_stack_top() const noexcept -> usize {
    return stack_.top();
}

auto Thread::current_stack_base() const noexcept -> usize {
    return active_ != nullptr ? active_->stack().base() : stack_.base();
}

auto Thread::current_stack_top() const noexcept -> usize {
    return active_ != nullptr ? active_->stack().top() : stack_.top();
}

auto Thread::contains_stack(usize address) const noexcept -> bool {
    if (stack_.contains(address)) {
        return true;
    }
    for (ipc::Activation* frame = active_; frame != nullptr;
         frame = frame->previous()) {
        if (frame->stack().contains(address)) {
            return true;
        }
    }
    return false;
}

auto Thread::env() noexcept -> Env& {
    return active_ != nullptr ? active_->env() : env_;
}

auto Thread::env() const noexcept -> const Env& {
    return active_ != nullptr ? active_->env() : env_;
}

auto Thread::ipc_buffer() noexcept -> ipc::Buffer* {
    return active_ != nullptr ? active_->ipc_buffer() : env_.ipc_buffer();
}

auto Thread::ipc_buffer() const noexcept -> const ipc::Buffer* {
    return active_ != nullptr ? active_->ipc_buffer() : env_.ipc_buffer();
}

auto Thread::current_wait() noexcept -> Wait& {
    return active_ != nullptr ? active_->wait() : wait_;
}

auto Thread::current_wait() const noexcept -> const Wait& {
    return active_ != nullptr ? active_->wait() : wait_;
}

auto Thread::call_depth() const noexcept -> usize {
    usize result{};
    for (ipc::Activation* frame = active_; frame != nullptr;
         frame = frame->previous()) {
        ++result;
    }
    return result;
}

auto Thread::cancel_pending() const noexcept -> bool {
    for (ipc::Activation* frame = active_; frame != nullptr;
         frame = frame->previous()) {
        if (frame->cancel_pending()) {
            return true;
        }
    }
    return false;
}

void Thread::push(ipc::Activation& frame) noexcept {
    libk_assert(frame.previous_ == nullptr);
    frame.previous_ = active_;
    active_ = &frame;
}

void Thread::pop(ipc::Activation& frame) noexcept {
    libk_assert(active_ == &frame);
    active_ = frame.previous_;
    frame.previous_ = retired_;
    retired_ = &frame;
}

void Thread::release_calls() noexcept {
    while (retired_ != nullptr) {
        auto* frame = retired_;
        retired_ = frame->previous_;
        frame->previous_ = nullptr;
        frame->release();
    }
}

auto Thread::env_before(
    const ipc::Activation& frame) noexcept -> Env& {
    libk_assert(active_ == &frame);
    return frame.previous_ != nullptr
        ? frame.previous_->env() : env_;
}

auto Thread::ipc_before(
    const ipc::Activation& frame) noexcept -> ipc::Buffer* {
    libk_assert(active_ == &frame);
    // Each Endpoint activation owns its IPC window independently of its roots.
    return frame.previous_ != nullptr ? frame.previous_->ipc_buffer() : env_.ipc_buffer();
}

auto Thread::begin_wait(
    Completion& relation,
    CpuRegistry& cpus) noexcept -> bool {
    if (sc_ == nullptr) {
        return false;
    }
    return current_wait().begin(
        relation, cpus, *sc_);
}

void Thread::block() noexcept {
    libk_assert(!arch::interrupts_enabled() && arch::local()->depth == 0);
    auto& wait = current_wait();
    while (wait.attached()) {
        if (wait.ready()) static_cast<void>(wait.finish());
        else current_cpu().dispatcher()->block_current();
    }
}

void Thread::cancel_wait() noexcept {
    libk_assert(current_wait().cancel());
}

auto Thread::prepare_retire() const noexcept -> bool {
    sync::Lock guard{lock_};
    if (!claims_.empty()) {
        return false;
    }
    return (state_ == State::Prepared
            || state_ == State::Exited)
        && sc_ == nullptr && !current_wait().attached()
        && active_ == nullptr && retired_ == nullptr && !in_kernel_
        && home_ == nullptr
        && (env_.kernel_bound()
            || env_.detached());
}

void Thread::request_stop(Stop& request) noexcept {
    sched::Dispatcher* home{};
    sched::Sc* context{};
    bool finish{};
    bool initiate{};
    {
        sync::Lock guard{lock_};
        libk_assert(request.started() && request.target_ == this);
        stops_.push_back(request);
        if (stopped_) {
            stops_.erase(request);
            finish = true;
        } else if (!stopping_) {
            stopping_ = true;
            initiate = true;
            home = home_;
            if (home == nullptr) {
                libk_assert(state_ == State::Prepared
                    || state_ == State::Exited);
                set_state(State::Exited);
                if (sc_ != nullptr) {
                    context = sc_;
                }
            }
        }
    }
    if (finish) {
        request.finish(*this);
    } else if (!initiate) {
        return;
    } else if (home != nullptr) {
        home->request_stop(*this);
    } else {
        object::ref<> lifetime{};
        if (context != nullptr) {
            auto unbound = context->unbind();
            libk_assert(unbound);
            lifetime = std::move(unbound).value();
        }
        // Prepared user threads already own execution authority and roots.
        // Complete Stop only after the common terminal path releases them.
        finish_stop();
    }
}

void Thread::finish(
    Exit::Reason reason,
    status_t status) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(state_ == State::Exited
            && sc_ == nullptr);
        stopping_ = true;
    }
    claims_.release();
    close_grants();
    env_.detach_user();
    static_cast<void>(exit_.claim(
        reason, status));

    complete();
}

void Thread::finish_stop() noexcept {
    finish(Exit::Reason::Stop, STATUS_CANCELED);
}

void Thread::finish_exit(status_t status) noexcept {
    finish(
        status == STATUS_OK
            ? Exit::Reason::Normal : Exit::Reason::Failed,
        status);
}

[[noreturn]] void Thread::start(void* argument) noexcept {
    auto* const thread = static_cast<Thread*>(argument);
    libk_assert(thread != nullptr);
    libk_assert(thread->state_ == Thread::State::Running);

    CpuLocal& cpu = current_cpu();
    libk_assert(cpu.dispatcher() != nullptr);
    cpu.dispatcher()->on_context_enter();

    volatile byte stack_marker{};
    libk_assert(thread->contains_stack(
        reinterpret_cast<usize>(&stack_marker)));

    if (auto* const kernel_start = std::get_if<KernelStart>(&thread->start_)) {
        const KernelStart start = *kernel_start;
        thread->start_ = KernelStart{};
        libk_assert(start.entry != nullptr);
        start.entry(start.argument);
        libk_assert(!thread->idle());
        sched::exit_current();
    }

    libk_assert(std::holds_alternative<UserStart>(thread->start_));
    thread->start_ = UserStart{};
    arch::resume_user(thread->current_stack_top());
}

auto Thread::draining() const noexcept -> bool {
    return in_kernel_ || activation() || retired_
        || current_wait().attached();
}

auto Thread::stop_ready() const noexcept -> bool {
    sync::Lock guard{lock_};
    return stopping_ && !in_kernel_ && !activation()
        && !current_wait().attached();
}

Stop::~Stop() noexcept {
    libk_assert(!started() || complete());
    libk_assert(target_ == nullptr && !hook_.is_linked());
}

void Stop::start(Thread& target) noexcept {
    libk_assert(!started() && target_ == nullptr);
    target_ = &target;
    phase_.store<libk::MemoryOrder::Release>(Phase::Started);
    target.request_stop(*this);
}

void Stop::finish(Thread& target) noexcept {
    libk_assert(started() && !complete() && target_ == &target && !hook_.is_linked());
    target_ = nullptr;
    const Notifier notify = notify_;
    phase_.store<libk::MemoryOrder::Release>(Phase::Complete);
    if (notify) notify();
}

const cap::GrantAttachmentOps Thread::grant_ops_{
    .invalidate = [](void* context, cap::GrantWork&& work,
                     cap::GrantInvalidation reason) noexcept {
        libk_assert(reason == cap::GrantInvalidation::Revoke);
        auto& relation = *static_cast<Root*>(context);
        relation.owner->invalidate(relation, std::move(work));
    },
    .released = [](void* context) noexcept {
        auto& relation = *static_cast<Root*>(context);
        relation.owner->released(relation);
    },
};

Thread::Flight::Flight(Thread& owner) noexcept : owner_(owner) {
    sync::Lock guard{owner_.grants_lock_};
    ++owner_.publishers_;
}

Thread::Flight::~Flight() noexcept {
    owner_.finish_retire();
}

auto Thread::authorize(const cap::Resolved<mm::VSpace>& space,
                       const cap::Resolved<cap::CSpace>& caps) noexcept
    -> std::expected<void, cap::GrantError> {
    if (state_ != State::Prepared || sc_ != nullptr || !env_.user_bound())
        return std::unexpected(cap::GrantError::InvalidState);
    auto& pair = roots_;
    Flight publishing{*this};
    {
        sync::Lock guard{grants_lock_};
        if (attaching_ || grants_closed_ || revoking_
            || pair[0].phase != Root::Phase::Idle || pair[1].phase != Root::Phase::Idle)
            return std::unexpected(cap::GrantError::InvalidState);
        attaching_ = true;
        for (auto& relation : pair) {
            relation.cap.reset();
            relation.phase = Root::Phase::Attached;
            relation.next = grants_;
            grants_ = &relation;
        }
    }
    auto result = space.attach(pair[0].cap);
    const bool first_attached = static_cast<bool>(result);
    if (result) result = caps.attach(pair[1].cap);
    bool accepted{}, closed{};
    {
        sync::Lock guard{grants_lock_};
        if (!first_attached) unlink(pair[0]);
        if (!result) unlink(pair[1]);
        accepted = result && !grants_closed_ && !revoking_;
        closed = grants_closed_;
        attaching_ = false;
    }
    if (!accepted) {
        detach(pair[0]);
        detach(pair[1]);
    }
    if (closed) close_grants();
    if (!result) return result;
    return accepted ? std::expected<void, cap::GrantError>{}
                    : std::unexpected(cap::GrantError::InvalidState);
}

void Thread::detach(Root& relation) noexcept {
    Flight publishing{*this};
    {
        sync::Lock guard{grants_lock_};
        if (relation.phase != Root::Phase::Attached) return;
        relation.phase = Root::Phase::Detaching;
    }
    if (relation.cap.detach()) released(relation);
}

void Thread::unlink(Root& relation) noexcept {
    auto** link = &grants_;
    while (*link != &relation) {
        libk_assert(*link != nullptr);
        link = &(*link)->next;
    }
    *link = relation.next;
    relation.next = nullptr;
    relation.phase = Root::Phase::Idle;
}

void Thread::released(Root& relation) noexcept {
    Flight publishing{*this};
    {
        sync::Lock guard{grants_lock_};
        libk_assert(relation.phase == Root::Phase::Detaching);
        unlink(relation);
    }
}

void Thread::invalidate(Root& relation, cap::GrantWork&& work) noexcept {
    Flight publishing{*this};
    bool start{}, closed{};
    {
        sync::Lock guard{grants_lock_};
        libk_assert(!relation.work);
        relation.work = std::move(work);
        closed = grants_closed_;
        start = !closed && !revoking_;
        if (start) revoking_ = true;
    }
    if (start) revoke_.start(*this);
    if (closed) close_grants();
}

void Thread::close_grants() noexcept {
    Flight publishing{*this};
    {
        sync::Lock guard{grants_lock_};
        grants_closed_ = true;
        if (attaching_) return;
    }
    for (;;) {
        Root* pending{};
        cap::GrantWork work{};
        {
            sync::Lock guard{grants_lock_};
            for (auto* relation = grants_; relation; relation = relation->next) {
                if (relation->phase == Root::Phase::Attached) { pending = relation; break; }
                if (relation->work) { work = std::move(relation->work); break; }
            }
        }
        if (pending != nullptr) detach(*pending);
        else if (work) work.reset();
        else break;
    }
}

void Thread::retire(object::cleanup&& cleanup) noexcept {
    Flight publishing{*this};
    {
        sync::Lock guard{grants_lock_};
        libk_assert(!cleanup_);
        cleanup_ = std::move(cleanup);
    }
    close_grants();
}

void Thread::finish_retire() noexcept {
    object::cleanup done;
    {
        sync::Lock guard{grants_lock_};
        libk_assert(publishers_ != 0);
        --publishers_;
        if (!cleanup_ || !grants_closed_ || grants_ != nullptr || attaching_ || publishers_ != 0
            || (revoking_ && !revoke_.complete())) return;
        done = std::move(cleanup_);
    }
    done.complete();
}

void Thread::prepare(usize top) noexcept {
    libk_assert(top >= stack_.base() && top <= stack_.top() && (top & 0xfU) == 0);
    ctx_ = arch::Ctx{top, &Thread::start, this};
}

void Thread::set_state(Thread::State state) noexcept {
    state_ = state;
    trace::emit(trace::Event::State, identity(), reinterpret_cast<u64>(sc_), static_cast<u64>(state));
}

auto Thread::stop_requested() const noexcept -> bool {
    sync::Lock guard{lock_};
    return stopping_ || stopped_;
}

auto Thread::stopped() const noexcept -> bool {
    sync::Lock guard{lock_};
    return stopped_;
}

auto Thread::claim_home(sched::Dispatcher& home) noexcept -> bool {
    sync::Lock guard{lock_};
    if ((home_ != nullptr && home_ != &home) || (stopping_ && home_ == nullptr)) return false;
    home_ = &home;
    return true;
}

auto Thread::owned_by(const sched::Dispatcher& home) const noexcept -> bool {
    sync::Lock guard{lock_};
    return stopping_ && home_ == &home;
}

auto Thread::try_bind(sched::Sc& binding) noexcept -> bool {
    sync::Lock guard{lock_};
    if (idle() || state_ != Thread::State::Prepared || sc_ != nullptr
        || home_ != nullptr || stopping_ || stopped_) return false;
    sc_ = &binding;
    return true;
}

auto Thread::release_sc(sched::Sc& binding,
                                sched::Dispatcher* owner) noexcept -> bool {
    sync::Lock guard{lock_};
    if (sc_ != &binding || home_ != owner
        || state_ == Thread::State::Running || state_ == Thread::State::Ready
        || state_ == Thread::State::Throttled || (owner != nullptr && state_ != Thread::State::Exited)) return false;
    sc_ = nullptr;
    return true;
}

void Thread::complete() noexcept {
    Flight publishing{*this};
    {
        sync::Lock guard{lock_};
        libk_assert(state_ == Thread::State::Exited && sc_ == nullptr);
        home_ = nullptr;
        stopped_ = true;
    }
    for (;;) {
        Stop* request{};
        {
            sync::Lock guard{lock_};
            if (stops_.empty()) return;
            request = &stops_.pop_front();
        }
        request->finish(*this);
    }
}

