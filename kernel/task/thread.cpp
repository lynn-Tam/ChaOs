#include <expected>
#include <task/thread.hpp>

#include <trace.hpp>
#include <cap/cspace.hpp>
#include <mm/vspace.hpp>
#include <mm/kspace.hpp>

#include <cpu.hpp>
#include <libk/assert.hpp>
#include <libk/types.hpp>
#include <cpu/cpu.hpp>
#include <utility>
#include <sched/sched.hpp>
#include <sched/dispatcher.hpp>
#include <sync.hpp>
#include <wait.hpp>

Thread::Thread(mm::Stack&& home_stack, Env&& roots, KernelStart start, Kind kind) noexcept
    : run_(std::in_place_type<Kernel>, std::move(home_stack), arch::Ctx{}, start),
      env_(std::move(roots)),
      kind_(kind) {
    libk_assert(env_.kernel_bound() && start.entry);
    ctx() = arch::Ctx{stack_top(), &Thread::start, this};
}

Thread::Thread(Env&& roots, UserStart start) noexcept
    : run_(std::in_place_type<arch::TrapFrame>), env_(std::move(roots)) {
    libk_assert(env_.user_bound() && arch::prepare_user_frame(frame(), start));
}

Thread::Thread(Env&& roots, KernelStart step) noexcept
    : run_(std::in_place_type<Kernel>, std::nullopt, arch::Ctx{}, step),
      env_(std::move(roots)), kind_(Kind::Worker) {
    libk_assert(env_.kernel_bound() && step.entry);
}

Thread::~Thread() noexcept {
    if (work_) work_->close(finalizer_);
    libk_assert(!cleanup_ && !mail_hold_);
    libk_assert(state_ != State::Running);
    libk_assert(sc_ == nullptr);
    libk_assert(!wait_.attached());
    libk_assert(!in_kernel_);
    libk_assert(home_ == nullptr);
}

auto Thread::stack_base() const noexcept -> usize {
    return transient() ? 0 : std::get<Kernel>(run_).stack->base();
}

auto Thread::stack_top() const noexcept -> usize {
    return transient() ? 0 : std::get<Kernel>(run_).stack->top();
}

auto Thread::Interest::admit(const cap::GrantPin& grant, std::span<const object::ref<>*> gates,
    Source::Commit commit, Source::Action revoke, Source::Action ready) noexcept -> status_t {
    std::optional<decltype(bindings_.hold())> publishing;
    sync::Lock guard{lock_};
    if (active_ || !bindings_.reset()) return STATUS_BUSY;
    // Another CPU may finish immediately after registration. Its last drain
    // callback must run after the enclosing Interest publication unlocks.
    publishing.emplace(bindings_.hold());
    revoke_ = revoke; ready_ = ready;
    const cap::GrantPin* grants[]{&grant};
    auto install = [&]() noexcept { active_ = true; commit(Source::Action::bind<&Interest::end>(*this)); };
    const auto status = bindings_.attach(grants, gates, Source::Action::bind(install));
    if (status != STATUS_OK) revoke_ = ready_ = {};
    return status;
}
void Thread::Interest::invalidate() noexcept {
    Source::Action revoke;
    { sync::Lock guard{lock_}; revoke = revoke_; }
    if (revoke) revoke();
}
void Thread::Interest::complete() noexcept {
    auto publishing = owner_.roots_.hold();
    Source::Action ready;
    { sync::Lock guard{lock_};
      if (!active_ || !bindings_.drained()) return;
      active_ = false; revoke_ = {}; ready = std::exchange(ready_, {});
    }
    if (ready) ready(); // Last Interest access; the fixed Wait may be reused.
}

auto Thread::await_grant(const cap::GrantPin& grant, std::span<const object::ref<>*> gates,
    Source::Commit commit, Source::Action revoke, Source::Action ready) noexcept -> status_t {
    return interest_.admit(grant, gates, commit, revoke, ready);
}

auto Thread::begin_wait(Cpus& cpus) noexcept -> bool {
    return wait().begin(cpus, *this);
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

void Thread::request_stop() noexcept {
    sched::Dispatcher* home;
    {
        sync::Lock guard{lock_};
        if (stopping_) return;
        stopping_ = true;
        home = home_;
        if (!home) {
            libk_assert(state_ == State::Prepared || state_ == State::Exited);
            state_ = State::Exited;
        }
    }
    if (home) home->request_stop(*this);
    else finish(STATUS_CANCELED, EXIT_STOP);
}

void Thread::finish(status_t status, u32 origin) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(state_ == State::Exited && sc_ == nullptr);
        stopping_ = true;

    }
    report({.origin=origin, .kind=0, .status=status, .reserved=0, .pc=0, .address=0});
    if (work_) work_->post(finalizer_);
    else finalize(); // Exclusive construction rollback without a runtime worker.
}
auto Thread::check_control(mm::Pmm& pmm, const object::ref<>& from, const object::ref<>& to, usize slot)
    noexcept -> std::expected<object::Control::Check, status_t> {
    if (!to.control()) return object::Control::Check{};
    if (!control_scan_) {
        auto result = object::ControlScan::probe(from, to);
        if (result || result.error() != STATUS_PENDING) return result;
        auto self = object::ref<>::retain_alive(finalizer_.key());
        control_scan_.emplace(pmm, self.allocation()->quota());
    }
    return control_scan_->check(from, to, slot);
}
auto Thread::finish_control(status_t status) noexcept -> bool {
    if (control_scan_ && !control_scan_->drain()) { control_status_ = status; return false; }
    control_scan_.reset(); control_status_.reset(); return true;
}

