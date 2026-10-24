// User execution keeps only its return frame; kernel threads own their stacks.

#pragma once

#include <trap.hpp>
#include <libk/types.hpp>
#include <cpu.hpp>
#include <array>
#include <cap/grant.hpp>
#include <libk/delegate.hpp>
#include <mm/kspace.hpp>
#include <task/env.hpp>
#include <object/scope.hpp>
#include <libk/noncopyable.hpp>
#include <expected>
#include <libk/intrusive_list.hpp>
#include <optional>
#include <sched/sched.hpp>
#include <sync.hpp>
#include <type_traits>
#include <variant>
#include <atomic>
#include <wait.hpp>
#include <uapi/task.h>
#include <work.hpp>

class Thread;
class Cpus;

namespace sched {
class Sc;
class Dispatcher;
} // namespace sched
class Thread final : private libk::noncopyable_nonmovable {
  public:
    class Exit final : private libk::noncopyable_nonmovable {
        mutable sync::Spin lock_;
        std::optional<ThreadExit> result_;
        Src<SOURCE_READY> events_;
    public:
        explicit Exit(Source::Drained drained = {}) noexcept : events_(drained) {}
        auto read() const noexcept -> std::optional<status_t> { sync::Lock guard{lock_}; return result_ ? std::optional{result_->status} : std::nullopt; }
        auto published() const noexcept -> bool { return read().has_value(); }
        auto events() noexcept -> Source& { return events_; }
        auto claim(ThreadExit result) noexcept -> Source::Publication {
            sync::Lock guard{lock_};
            if (result_) return {};
            result_ = result;
            return events_.prepare(SOURCE_READY, true);
        }
        auto claim(status_t status) noexcept -> bool {
            return bool(claim(ThreadExit{.origin=EXIT_NORMAL, .kind=0, .status=status, .reserved=0, .pc=0, .address=0}));
        }
    };
    enum class State : u8 { Prepared, Ready, Running, Throttled, Blocked, Exited };

    enum class Kind : u8 {
        Normal,
        Idle,
        Worker,
    };

    using Entry = void (*)(void*) noexcept;

    struct KernelStart final {
        Entry entry{};
        void* argument{};
    };

    using UserStart = arch::UserStart;

    Thread(mm::Stack&& home_stack, Env&& roots, KernelStart start,
           Kind kind = Kind::Normal) noexcept;
    Thread(Env&& roots, UserStart start) noexcept;
    Thread(Env&& roots, KernelStart step) noexcept;
    ~Thread() noexcept;

    [[nodiscard]] auto state() const noexcept -> State { sync::Lock guard{lock_}; return state_; }
    [[nodiscard]] auto stack_base() const noexcept -> usize;
    [[nodiscard]] auto stack_top() const noexcept -> usize;
    [[nodiscard]] auto kind() const noexcept -> Kind { return kind_; }
    [[nodiscard]] auto idle() const noexcept -> bool { return kind_ == Kind::Idle; }
    [[nodiscard]] auto identity() const noexcept -> usize { return reinterpret_cast<usize>(this); }
    [[nodiscard]] auto ctx() noexcept -> arch::Ctx& { return std::get<Kernel>(run_).ctx; }
    auto cpu_domain() noexcept -> object::ref<>;
    auto transient() const noexcept -> bool { return user() || kind_ == Kind::Worker; }
    auto user() const noexcept -> bool { return std::holds_alternative<arch::TrapFrame>(run_); }
    auto frame() noexcept -> arch::TrapFrame& { return std::get<arch::TrapFrame>(run_); }
    void report(ThreadExit) noexcept;
    void return_user() noexcept;
    [[nodiscard]] auto exit() noexcept -> Exit& { return exit_; }
    [[nodiscard]] auto exit() const noexcept -> const Exit& { return exit_; }
    [[nodiscard]] auto draining() const noexcept -> bool;
    [[nodiscard]] auto stop_ready() const noexcept -> bool;
    [[nodiscard]] auto stop_requested() const noexcept -> bool;
    [[nodiscard]] auto stopped() const noexcept -> bool;
    [[nodiscard]] auto claim_home(sched::Dispatcher&) noexcept -> bool;
    [[nodiscard]] auto owned_by(const sched::Dispatcher&) const noexcept -> bool;

