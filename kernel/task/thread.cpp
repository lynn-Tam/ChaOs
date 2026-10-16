#include <expected>
#include <task/thread.hpp>

#include <mm/pager.hpp>
#include <mm/mem.hpp>
#include <trace.hpp>
#include <cap/cspace.hpp>
#include <mm/vspace.hpp>
#include <mm/kspace.hpp>

#include <cpu.hpp>
#include <libk/assert.hpp>
#include <libk/types.hpp>
#include <cpu/cpu.hpp>
#include <utility>
#include <sched/sc.hpp>
#include <sched/dispatcher.hpp>
#include <sched/sc.hpp>
#include <sync.hpp>
#include <wait.hpp>
#include <ipc/notification.hpp>

Thread::Thread(mm::Stack&& home_stack, Env&& roots, KernelStart start, Kind kind) noexcept
    : stack_(std::move(home_stack)), env_(std::move(roots)), roots_{{{*this}, {*this}}},
      revoke_(Stop::Notifier::bind<&Thread::close_grants>(*this)), start_(start), kind_(kind) {
    libk_assert(env_.kernel_bound());
    libk_assert(start.entry != nullptr);
    prepare(stack_.top());
}

Thread::Thread(mm::Stack&& home_stack, Env&& roots, UserStart start, Kind kind) noexcept
    : Thread(resource::Charge{}, std::move(home_stack), std::move(roots), start, kind) {}

Thread::Thread(resource::Charge&& stack_charge, mm::Stack&& home_stack, Env&& roots,
               UserStart start, Kind kind) noexcept
    : stack_charge_(std::move(stack_charge)), stack_(std::move(home_stack)), env_(std::move(roots)),
      roots_{{{*this}, {*this}}}, revoke_(Stop::Notifier::bind<&Thread::close_grants>(*this)),
      start_(start), kind_(kind) {
    libk_assert(!idle());
    libk_assert(env_.user_bound());
    const auto kernel_stack_top = arch::prepare_user_stack(stack_.top(), start);
    libk_assert(kernel_stack_top);
    libk_assert(!stack_charge_ ||
                stack_charge_.amount() == resource::budget{.memory = mm::Stack::StackBytes});
    prepare(*kernel_stack_top);
}

Thread::~Thread() noexcept {
    if (work_) work_->close(finalizer_);
    changed_.reset();
    libk_assert(!cleanup_ && grants_ == nullptr && !attaching_ && publishers_ == 0);
    libk_assert(!revoke_.started() || revoke_.complete());
    libk_assert(state_ != State::Running);
    libk_assert(sc_ == nullptr);
    libk_assert(!wait_.attached());
    libk_assert(!in_kernel_);
    libk_assert(stops_.empty() && home_ == nullptr);
}

auto Thread::stack_base() const noexcept -> usize {
    return stack_.base();
}

auto Thread::stack_top() const noexcept -> usize {
    return stack_.top();
}

auto Thread::begin_wait(Completion& relation, Cpus& cpus) noexcept -> bool {
    return wait().begin(relation, cpus, *this);
}

void Thread::block() noexcept {
    libk_assert(!arch::interrupts_enabled() && arch::local()->depth == 0);
    auto& edge = wait_;
    while (edge.attached()) {
        if (edge.ready())
            static_cast<void>(edge.finish());
        else
            current_cpu().dispatcher().block_current();
    }
}

void Thread::cancel_wait() noexcept {
    libk_assert(wait().cancel());
}

void Thread::request_stop(Stop& request) noexcept {
    sched::Dispatcher* home{};

    bool done{};
    bool initiate{};
    {
        sync::Lock guard{lock_};
        libk_assert(request.started() && request.target_ == this);
        stops_.push_back(request);
        if (stopped_) {
            stops_.erase(request);
            done = true;
        } else if (!stopping_) {
            stopping_ = true;
            initiate = true;
            home = home_;
            if (home == nullptr) {
                libk_assert(state_ == State::Prepared || state_ == State::Exited);
                state_ = State::Exited;
            }
        }
    }
    if (done) {
        request.finish(*this);
    } else if (!initiate) {
        return;
    } else if (home != nullptr) {
        home->request_stop(*this);
    } else {
        // Prepared user threads already own execution authority and roots.
        // Complete Stop only after the common terminal path releases them.
        finish(STATUS_CANCELED);
    }
}