void Thread::finalize() noexcept {
    bool finish;
    { sync::Lock guard{lock_}; if (state_ != State::Exited || !exit_.published()) return; finish = !stopped_; }
    if (!finish_control()) { post_finalize(); return; }
    if (finish) {
        auto publishing = roots_.hold();
        static_cast<void>(roots_.close());
        env_.detach_user();
        complete(); state_changed();
    }
    { sync::Lock guard{lock_}; if (!cleanup_) return; }
    if (!roots_.drained()) return;
    if (events_.snapshot().status != STATUS_CLOSED || exit_.events().snapshot().status != STATUS_CLOSED) {
        auto publishing = roots_.hold();
        events_.close(); exit_.events().close();
    }
    if (!roots_.drained() || !events_.drained() || !exit_.events().drained()) return;
    object::cleanup done;
    { sync::Lock guard{lock_}; done = std::move(cleanup_); }
    if (done) { if (work_) work_->close(finalizer_); done.complete(); }
}

void Thread::report(ThreadExit result) noexcept {
    Source::Publication thread_result, vm_result;
    sync::Lock guard{lock_};
    thread_result = exit_.claim(result);
    if (!thread_result) return;
    if (auto* vm = env_.vspace(); vm && finalizer_.key().valid()) {
        auto self = object::ref<>::retain_alive(finalizer_.key());
        vm_result = vm->fail_exec(self.id().value, result);
    }
}
[[noreturn]] void Thread::start(void* argument) noexcept {
    auto* const thread = static_cast<Thread*>(argument);
    libk_assert(thread != nullptr);
    libk_assert(thread->state_ == Thread::State::Running);

    Cpu& cpu = current_cpu();
    cpu.dispatcher().on_context_enter();

    if (thread->user()) {
        thread->return_user();
        arch::resume_user(thread->frame());
    }
    volatile byte stack_marker{};
    libk_assert(thread->transient() || thread->contains_stack(reinterpret_cast<usize>(&stack_marker)));
    const auto start = std::get<Kernel>(thread->run_).start;
    libk_assert(start.entry);
    start.entry(start.argument);
    libk_assert(!thread->idle());
    sched::exit_current();
}

void Thread::return_user() noexcept {
    libk_assert(user() && !arch::interrupts_enabled() && !in_kernel_);
    for (;;) {
        if (waiting()) {
            if (wait_.ready()) static_cast<void>(wait_.finish());
            else current_cpu().dispatcher().request_reschedule(sched::DispatchReason::Block);
        }
        if (!waiting()) if (const auto result = wait_result()) {
            arch::TrapCtx context{frame()};
            context.set_result(0, static_cast<usize>(static_cast<isize>(result->status)));
            context.set_result(1, result->value);
            context.set_result(2, result->sequence);
        }
        current_cpu().dispatcher().resched();
        libk_assert(current_cpu().current == this);
        // A ready publication can win between the test and the block attempt.
        if (!waiting()) { env_.root().sync(current_cpu()); return; }
    }
}

auto Thread::draining() const noexcept -> bool {
    return in_kernel_ || wait_.attached();
}

auto Thread::stop_ready() const noexcept -> bool {
    sync::Lock guard{lock_};
    return stopping_ && !in_kernel_ && !wait_.attached();
}

auto Thread::bind_roots(const cap::Resolved<mm::VSpace>& space,
                       const cap::Resolved<cap::CSpace>& caps) noexcept
    -> std::expected<void, cap::GrantError> {
    if (state_ != State::Prepared || sc_ != nullptr || !env_.user_bound())
        return std::unexpected(cap::GrantError::InvalidState);
    auto self = object::ref<>::retain_alive(finalizer_.key());
    const object::ref<>* gates[]{&self};
    const cap::GrantPin* grants[]{&space.pin(), &caps.pin()};
    return roots_.attach(grants, gates) == STATUS_OK ? std::expected<void, cap::GrantError>{}
        : std::unexpected(cap::GrantError::InvalidState);
}
void Thread::stop() noexcept {
    auto publishing = roots_.hold();
    request_stop(); state_changed();
}
void Thread::retire(object::cleanup&& cleanup) noexcept {
    auto publishing = roots_.hold();
    { sync::Lock guard{lock_}; libk_assert(!cleanup_); cleanup_ = std::move(cleanup); }
    request_stop(); post_finalize();
}

