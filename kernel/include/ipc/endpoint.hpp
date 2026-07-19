#pragma once

#include <arch/trap.hpp>
#include <arch/user.hpp>
#include <cap/grant.hpp>
#include <cap/resolved.hpp>
#include <core/types.hpp>
#include <execution/binding.hpp>
#include <execution/frame.hpp>
#include <execution/target.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <mm/kernel_stack.hpp>
#include <mm/memory_object.hpp>
#include <mm/node_pool.hpp>
#include <mm/user_view.hpp>
#include <object/object_cleanup.hpp>
#include <operation/completion.hpp>
#include <operation/wait.hpp>
#include <resource/sponsorship.hpp>
#include <sched/types.hpp>
#include <time/time.hpp>
#include <uapi/endpoint.h>

namespace kernel {
class CpuRegistry;
class Vproc;
}

namespace kernel::sched {
class CpuDispatcher;
}

namespace kernel::ipc {

enum class EndpointError : u8 {
    Closed,
    Busy,
    InvalidConfig,
    InvalidCaller,
    DepthExceeded,
    GenerationExhausted,
    QueueFull,
    BudgetTooLow,
    Denied,
};

struct EndpointConfig final {
    arch::UserStart entry{};
    usize capacity{};
    usize call_capacity{};
    usize max_depth{};
    time::Duration budget_floor{};
    sched::Urgency urgency_ceiling{*sched::Urgency::make(0)};
};

using CodePages = libk::InplaceVector<
    kernel::mm::PageLease, MYOS_ENDPOINT_MAX_CODE_PAGES>;
using StackPages = libk::InplaceVector<
    kernel::mm::PageLease, MYOS_ENDPOINT_MAX_STACK_PAGES>;

class Endpoint;
class Call;

// Stable Endpoint-owned slot. It is deliberately not an ObjectStore object:
// reply authority and lifetime are exactly the active stack frame.
class Activation final : private libk::noncopyable_nonmovable {
public:
    enum class State : u8 {
        Free,
        Preparing,
        Active,
        Replying,
        Canceling,
        Complete,
    };

    Activation(
        Endpoint& endpoint,
        ExecutionBinding& service,
        kernel::resource::Charge&& stack_charge,
        KernelStack&& kernel_stack,
        kernel::mm::UserView&& user_stack,
        libk::optional<Buffer>&& ipc,
        StackPages&& resident,
        kernel::mm::VirtAddr user_stack_top) noexcept;
    ~Activation() noexcept;

    [[nodiscard]] auto state() const noexcept -> State { return state_; }
    [[nodiscard]] auto frame() noexcept -> execution::Frame& { return frame_; }
    [[nodiscard]] auto endpoint() const noexcept -> Endpoint& {
        return *endpoint_;
    }

private:
    friend class Endpoint;

    Endpoint* endpoint_{};
    kernel::resource::Charge stack_charge_{};
    KernelStack kernel_stack_;
    kernel::mm::UserView user_stack_;
    libk::optional<Buffer> ipc_{};
    StackPages resident_{};
    operation::Wait wait_{};
    execution::Frame frame_;
    Call* call_{};
    Call* root_{};
    kernel::mm::VirtAddr user_stack_top_{};
    u64 generation_{};
    usize depth_{};
    State state_{State::Free};
};

// Endpoint-owned call truth. It survives queueing, admission and activation;
// Activation contributes only the executable stack/root slot.
class Call final : private libk::noncopyable_nonmovable {
public:
    enum class State : u8 {
        Free,
        Preparing,
        Queued,
        Admitted,
        Active,
        Replying,
        Canceling,
        Reaping,
        Complete,
    };

    explicit Call(Endpoint& endpoint) noexcept;
    ~Call() noexcept;

private:
    friend class Endpoint;
    friend class ::kernel::Vproc;

    enum class CallerOwner : u8 {
        None,
        Direct,
        Runtime,
    };

    enum class ContinuationState : u8 {
        None,
        Materialized,
        Parking,
        Parked,
        ResumePending,
        Resuming,
    };

    [[nodiscard]] auto complete() const noexcept -> bool;
    [[nodiscard]] auto read() noexcept -> operation::Result;
    void release() noexcept;
    [[nodiscard]] auto cancel() noexcept -> bool;
    void resume(arch::TrapContext& trap) noexcept;
    void wake_parked() noexcept;
    static void invalidate_authority(
        void* context,
        cap::GrantWork&& work,
        cap::GrantInvalidation reason) noexcept;
    static void authority_released(void* context) noexcept;

    static const cap::GrantAttachmentOps authority_ops_;

    Endpoint* endpoint_{};
    operation::Completion completion_;
    libk::ManualLifetime<cap::GrantAttachment> authority_{};
    execution::TargetHold caller_{};
    arch::UserFrame caller_frame_{};
    myos_user_context caller_context_{};
    arch::UserFrame parked_frame_{};
    arch::UserFrame base_frame_{};
    myos_user_context base_context_{};
    execution::Frame* parked_top_{};
    Activation* activation_{};
    usize arguments_[3]{};
    operation::Result result_{};
    u64 generation_{};
    u64 sequence_{};
    usize depth_{};
    usize urgency_{};
    usize badge_{};
    usize cap_limit_{};
    isize cancel_status_{MYOS_STATUS_CANCELED};
    CpuRegistry* cpus_{};
    usize publishers_{};
    bool cancel_pending_{};
    bool async_{};
    bool operation_committed_{};
    operation::Key operation_{};
    Vproc* vproc_{};
    CallerOwner caller_owner_{CallerOwner::None};
    ContinuationState continuation_{ContinuationState::None};
    State state_{State::Free};
};

enum class CallDisposition : u8 {
    Entered,
    Blocking,
    Pending,
};

struct CallResult final {
    CallDisposition disposition{CallDisposition::Entered};
    operation::Key operation{};
};

// Immutable protected-procedure entry plus a fixed, sponsor-paid Activation
// pool. The Endpoint is the sole owner of service roots, code relation,
// admission state and every callee stack.
class Endpoint final : private libk::noncopyable_nonmovable {
public:
    static constexpr usize max_activations = MYOS_ENDPOINT_MAX_ACTIVATIONS;

