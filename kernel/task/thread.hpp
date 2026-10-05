// Thread is a stable kernel execution object. Its home stack belongs only to it.

#pragma once

#include <trap.hpp>
#include <base/types.hpp>
#include <arch/context.hpp>
#include <array>
#include <cap/grant.hpp>
#include <task/exit.hpp>
#include <libk/delegate.hpp>
#include <mm/kspace.hpp>
#include <resource/sponsorship.hpp>
#include <task/env.hpp>
#include <libk/noncopyable.hpp>
#include <expected>
#include <libk/intrusive_list.hpp>
#include <optional>
#include <mm/pager.hpp>
#include <sync.hpp>
#include <type_traits>
#include <variant>
#include <libk/sync/atomic.hpp>
#include <sched/remote_queue.hpp>
#include <trap/event.hpp>
#include <wait.hpp>

class Thread;
class CpuRegistry;
namespace ipc {
class Notification;
class Activation;
}
class Completion;

namespace sched {
class Sc;
class Dispatcher;
class RemoteQueue;
}
// A caller-owned interest in exit cleanup. Completion may destroy it.
class Stop final {
public:
    using Notifier = libk::delegate<void() noexcept>;
    explicit Stop(Notifier notify = {}) noexcept : notify_(notify) {}
    Stop(const Stop&) = delete;
    auto operator=(const Stop&) -> Stop& = delete;
    ~Stop() noexcept;

    [[nodiscard]] auto started() const noexcept -> bool {
        return phase_.load<libk::MemoryOrder::Acquire>() != Phase::Idle;
    }
    [[nodiscard]] auto complete() const noexcept -> bool {
        return phase_.load<libk::MemoryOrder::Acquire>() == Phase::Complete;
    }
    void start(Thread&) noexcept;

private:
    friend class Thread;

    void finish(Thread&) noexcept;

    Notifier notify_{};
    Thread* target_{};
    libk::IntrusiveListHook hook_{};
    enum class Phase : u8 { Idle, Started, Complete };
    libk::Atomic<Phase> phase_{Phase::Idle};
};

class Thread final : private libk::noncopyable_nonmovable {
public:
    enum class State : u8 { Prepared, Ready, Running, Throttled, Blocked, Exited };

    enum class Kind : u8 {
        Normal,
        Idle,
    };

    using Entry = void (*)(void*) noexcept;

    struct KernelStart final {
        Entry entry{};
        void* argument{};
    };

    using UserStart = arch::UserStart;

    Thread(
        mm::Stack&& home_stack,
        Env&& roots,
        KernelStart start,
        Kind kind = Kind::Normal) noexcept;
    Thread(
        mm::Stack&& home_stack,
        Env&& roots,
        UserStart start,
        Kind kind = Kind::Normal) noexcept;
    Thread(
        resource::Charge&& stack_charge,
        mm::Stack&& home_stack,
        Env&& roots,
        UserStart start,
        Kind kind = Kind::Normal) noexcept;
    ~Thread() noexcept;