void Thread::finish(status_t status) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(state_ == State::Exited && sc_ == nullptr);
        stopping_ = true;
        terminal_status_ = status;
    }
    if (work_) work_->post(finalizer_);
    else finalize(); // Exclusive construction rollback without a runtime worker.
}
void Thread::finalize() noexcept {
    Flight publishing{*this};
    if (work_) work_->close(finalizer_);
    claims_.release();
    close_grants();
    env_.detach_user();
    static_cast<void>(exit_.claim(terminal_status_));
    complete();
    static_cast<void>(changed_.signal());
}

[[noreturn]] void Thread::start(void* argument) noexcept {
    auto* const thread = static_cast<Thread*>(argument);
    libk_assert(thread != nullptr);
    libk_assert(thread->state_ == Thread::State::Running);

    Cpu& cpu = current_cpu();
    cpu.dispatcher().on_context_enter();

    volatile byte stack_marker{};
    libk_assert(thread->contains_stack(reinterpret_cast<usize>(&stack_marker)));

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
    arch::resume_user(thread->stack_top());
}

auto Thread::draining() const noexcept -> bool {
    return in_kernel_ || wait_.attached();
}

auto Thread::stop_ready() const noexcept -> bool {
    sync::Lock guard{lock_};
    return stopping_ && !in_kernel_ && !wait_.attached();
}

Stop::~Stop() noexcept {
    libk_assert(!started() || complete());
    libk_assert(target_ == nullptr && !hook_.is_linked());
}

void Stop::start(Thread& target) noexcept {
    libk_assert(!started() && target_ == nullptr);
    target_ = &target;
    phase_.store(Phase::Started, std::memory_order_release);
    target.request_stop(*this);
}

void Stop::finish(Thread& target) noexcept {
    libk_assert(started() && !complete() && target_ == &target && !hook_.is_linked());
    target_ = nullptr;
    const Notifier notify = notify_;
    phase_.store(Phase::Complete, std::memory_order_release);
    if (notify)
        notify();
}

const cap::GrantAttachmentOps Thread::grant_ops_{
    .invalidate =
        [](void* context, cap::GrantWork&& work) noexcept {
            auto& relation = *static_cast<Root*>(context);
            relation.owner->invalidate(relation, std::move(work));
        },
    .released =
        [](void* context) noexcept {
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
        if (attaching_ || grants_closed_ || revoking_ || pair[0].phase != Root::Phase::Idle ||
            pair[1].phase != Root::Phase::Idle)
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
    if (result)
        result = caps.attach(pair[1].cap);
    bool accepted{}, closed{};
    {
        sync::Lock guard{grants_lock_};
        if (!first_attached)
            unlink(pair[0]);
        if (!result)
            unlink(pair[1]);
        accepted = result && !grants_closed_ && !revoking_;
        closed = grants_closed_;
        attaching_ = false;
    }
    if (!accepted) {
        detach(pair[0]);
        detach(pair[1]);
    }
    if (closed)
        close_grants();
    if (!result)
        return result;
    return accepted ? std::expected<void, cap::GrantError>{}
                    : std::unexpected(cap::GrantError::InvalidState);
}

void Thread::detach(Root& relation) noexcept {
    Flight publishing{*this};
    {
        sync::Lock guard{grants_lock_};
        if (relation.phase != Root::Phase::Attached)
            return;
        relation.phase = Root::Phase::Detaching;
    }
    if (relation.cap.detach())
        released(relation);
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
        if (start)
            revoking_ = true;
    }
    if (start)
        revoke_.start(*this);
    if (closed)
        close_grants();
}

void Thread::close_grants() noexcept {
    Flight publishing{*this};
    {
        sync::Lock guard{grants_lock_};
        grants_closed_ = true;
        if (attaching_)
            return;
    }
    for (;;) {
        Root* pending{};
        cap::GrantWork work{};
        {
            sync::Lock guard{grants_lock_};
            for (auto* relation = grants_; relation; relation = relation->next) {
                if (relation->phase == Root::Phase::Attached) {
                    pending = relation;
                    break;
                }
                if (relation->work) {
                    work = std::move(relation->work);
                    break;
                }
            }
        }
        if (pending != nullptr)
            detach(*pending);
        else if (work)
            work.reset();
        else
            break;
    }
}

void Thread::stop() noexcept {
    Flight publishing{*this};
    bool start;
    {
        sync::Lock guard{grants_lock_};
        start = !revoking_;
        revoking_ = true;
    }
    if (start) revoke_.start(*this);
    static_cast<void>(changed_.signal());
}

