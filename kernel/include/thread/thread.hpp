// Thread is a stable kernel execution object. Its home stack belongs only to it.

#pragma once

#include <arch/user.hpp>
#include <core/types.hpp>
#include <execution/execution.hpp>
#include <execution/authority.hpp>
#include <execution/stop.hpp>
#include <libk/noncopyable.hpp>
#include <libk/expected.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/optional.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <libk/typetraits.hpp>
#include <libk/variant.hpp>
#include <sched/remote_queue.hpp>
#include <trap/event.hpp>
#include <operation/wait.hpp>

namespace kernel {

class Thread;
class CpuRegistry;
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
    [[nodiscard]] auto waiting() const noexcept -> bool {
        return wait_.attached();
    }
    [[nodiscard]] auto wait_ready() const noexcept -> bool;
    [[nodiscard]] auto begin_wait(
        operation::Completion& relation,
        CpuRegistry& cpus) noexcept -> bool;
    void resume_wait(arch::TrapContext& trap) noexcept;
    void cancel_wait() noexcept;
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
    void finish_stop() noexcept;

    using StopList = libk::IntrusiveList<
        execution::Stop, &execution::Stop::hook_>;

    Execution execution_;
    execution::Authority authority_;
    libk::variant<KernelStart, UserStart> start_;
    Kind kind_{Kind::Normal};
    libk::optional<kernel::trap::Event> user_fault_{};
    operation::Wait wait_{};
    u64 user_syscalls_{};
    mutable libk::TicketSpinLock stop_lock_{};
    StopList stops_{};
    bool stop_requested_{};
    bool stopped_{};
};

static_assert(!libk::is_copy_constructible_v<Thread>);
static_assert(!libk::is_move_constructible_v<Thread>);

} // namespace kernel
