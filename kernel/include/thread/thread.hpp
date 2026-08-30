// Thread is a stable kernel execution object. Its home stack belongs only to it.

#pragma once

#include <arch/user.hpp>
#include <core/types.hpp>
#include <execution/execution.hpp>
#include <execution/authority.hpp>
#include <execution/frame.hpp>
#include <execution/stop.hpp>
#include <libk/noncopyable.hpp>
#include <libk/expected.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/optional.hpp>
#include <pager/claim.hpp>
#include <sync/lock.hpp>
#include <libk/typetraits.hpp>
#include <libk/variant.hpp>
#include <sched/remote_queue.hpp>
#include <trap/event.hpp>
#include <operation/wait.hpp>
#include <fault/terminal.hpp>

namespace kernel {

class Thread;
class CpuRegistry;
namespace fault {
class TerminalObservation;
}
namespace ipc {
class Notification;
}
namespace operation {
class Completion;
}
namespace execution {
class Target;
}
namespace sched {
class Binding;
class SchedulingContext;
class CpuDispatcher;
class RemoteQueue;
}

class Thread final : private libk::noncopyable_nonmovable {
public:
    using State = ExecutionState;

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
        KernelStack&& home_stack,
        ExecutionBinding&& execution,
        KernelStart start,
        Kind kind = Kind::Normal) noexcept;
    Thread(
        KernelStack&& home_stack,
        ExecutionBinding&& execution,
        UserStart start,
        Kind kind = Kind::Normal) noexcept;
    Thread(
        kernel::resource::Charge&& stack_charge,
        KernelStack&& home_stack,
        ExecutionBinding&& execution,
        UserStart start,
        Kind kind = Kind::Normal) noexcept;
    ~Thread() noexcept;

    [[nodiscard]] auto state() const noexcept -> State {
        return execution_.state();
    }
    [[nodiscard]] auto home_stack_base() const noexcept -> usize;
    [[nodiscard]] auto home_stack_top() const noexcept -> usize;
    [[nodiscard]] auto kind() const noexcept -> Kind { return kind_; }
    [[nodiscard]] auto idle() const noexcept -> bool {
        return kind_ == Kind::Idle;
    }
    [[nodiscard]] auto binding() noexcept -> sched::Binding* {
        return execution_.scheduler_binding();
    }
    [[nodiscard]] auto binding() const noexcept -> const sched::Binding* {
        return execution_.scheduler_binding();
    }
    [[nodiscard]] auto execution() noexcept -> Execution& {
        return execution_;
    }
    [[nodiscard]] auto execution() const noexcept -> const Execution& {
        return execution_;
    }
    [[nodiscard]] auto current_stack_base() const noexcept -> usize;
    [[nodiscard]] auto current_stack_top() const noexcept -> usize;
    [[nodiscard]] auto contains_stack(usize address) const noexcept -> bool;
    [[nodiscard]] auto effective_binding() noexcept -> ExecutionBinding&;
    [[nodiscard]] auto effective_binding() const noexcept
        -> const ExecutionBinding&;
    [[nodiscard]] auto ipc_buffer() noexcept -> ipc::Buffer*;
    [[nodiscard]] auto ipc_buffer() const noexcept -> const ipc::Buffer*;
    [[nodiscard]] auto active_frame() const noexcept -> execution::Frame* {
        return active_;
    }
    [[nodiscard]] auto current_wait() noexcept -> operation::Wait&;
    [[nodiscard]] auto current_wait() const noexcept -> const operation::Wait&;
    [[nodiscard]] auto frame_depth() const noexcept -> usize;
    [[nodiscard]] auto cancel_pending() const noexcept -> bool;
    void push(execution::Frame& frame) noexcept;
    void pop(execution::Frame& frame) noexcept;
    [[nodiscard]] auto binding_before(
        const execution::Frame& frame) noexcept -> ExecutionBinding&;
    [[nodiscard]] auto ipc_before(
        const execution::Frame& frame) noexcept -> ipc::Buffer*;
    [[nodiscard]] auto authorize(
        const cap::Resolved<kernel::mm::VSpace>& vspace,
        const cap::Resolved<cap::CSpace>& cspace) noexcept
        -> libk::Expected<void, cap::GrantError>;
    void record_user_fault(const kernel::trap::Event& event) noexcept {
        user_fault_ = event;
    }
    [[nodiscard]] auto user_fault() const noexcept
        -> const libk::optional<kernel::trap::Event>& { return user_fault_; }
    void note_user_syscall() noexcept { ++user_syscalls_; }
    [[nodiscard]] auto user_syscalls() const noexcept -> u64 {
        return user_syscalls_;
    }
    [[nodiscard]] auto terminal() const noexcept -> const fault::TerminalRecord& {
        return terminal_;
    }
    [[nodiscard]] auto terminal() noexcept -> fault::TerminalRecord& {
        return terminal_;
    }
    [[nodiscard]] auto observe_terminal(
        ipc::Notification& notification,
        u64 badge) noexcept -> bool;
    void clear_terminal_observation() noexcept;
    [[nodiscard]] auto waiting() const noexcept -> bool {
        return wait_.attached();
    }
    [[nodiscard]] auto wait_ready() const noexcept -> bool;
    [[nodiscard]] auto begin_wait(
        operation::Completion& relation,
        CpuRegistry& cpus) noexcept -> bool;
    [[nodiscard]] auto resume_wait(arch::TrapContext& trap) noexcept -> bool;
    void cancel_wait() noexcept;
    // Terminal edge: every outstanding Pager service claim registered by this
    // Thread returns to Published through the requeue owner path. Safe to
    // call more than once; entries clear as they are invalidated.
    void release_pager_claims() noexcept;
    [[nodiscard]] auto pager_claims() noexcept -> pager::ClaimIndex& {
        return pager_claims_;
    }
    [[nodiscard]] auto prepare_retire() const noexcept -> bool;

private:
    friend class sched::Binding;
    friend class sched::SchedulingContext;
    friend class sched::CpuDispatcher;
    friend class sched::RemoteQueue;
    friend class execution::Stop;
    friend class execution::Target;

    [[noreturn]] static void start(void* argument) noexcept;
    void request_stop(execution::Stop& request) noexcept;
    void finish_terminal(
        fault::Reason reason,
        myos_status_t status) noexcept;
    void finish_stop() noexcept;
    void finish_exit(myos_status_t status = MYOS_STATUS_OK) noexcept;

    using StopList = libk::IntrusiveList<
        execution::Stop, &execution::Stop::hook_>;

    Execution execution_;
    execution::Authority authority_;
    libk::variant<KernelStart, UserStart> start_;
    Kind kind_{Kind::Normal};
    libk::optional<kernel::trap::Event> user_fault_{};
    operation::Wait wait_{};
    execution::Frame* active_{};
    pager::ClaimIndex pager_claims_{};
    u64 user_syscalls_{};
    fault::TerminalRecord terminal_{};
    fault::TerminalObservation* terminal_observation_{};
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::ThreadStop>
        stop_lock_{};
    StopList stops_{};
    bool stop_requested_{};
    bool stopped_{};
};

static_assert(!libk::is_copy_constructible_v<Thread>);
static_assert(!libk::is_move_constructible_v<Thread>);

} // namespace kernel