void Thread::state_changed() noexcept {
    auto publishing = roots_.hold();
    Source::Publication publication;
    bool changed;
    {
        sync::Lock guard{lock_};
        const auto flags = (enabled_ ? THREAD_ENABLED : 0U) | (applied_enabled_ ? THREAD_APPLIED : 0U)
            | (destination_ ? THREAD_MIGRATING : 0U) | (sc_ ? THREAD_BOUND : 0U) | (stopping_ ? THREAD_STOPPING : 0U) | (stopped_ ? THREAD_STOPPED : 0U);
        changed = events_.snapshot().value != flags;
        publication = events_.prepare(flags, true);
    }
    if (changed) object::changed(finalizer_.key());
}

void Thread::set_state(Thread::State state) noexcept {
    sync::Lock guard{lock_};
    state_ = state;
    if (state == State::Running && Cpu::local()) recorded_at_ = current_cpu().dispatcher().clock().rd_ticks();
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

auto Thread::cpu_domain() noexcept -> object::ref<> {
    object::ref<sched::Sc> context;
    { sync::Lock guard{lock_}; context = sc_ref_; }
    if (!context) return {};
    sync::Lock guard{context->lock_};
    return context->domain_ ? object::ref<>::retain_alive(context->domain_->close_work_.key()) : object::ref<>{};
}

auto Thread::claim_home(sched::Dispatcher& home) noexcept -> bool {
    sync::Lock guard{lock_};
    if ((home_ != nullptr && home_ != &home) || (stopping_ && home_ == nullptr))
        return false;
    home_ = &home;
    cpu_ = home.id();
    if (!home_epoch_) home_epoch_ = 1;
    return true;
}

auto Thread::owned_by(const sched::Dispatcher& home) const noexcept -> bool {
    sync::Lock guard{lock_};
    return home_ == &home;
}

auto Thread::home_cpu() const noexcept -> CpuId {
    sync::Lock guard{lock_};
    libk_assert(home_);
    return home_.load(std::memory_order_acquire)->id();
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

auto Thread::migrate(Cpus& cpus, CpuId cpu, u64 epoch) noexcept -> std::expected<u64, status_t> {
    auto* target = cpus.get(cpu);
    if (!target || !target->online()) return std::unexpected(STATUS_BAD_ARGS);
    object::ref<sched::Sc> sc;
    { sync::Lock guard{lock_}; sc = sc_ref_ ? sc_ref_ : pending_sc_; }
    if (!sc) return std::unexpected(STATUS_BUSY);
    sched::Dispatcher* previous;
    { sync::Lock resource{sc->lock_};
      sync::Lock guard{lock_};
      if (home_epoch_ != epoch) return std::unexpected(STATUS_STALE);
      if (!user() || !home_ || stopping_ || stopped_ || destination_ || detach_sc_
          || sc->owner_ != this || sc->withdrawing_) return std::unexpected(STATUS_BUSY);
      if (cpu.raw >= 64 || !(sc->config_.cpus & (u64{1} << cpu.raw))) return std::unexpected(STATUS_DENIED);
      previous = home_;
      if (previous == &target->dispatcher()) return home_epoch_;
      if (home_epoch_ == std::numeric_limits<u64>::max()) return std::unexpected(STATUS_EXHAUSTED);
      destination_ = &target->dispatcher();
    }
    state_changed(); previous->request_control(*this); return epoch + 1;
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
    const auto runtime = runtime_ + (state_ == State::Running && now > recorded_at_ ? now - recorded_at_ : 0);
    return {.version = 2, .size = sizeof(ThreadInfo), .requested = requested_, .applied = applied_,
        .runtime = runtime, .association = association_,
        .flags = (enabled_ ? THREAD_ENABLED : 0U) | (applied_enabled_ ? THREAD_APPLIED : 0U)
            | (state_ == State::Running ? THREAD_RUNNING : 0U) | (wait_.attached() ? THREAD_WAITING : 0U)
            | (destination_ ? THREAD_MIGRATING : 0U) | (sc_ ? THREAD_BOUND : 0U) | (stopping_ ? THREAD_STOPPING : 0U) | (stopped_ ? THREAD_STOPPED : 0U),
        .cpu = static_cast<u32>(cpu_.raw), .home_epoch=home_epoch_,
        .destination=static_cast<u32>(destination_ ? destination_->id().raw : cpu_.raw), .reserved=0};
}
auto Thread::can_bind(const sched::Sc& binding) const noexcept -> bool {
    libk_assert(lock_.held());
    auto* home = home_.load(std::memory_order_acquire);
    return !idle() && !sc_ && !pending_sc_ && !stopping_ && !stopped_ && home
        && binding.domain_ && binding.domain_->accepting() && home->id().raw < 64
        && (binding.config_.cpus & (u64{1} << home->id().raw));
}
void Thread::bind(object::ref<sched::Sc> binding, u64 generation) noexcept {
    libk_assert(can_bind(binding.get()));
    pending_sc_ = std::move(binding); association_ = generation;
}

void Thread::complete() noexcept {
    sync::Lock guard{lock_};
    libk_assert(state_ == State::Exited && sc_ == nullptr);
    home_ = nullptr;
    destination_ = nullptr; transferred_ = false; wait_when_.reset();
    stopped_ = true;
}