    [[nodiscard]] auto state() const noexcept -> State {
        return state_;
    }
    [[nodiscard]] auto home_stack_base() const noexcept -> usize;
    [[nodiscard]] auto home_stack_top() const noexcept -> usize;
    [[nodiscard]] auto kind() const noexcept -> Kind { return kind_; }
    [[nodiscard]] auto idle() const noexcept -> bool {
        return kind_ == Kind::Idle;
    }
    [[nodiscard]] auto sc() noexcept -> sched::Sc* {
        return sc_;
    }
    [[nodiscard]] auto sc() const noexcept -> const sched::Sc* {
        return sc_;
    }
    [[nodiscard]] auto identity() const noexcept -> usize {
        return reinterpret_cast<usize>(this);
    }
    [[nodiscard]] auto ctx() noexcept -> arch::KernelContext& { return ctx_; }
    [[nodiscard]] auto exit() noexcept -> Exit& { return exit_; }
    [[nodiscard]] auto exit() const noexcept -> const Exit& { return exit_; }
    [[nodiscard]] auto draining() const noexcept -> bool;
    [[nodiscard]] auto stop_ready() const noexcept -> bool;
    [[nodiscard]] auto stop_requested() const noexcept -> bool;
    [[nodiscard]] auto stopped() const noexcept -> bool;
    [[nodiscard]] auto claim_home(sched::Dispatcher&) noexcept -> bool;
    [[nodiscard]] auto owned_by(const sched::Dispatcher&) const noexcept -> bool;
    [[nodiscard]] auto try_bind(sched::Sc&) noexcept -> bool;
    [[nodiscard]] auto release_sc(sched::Sc&, sched::Dispatcher*) noexcept -> bool;
    [[nodiscard]] auto current_stack_base() const noexcept -> usize;
    [[nodiscard]] auto current_stack_top() const noexcept -> usize;
    [[nodiscard]] auto contains_stack(usize address) const noexcept -> bool;
    [[nodiscard]] auto env() noexcept -> Env&;
    [[nodiscard]] auto env() const noexcept
        -> const Env&;
    [[nodiscard]] auto ipc_buffer() noexcept -> ipc::Buffer*;
    [[nodiscard]] auto ipc_buffer() const noexcept -> const ipc::Buffer*;
    [[nodiscard]] auto activation() const noexcept -> ipc::Activation* {
        return active_;
    }
    [[nodiscard]] auto current_wait() noexcept -> Wait&;
    [[nodiscard]] auto current_wait() const noexcept -> const Wait&;
    [[nodiscard]] auto call_depth() const noexcept -> usize;
    [[nodiscard]] auto cancel_pending() const noexcept -> bool;
    void push(ipc::Activation& frame) noexcept;
    void pop(ipc::Activation& frame) noexcept;
    [[nodiscard]] auto env_before(
        const ipc::Activation& frame) noexcept -> Env&;
    [[nodiscard]] auto ipc_before(
        const ipc::Activation& frame) noexcept -> ipc::Buffer*;
    [[nodiscard]] auto authorize(
        const cap::Resolved<mm::VSpace>& vspace,
        const cap::Resolved<cap::CSpace>& cspace) noexcept
        -> std::expected<void, cap::GrantError>;
    void record_user_fault(const trap::Event& event) noexcept {
        user_fault_ = event;
    }
    [[nodiscard]] auto user_fault() const noexcept
        -> const std::optional<trap::Event>& { return user_fault_; }
    void note_user_syscall() noexcept { ++user_syscalls_; }
    [[nodiscard]] auto user_syscalls() const noexcept -> u64 {
        return user_syscalls_;
    }
    [[nodiscard]] auto waiting() const noexcept -> bool {
        return current_wait().attached();
    }
    [[nodiscard]] auto begin_wait(
        Completion& relation,
        CpuRegistry& cpus) noexcept -> bool;
    void cancel_wait() noexcept;
    // Kernel calls retain their stack until all local transactions have drained.
    void enter_kernel() noexcept { libk_assert(!in_kernel_); in_kernel_ = true; }
    void leave_kernel() noexcept { libk_assert(in_kernel_); in_kernel_ = false; }
    void block() noexcept;
    // Called only after switching away from every popped frame stack.
    void release_calls() noexcept;
    // Terminal edge: every outstanding Pager service claim registered by this
    // Thread returns to Published through the requeue owner path. Safe to
    // call more than once; entries clear as they are invalidated.
    [[nodiscard]] auto claims() noexcept -> Pager::Claims& {
        return claims_;
    }
    [[nodiscard]] auto prepare_retire() const noexcept -> bool;
    void retire(object::cleanup&&) noexcept;

private:
    friend class sched::Dispatcher;
    friend class sched::RemoteQueue;
    friend class Stop;

    [[noreturn]] static void start(void* argument) noexcept;
    void request_stop(Stop& request) noexcept;
    void finish(
        Exit::Reason reason,
        myos_status_t status) noexcept;
    void finish_stop() noexcept;
    void finish_exit(myos_status_t status = MYOS_STATUS_OK) noexcept;

    struct Root {
        Root(Thread& owner) noexcept : owner(&owner), cap(this, grant_ops_) {}
        Thread* owner;
        cap::GrantAttachment cap;
        cap::GrantWork work{};
        Root* next{};
        enum class Phase : u8 { Idle, Attached, Detaching };
        Phase phase{Phase::Idle};
    };
    // Foreign detach/stop/work callbacks may synchronously complete cleanup.
    // Keep the entity resident until the entire publishing call has returned.
    class Flight {
    public:
        explicit Flight(Thread&) noexcept;
        ~Flight() noexcept;
        Flight(const Flight&) = delete;
        auto operator=(const Flight&) -> Flight& = delete;
    private:
        Thread& owner_;
    };
    static const cap::GrantAttachmentOps grant_ops_;
    void detach(Root&) noexcept;
    void released(Root&) noexcept;
    void invalidate(Root&, cap::GrantWork&&) noexcept;
    void close_grants() noexcept;
    void finish_retire() noexcept;

    void set_state(State) noexcept;
    void prepare(usize top) noexcept;
    void complete() noexcept;

    resource::Charge stack_charge_{};
    mm::Stack stack_;
    arch::KernelContext ctx_{};
    Env env_;
    State state_{State::Prepared};
    sched::Sc* sc_{};
    sched::Dispatcher* home_{};
    mutable sync::Spin lock_{};
    libk::IntrusiveList<Stop, &Stop::hook_> stops_{};
    bool stopping_{};
    bool stopped_{};

    std::array<Root, 2> roots_;
    Root* grants_{};
    void unlink(Root&) noexcept;
    mutable sync::Spin grants_lock_{};
    Stop revoke_;
    object::cleanup cleanup_{};
    usize publishers_{};
    bool attaching_{};
    bool grants_closed_{};
    bool revoking_{};
    Exit exit_{};
    std::variant<KernelStart, UserStart> start_;
    Kind kind_{Kind::Normal};
    std::optional<trap::Event> user_fault_{};
    Wait wait_{};
    ipc::Activation* active_{};
    ipc::Activation* retired_{};
    bool in_kernel_{};
    Pager::Claims claims_{};
    u64 user_syscalls_{};
};

static_assert(!std::is_copy_constructible_v<Thread>);
static_assert(!std::is_move_constructible_v<Thread>);