    [[nodiscard]] auto home_cpu() const noexcept -> CpuId;
    [[nodiscard]] auto execution(bool enabled) noexcept -> std::optional<u64>;
    auto migrate(Cpus&, CpuId, u64 epoch) noexcept -> std::expected<u64, status_t>;
    auto detach_resource(u64 generation) noexcept -> status_t;
    void links(u64 cursor, LinkRecords& out) const noexcept { roots_.links(out, LINK_ROOT, cursor); }
    auto info(u64 now) const noexcept -> ThreadInfo;
    auto events() noexcept -> Source& { return events_; }
    void state_changed() noexcept;
    auto await_grant(const cap::GrantPin&, std::span<const object::ref<>*>,
        Source::Commit commit, Source::Action revoke, Source::Action ready) noexcept -> status_t;
    [[nodiscard]] auto contains_stack(usize address) const noexcept -> bool {
        return !transient() && std::get<Kernel>(run_).stack->contains(address);
    }
    [[nodiscard]] auto env() noexcept -> Env& { return env_; }
    [[nodiscard]] auto env() const noexcept -> const Env& { return env_; }
    [[nodiscard]] auto ipc_buffer() noexcept -> mm::View* { return env_.ipc_buffer(); }
    [[nodiscard]] auto ipc_buffer() const noexcept -> const mm::View* {
        return env_.ipc_buffer();
    }
    [[nodiscard]] auto wait() noexcept -> Wait& { return wait_; }
    [[nodiscard]] auto wait() const noexcept -> const Wait& { return wait_; }
    [[nodiscard]] auto bind_roots(const cap::Resolved<mm::VSpace>& vspace,
                                 const cap::Resolved<cap::CSpace>& cspace) noexcept
        -> std::expected<void, cap::GrantError>;
    auto check_control(mm::Pmm&, const object::ref<>& from, const object::ref<>& to, usize slot)
        noexcept -> std::expected<object::Control::Check, status_t>;
    auto finish_control(status_t = STATUS_CANCELED) noexcept -> bool;
    auto control_status() const noexcept -> std::optional<status_t> { return control_status_; }
    void note_user_syscall() noexcept { ++user_syscalls_; }
    [[nodiscard]] auto user_syscalls() const noexcept -> u64 { return user_syscalls_; }
    [[nodiscard]] auto waiting() const noexcept -> bool { return wait().attached(); }
    [[nodiscard]] auto begin_wait(Cpus& cpus) noexcept -> bool;
    void cancel_wait() noexcept;
    void park(bool result) noexcept { libk_assert(waiting()); return_wait_ = result; }
    auto wait_result() noexcept -> std::optional<WaitResult> {
        libk_assert(!waiting());
        return std::exchange(return_wait_, false) ? std::optional{wait_.result()} : std::nullopt;
    }
    // No provider may schedule until its local transactions have drained.
    void enter_kernel() noexcept {
        libk_assert(!in_kernel_);
        in_kernel_ = true;
    }
    void leave_kernel() noexcept {
        libk_assert(in_kernel_);
        in_kernel_ = false;
    }
    void block() noexcept;
    void stop() noexcept;
    void retire(object::cleanup&&) noexcept;
    void bind_work(WorkQueue& work, object::Key owner) noexcept { work_ = &work; finalizer_.bind(owner); roots_.bind(owner); interest_.bind(owner); events_.bind_work(work, owner); exit_.events().bind_work(work, owner); if (!env_.bind_exec(exec_, owner, Source::Action::bind<&Thread::stop>(*this))) stop(); }