void Thread::retire(object::cleanup&& cleanup) noexcept {
    Flight publishing{*this};
    bool start{};
    {
        sync::Lock guard{grants_lock_};
        libk_assert(!cleanup_);
        cleanup_ = std::move(cleanup);
        start = !revoking_;
        revoking_ = true;
    }
    // The same stop owns grant revocation and retirement. It drains execution,
    // claims and environment roots before close_grants can complete cleanup.
    if (start)
        revoke_.start(*this);
}

void Thread::finish_retire() noexcept {
    object::cleanup done;
    {
        sync::Lock guard{grants_lock_};
        libk_assert(publishers_ != 0);
        --publishers_;
        if (!cleanup_ || !grants_closed_ || grants_ != nullptr || attaching_ || publishers_ != 0 ||
            (revoking_ && !revoke_.complete()))
            return;
        done = std::move(cleanup_);
    }
    done.complete();
}

void Thread::prepare(usize top) noexcept {
    libk_assert(top >= stack_.base() && top <= stack_.top() && (top & 0xfU) == 0);
    ctx_ = arch::Ctx{top, &Thread::start, this};
}

void Thread::set_state(Thread::State state) noexcept {
    sync::Lock guard{lock_};
    state_ = state;
    if (state == State::Running && Cpu::local()) accounted_at_ = current_cpu().dispatcher().clock().rd_ticks();
    trace::emit(trace::Event::State, identity(), reinterpret_cast<u64>(sc_),
                static_cast<u64>(state));
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
    if ((home_ != nullptr && home_ != &home) || (stopping_ && home_ == nullptr))
        return false;
    home_ = &home;
    cpu_ = home.id();
    return true;
}

auto Thread::owned_by(const sched::Dispatcher& home) const noexcept -> bool {
    sync::Lock guard{lock_};
    return home_ == &home;
}

auto Thread::home_cpu() const noexcept -> CpuId {
    sync::Lock guard{lock_};
    libk_assert(home_);
    return home_->id();
}

void Thread::refill() noexcept {
    current_cpu().dispatcher().refresh(*this);
}

auto Thread::execution(bool enabled) noexcept -> std::optional<u64> {
    sched::Dispatcher* home;
    u64 seq;
    {
        sync::Lock guard{lock_};
        if (stopping_ || stopped_ || !home_) return std::nullopt;
        enabled_ = enabled;
        seq = ++requested_;
        home = home_;
    }
    home->request_control(*this);
    return seq;
}

auto Thread::detach_resource(u64 generation) noexcept -> status_t {
    sched::Dispatcher* home;
    {
        sync::Lock guard{lock_};
        if (!association_ || generation != association_) return STATUS_STALE;
        if (stopping_ || stopped_) return STATUS_BUSY;
        detach_sc_ = true;
        home = home_;
    }
    home->request_control(*this);
    return STATUS_OK;
}
auto Thread::info(u64 now) const noexcept -> ThreadInfo {
    sync::Lock guard{lock_};
    const auto runtime = runtime_ + (state_ == State::Running && now > accounted_at_ ? now - accounted_at_ : 0);
    return {.version = 1, .size = sizeof(ThreadInfo), .requested = requested_, .applied = applied_,
        .runtime = runtime, .association = association_,
        .flags = (enabled_ ? THREAD_ENABLED : 0U) | (applied_enabled_ ? THREAD_APPLIED : 0U)
            | (state_ == State::Running ? THREAD_RUNNING : 0U) | (wait_.attached() ? THREAD_WAITING : 0U)
            | (sc_ ? THREAD_BOUND : 0U) | (stopping_ ? THREAD_STOPPING : 0U) | (stopped_ ? THREAD_STOPPED : 0U),
        .cpu = static_cast<u32>(cpu_.raw)};
}
auto Thread::observe(ipc::Notification& notice, u64 badge) noexcept -> bool {
    {
        sync::Lock guard{lock_};
        if (!notice.bind(changed_, badge)) return false;
    }
    static_cast<void>(changed_.signal());
    return true;
}

auto Thread::try_bind(object::ref<sched::Sc> binding, u64 generation) noexcept -> bool {
    sync::Lock guard{lock_};
    if (idle() || sc_ || pending_sc_ || stopping_ || stopped_ || !home_
        || !binding->domain_->allows(home_->id())
        || !(binding->config_.cpus & (u64{1} << home_->id().raw))) return false;
    pending_sc_ = std::move(binding);
    association_ = generation;
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
            if (stops_.empty())
                return;
            request = &stops_.pop_front();
        }
        request->finish(*this);
    }
}
