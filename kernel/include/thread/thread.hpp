// Thread is a stable kernel execution object. Its home stack belongs only to it.

#pragma once

#include <arch/context.hpp>
#include <arch/user.hpp>
#include <core/types.hpp>
#include <libk/noncopyable.hpp>
#include <libk/expected.hpp>
#include <libk/optional.hpp>
#include <libk/typetraits.hpp>
#include <libk/variant.hpp>
#include <mm/kernel_stack.hpp>
#include <thread/execution.hpp>
#include <trap/event.hpp>

namespace kernel {

class Thread;
class CpuRegistry;
class WaitRelation;
namespace sched {
class Binding;
class SchedulingContext;
class CpuDispatcher;
}

class Thread final : private libk::noncopyable_nonmovable {
public:
    enum class State : u8 {
        Prepared,
        Ready,
        Running,
        Throttled,
        Blocked,
        Exited,
    };

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

    enum class RebindError : u8 {
        Active,
        Incompatible,
    };

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
    ~Thread() noexcept;

    [[nodiscard]] auto state() const noexcept -> State { return state_; }
    [[nodiscard]] auto home_stack_base() const noexcept -> usize;
    [[nodiscard]] auto home_stack_top() const noexcept -> usize;
    [[nodiscard]] auto kind() const noexcept -> Kind { return kind_; }
    [[nodiscard]] auto idle() const noexcept -> bool {
        return kind_ == Kind::Idle;
    }
    [[nodiscard]] auto binding() noexcept -> sched::Binding* {
        return binding_;
    }
    [[nodiscard]] auto binding() const noexcept -> const sched::Binding* {
        return binding_;
    }
    [[nodiscard]] auto execution() noexcept -> ExecutionBinding& {
        return execution_;
    }
    [[nodiscard]] auto execution() const noexcept -> const ExecutionBinding& {
        return execution_;
    }
    [[nodiscard]] auto rebind(ExecutionBinding&& binding) noexcept
        -> libk::Expected<void, RebindError>;
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
        return wait_ != nullptr;
    }
    [[nodiscard]] auto wait_ready() const noexcept -> bool;
    [[nodiscard]] auto begin_wait(
        WaitRelation& relation,
        CpuRegistry& cpus) noexcept -> bool;
    void resume_wait(arch::TrapContext& trap) noexcept;
    void cancel_wait() noexcept;

private:
    friend class sched::Binding;
    friend class sched::SchedulingContext;
    friend class sched::CpuDispatcher;

    [[noreturn]] static void start(void* argument) noexcept;
    KernelStack home_stack_;
    arch::KernelContext context_{};
    ExecutionBinding execution_;
    libk::variant<KernelStart, UserStart> start_;
    State state_{State::Prepared};
    Kind kind_{Kind::Normal};
    sched::Binding* binding_{};
    libk::optional<kernel::trap::Event> user_fault_{};
    WaitRelation* wait_{};
    u64 user_syscalls_{};
};

static_assert(!libk::is_copy_constructible_v<Thread>);
static_assert(!libk::is_move_constructible_v<Thread>);

} // namespace kernel