  private:
    friend class Wait;
    friend class sched::Dispatcher;
    friend class sched::Sc;
    friend class sched::Domain;
    auto can_bind(const sched::Sc&) const noexcept -> bool;
    void bind(object::ref<sched::Sc>, u64 generation) noexcept;

    [[noreturn]] static void start(void* argument) noexcept;
    void request_stop() noexcept;
    void finish(status_t status, u32 origin = EXIT_NORMAL) noexcept;
    void finalize() noexcept;
    WorkQueue* work_{};
    Work finalizer_{Work::Fn::bind<&Thread::finalize>(*this)};
    mm::Exec exec_;

    void post_finalize() noexcept { if (work_) work_->post(finalizer_); else finalize(); }
    void sources_drained() noexcept { post_finalize(); }

    void set_state(State) noexcept;
    void complete() noexcept;

    // Prepaid short observation authority, separate from environment roots.
    class Interest {
        Thread& owner_;
        mutable sync::Spin lock_;
        Source::Action revoke_, ready_;
        bool active_{};
        cap::Bindings<1, false, true> bindings_{
            Source::Action::bind<&Interest::invalidate>(*this), Source::Action::bind<&Interest::complete>(*this)};
        void invalidate() noexcept;
        void complete() noexcept;
        void end() noexcept { static_cast<void>(bindings_.close()); }
    public:
        explicit Interest(Thread& owner) noexcept : owner_(owner) {}
        ~Interest() noexcept { libk_assert(!active_); }
        void bind(object::Key key) noexcept { bindings_.bind(key); }
        auto admit(const cap::GrantPin&, std::span<const object::ref<>*>,
            Source::Commit, Source::Action, Source::Action) noexcept -> status_t;
    } interest_{*this};

    struct Kernel { std::optional<mm::Stack> stack; arch::Ctx ctx; KernelStart start; };
    std::optional<object::ControlScan> control_scan_;
    std::optional<status_t> control_status_;
    std::variant<Kernel, arch::TrapFrame> run_;
    Env env_;
    State state_{State::Prepared};
    sched::Sc* sc_{};
    object::ref<sched::Sc> sc_ref_{}, pending_sc_{};
    bool detach_sc_{};
    std::atomic<sched::Dispatcher*> home_{};
    sched::Dispatcher* destination_{};
    std::optional<u64> wait_when_;
    u64 home_epoch_{};
    bool transferred_{};
    sched::Node ready_{.thread = this};
    libk::intrusive_list_hook maintenance_hook_{};
    libk::intrusive_list_hook mail_hook_{};
    sched::Deadline refill_{sched::Deadline::Fn::bind<&Thread::refill>(*this)};
    void refill() noexcept;
    u8 actions_{};
    std::atomic<bool> mailed_{};
    bool wake_credit_{};
    bool enabled_{}, applied_enabled_{};
    Src<THREAD_ENABLED | THREAD_APPLIED | THREAD_MIGRATING | THREAD_BOUND | THREAD_STOPPING | THREAD_STOPPED> events_{Source::Drained::bind<&Thread::sources_drained>(*this)};
    u64 requested_{}, applied_{};
    u64 runtime_{}, recorded_at_{};
    CpuId cpu_{};
    u64 association_{};
    mutable sync::Spin lock_{};
    bool stopping_{};
    bool stopped_{};

    cap::Bindings<2> roots_{cap::Bindings<2>::Action::bind<&Thread::stop>(*this),
        cap::Bindings<2>::Action::bind<&Thread::post_finalize>(*this)};
    std::optional<cap::Bindings<2>::Hold> mail_hold_;
    object::cleanup cleanup_{};
    Exit exit_{Source::Drained::bind<&Thread::sources_drained>(*this)};
    Kind kind_{Kind::Normal};
    Wait wait_{};
    bool in_kernel_{}, return_wait_{};
    u64 user_syscalls_{};
};

static_assert(!std::is_copy_constructible_v<Thread>);
static_assert(!std::is_move_constructible_v<Thread>);