    Endpoint(
        kernel::mm::Pmm& pmm,
        ExecutionBinding&& service,
        kernel::mm::UserView&& code,
        CodePages&& resident_code,
        EndpointConfig config) noexcept;
    ~Endpoint() noexcept;

    [[nodiscard]] auto add_activation(
        kernel::resource::Charge&& stack_charge,
        KernelStack&& kernel_stack,
        kernel::mm::UserView&& user_stack,
        libk::optional<Buffer>&& ipc,
        StackPages&& resident,
        kernel::mm::VirtAddr user_stack_top) noexcept
        -> libk::Expected<void, EndpointError>;
    [[nodiscard]] auto add_call() noexcept
        -> libk::Expected<void, EndpointError>;
    [[nodiscard]] auto open() noexcept -> libk::Expected<void, EndpointError>;
    [[nodiscard]] auto call(
        const cap::Resolved<Endpoint>& authority,
        execution::Target caller,
        arch::TrapContext& trap,
        sched::CpuDispatcher& dispatcher,
        CpuRegistry& cpus,
        const usize (&arguments)[3],
        bool async,
        usize cookie) noexcept
        -> libk::Expected<CallResult, EndpointError>;
    [[nodiscard]] auto enter(
        Vproc& caller,
        operation::Key operation,
        arch::TrapContext& trap,
        sched::CpuDispatcher& dispatcher) noexcept
        -> libk::Expected<void, EndpointError>;
    [[nodiscard]] auto reply(
        execution::Target caller,
        arch::TrapContext& trap,
        sched::CpuDispatcher& dispatcher,
        isize status,
        usize value) noexcept -> libk::Expected<void, EndpointError>;
    // Called only from the common user trap-exit boundary. Returns true after
    // the current Vproc Endpoint chain was detached and the trap return was
    // redirected to its base runtime.
    [[nodiscard]] auto park(
        execution::Target caller,
        arch::TrapContext& trap,
        sched::CpuDispatcher& dispatcher) noexcept -> bool;

    void close() noexcept;
    void retire(object::ObjectCleanup&& cleanup) noexcept;
    void bind_sponsor(kernel::resource::Sponsorship& sponsor) noexcept;

private:
    friend class Call;
    friend class Activation;

    enum class State : u8 {
        Constructing,
        Open,
        Draining,
        Closed,
    };

    [[nodiscard]] static auto hold(execution::Target target) noexcept
        -> libk::Expected<execution::TargetHold, EndpointError>;
    [[nodiscard]] auto depth(const Execution& execution) const noexcept
        -> usize;
    [[nodiscard]] auto call_complete(const Call& call) const noexcept -> bool;
    [[nodiscard]] auto read_call(Call& call) noexcept -> operation::Result;
    void release_call(Call& call) noexcept;
    [[nodiscard]] auto cancel_call(Call& call) noexcept -> bool;
    void resume_call(Call& call, arch::TrapContext& trap) noexcept;
    void invalidate_call(Call& call) noexcept;
    void authority_quiesced(Call& call) noexcept;
    void publish_cancel(Call& call) noexcept;
    void publisher_done(Call& call) noexcept;
    void finish_reap(Call& call) noexcept;
    void publish_ready(Call& call) noexcept;
    [[nodiscard]] auto enter(
        Call& call,
        arch::TrapContext& trap,
        sched::CpuDispatcher& dispatcher) noexcept -> bool;
    static void unwind_frame(
        void* owner,
        arch::TrapContext& trap,
        sched::CpuDispatcher& dispatcher,
        isize status) noexcept;
    [[nodiscard]] static auto frame_cancel_pending(
        const void* owner) noexcept -> bool;
    [[nodiscard]] auto finish_active(
        Activation& activation,
        arch::TrapContext& trap,
        sched::CpuDispatcher& dispatcher,
        isize status,
        usize value,
        bool reply) noexcept -> bool;
    [[nodiscard]] auto resume_parked(
        Call& root,
        Vproc& vproc,
        arch::TrapContext& trap,
        sched::CpuDispatcher& dispatcher) noexcept -> bool;
    [[nodiscard]] auto next_call_locked(Activation& slot) noexcept -> Call*;
    void reset_call_locked(Call& call) noexcept;
    void try_finish_retire() noexcept;

    mutable libk::TicketSpinLock lock_{};
    ExecutionBinding service_;
    kernel::mm::UserView code_;
    CodePages resident_code_{};
    EndpointConfig config_{};
    kernel::mm::NodePool<Activation> activations_;
    kernel::mm::NodePool<Call> calls_;
    Activation* slots_[max_activations]{};
    Call* call_slots_[MYOS_ENDPOINT_MAX_CALLS]{};
    object::ObjectCleanup cleanup_{};
    usize slot_count_{};
    usize call_count_{};
    usize outstanding_{};
    u64 generation_{};
    State state_{State::Constructing};
};

} // namespace kernel::ipc
