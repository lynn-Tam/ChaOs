#pragma once

#include <arch/user.hpp>
#include <arch/trap.hpp>
#include <core/types.hpp>
#include <execution/authority.hpp>
#include <execution/execution.hpp>
#include <execution/stop.hpp>
#include <ipc/tunnel_link.hpp>
#include <ipc/notification_link.hpp>
#include <libk/expected.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <pager/claim.hpp>
#include <sync/lock.hpp>
#include <mm/direct_map.hpp>
#include <mm/memory_object.hpp>
#include <mm/reclaim.hpp>
#include <mm/user_view.hpp>
#include <operation/completion.hpp>
#include <operation/key.hpp>
#include <sched/remote_queue.hpp>
#include <uapi/status.h>
#include <uapi/vproc.h>
#include <fault/terminal.hpp>

namespace kernel {

namespace mm {
enum class FaultKind : u8;
}

namespace fault {
class TerminalObservation;
}

class CpuRegistry;
namespace operation {
class Completion;
}
namespace ipc {
class Notification;
class Tunnel;
}
namespace execution {
class Target;
}
namespace sched {
class Binding;
class CpuDispatcher;
class SchedulingContext;
class RemoteQueue;
}

enum class VprocError : u8 {
    InvalidRuntime,
    InvalidState,
    TableFull,
    InvalidKey,
    GenerationExhausted,
};

struct VprocRuntime final : private libk::noncopyable {
    VprocRuntime() noexcept = default;
    VprocRuntime(VprocRuntime&&) noexcept = default;
    auto operator=(VprocRuntime&&) noexcept -> VprocRuntime& = default;

    kernel::mm::UserView control_view{};
    kernel::mm::UserView event_view{};
    kernel::mm::PageLease control_page{};
    kernel::mm::PageLease event_page{};
    myos_vproc_control_page* control{};
    myos_vproc_event_page* events{};
    kernel::mm::VirtAddr control_address{};
    kernel::mm::VirtAddr event_address{};
};

struct VprocArm final : private libk::noncopyable {
    VprocArm() noexcept = default;
    VprocArm(VprocArm&&) noexcept = default;
    auto operator=(VprocArm&&) noexcept -> VprocArm& = default;

    kernel::mm::UserView code_view{};
    kernel::mm::UserView stack_view{};
    kernel::mm::PageLease code_page{};
    kernel::mm::PageLease stack_page{};
    arch::UserStart entry{};
};

/*luna change: give the fixed Vproc fault continuation its sole lifecycle and pin identity, reason: one slot must cover admission, callback and stop races without duplicating PageRequest truth*/
struct FaultSlot final : private libk::noncopyable {
    enum class State : u8 {
        Free,
        Attaching,
        PendingPage,
        PageReady,
        PageFailed,
        Claimed,
        Resuming,
        Dropping,
    };

    mm::WaitRelation page_wait{};
    /*luna change: keep pressure retry payload in the same fixed FaultSlot,
      reason: Vproc has no heap/table lane for a second continuation owner*/
    mm::FrameDemand demand{};
    arch::UserFrame frame{};
    mm::MemoryObject* memory{};
    CpuRegistry* cpus{};
    mm::VirtAddr address{};
    mm::Access access{mm::Access::Read};
    u64 generation{};
    u64 relation_generation{};
    usize pc{};
    u8 kind{};
    u8 publishers{};
    State state{State::Free};

    /*luna change: centralize FaultSlot teardown while preserving lane generation, reason: relation detachment and pin release must precede any slot reuse*/
    void reset() noexcept {
        KASSERT(!page_wait.attached());
        KASSERT(memory == nullptr && publishers == 0);
        KASSERT(!demand);
        /*luna change: forbid reset with a live relation owner, reason: generation zero is the only reusable FaultSlot boundary*/
        KASSERT(relation_generation == 0);
        frame = {};
        cpus = nullptr;
        address = {};
        access = mm::Access::Read;
        relation_generation = 0;
        pc = 0;
        kind = 0;
        state = State::Free;
    }
};

// A kernel-visible execution lane whose user continuations are owned by its
// runtime.  The kernel owns only the lane, bounded operation table and one
// non-reentrant upcall frame; user tasks are never kernel objects.
class Vproc final : private libk::noncopyable_nonmovable {
public:
    using State = ExecutionState;
    static constexpr usize max_operations = MYOS_VPROC_MAX_OPERATIONS;

    Vproc(
        kernel::resource::Charge&& stack_charge,
        KernelStack&& home_stack,
        ExecutionBinding&& binding,
        arch::UserStart runtime_entry,
        VprocRuntime&& runtime) noexcept;
    ~Vproc() noexcept;

    [[nodiscard]] auto state() const noexcept -> State {
        return execution_.state();
    }
    [[nodiscard]] auto binding() noexcept -> sched::Binding* {
        return execution_.scheduler_binding();
    }
    [[nodiscard]] auto binding() const noexcept -> const sched::Binding* {
        return execution_.scheduler_binding();
    }
    [[nodiscard]] auto execution() noexcept -> Execution& { return execution_; }
    [[nodiscard]] auto execution() const noexcept -> const Execution& {
        return execution_;
    }
    [[nodiscard]] auto authorize(
        const cap::Resolved<kernel::mm::VSpace>& vspace,
        const cap::Resolved<cap::CSpace>& cspace,
        const cap::Resolved<kernel::mm::MemoryObject>& control,
        const cap::Resolved<kernel::mm::MemoryObject>& events) noexcept
        -> libk::Expected<void, cap::GrantError>;

    [[nodiscard]] auto begin_operation(
        operation::Completion& completion,
        CpuRegistry& cpus,
        usize cookie) noexcept -> libk::Expected<operation::Key, VprocError>;
    [[nodiscard]] auto poll_operation(operation::Key key) const noexcept
        -> libk::Expected<operation::Result, VprocError>;
    [[nodiscard]] auto cancel_operation(operation::Key key) noexcept
        -> libk::Expected<void, VprocError>;
    [[nodiscard]] auto finish_operation(operation::Key key) noexcept
        -> libk::Expected<operation::Result, VprocError>;
    [[nodiscard]] auto pending_sequence() const noexcept -> u64;
    [[nodiscard]] auto request_park(u64 observed_sequence) noexcept
        -> libk::Expected<void, VprocError>;
    [[nodiscard]] auto arm(
        const cap::Resolved<kernel::mm::MemoryObject>& code,
        const cap::Resolved<kernel::mm::MemoryObject>& stack,
        VprocArm&& registration) noexcept
        -> libk::Expected<void, VprocError>;
    void on_trap_exit(arch::TrapContext& trap) noexcept;
    /*luna change: route Vproc faults through the fixed FaultSlot adapter, reason: Pending preserves the exact frame while runtime entry owns execution*/
    [[nodiscard]] auto fault(
        arch::TrapContext& trap,
        CpuRegistry& cpus,
        CpuId local,
        mm::VirtAddr address,
        mm::Access access) noexcept -> mm::FaultKind;
    /*luna change: expose the sole FaultKey claim/resume/drop edges, reason: runtime control must consume one fixed continuation without duplicating slot state*/
    [[nodiscard]] auto claim_fault(u64 key) noexcept
        -> libk::Expected<mm::FaultKind, VprocError>;
    [[nodiscard]] auto resume_fault(
        arch::TrapContext& trap,
        u64 key) noexcept -> libk::Expected<mm::FaultKind, VprocError>;
    [[nodiscard]] auto drop_fault(u64 key) noexcept
        -> libk::Expected<void, VprocError>;
    [[nodiscard]] auto resume(
        arch::TrapContext& trap,
        u64 generation) noexcept -> libk::Expected<void, VprocError>;
    [[nodiscard]] auto in_upcall() const noexcept -> bool;
    void request_exit() noexcept;
    // Called by the execution syscall path.  The cleanup sequence remains
    // request_exit(), but the eventual terminal winner is NormalExit rather
    // than an externally requested Stop.
    void request_normal_exit(myos_status_t status = MYOS_STATUS_OK) noexcept;
    // Terminal edge: every outstanding Pager service claim registered by this
    // lane returns to Published through the requeue owner path. Idempotent.
    void release_pager_claims() noexcept;
    [[nodiscard]] auto pager_claims() noexcept -> pager::ClaimIndex& {
        return pager_claims_;
    }
    [[nodiscard]] auto prepare_retire() const noexcept -> bool;
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

private:
    friend class operation::Completion;
    friend class sched::Binding;
    friend class sched::SchedulingContext;
    friend class sched::CpuDispatcher;
    friend class sched::RemoteQueue;
    friend class execution::Stop;
    friend class execution::Target;
    friend class ipc::Tunnel;
    friend class ipc::Notification;

    enum class OperationState : u8 {
        Free,
        Pending,
        Ready,
    };

    enum class UpcallState : u8 {
        Unarmed,
        Armed,
        Active,
    };

    enum class ActivationPost : u8 {
        Idle,
        Posting,
        Pending,
        Consumed,
    };

    struct OperationSlot final {
        operation::Completion* completion{};
        usize value{};
        usize cookie{};
        u64 generation{};
        myos_status_t status{MYOS_STATUS_OK};
        OperationState state{OperationState::Free};
    };

    struct IngressSlot final {
        ipc::TunnelLink* link{};
        u64 binding_generation{};
        u64 signal_sequence{};
        usize tag{};
    };

    struct NotificationSlot final {
        ipc::NotificationLink* link{};
        u64 binding_generation{};
        u64 signal_sequence{};
        usize tag{};
    };


    [[noreturn]] static void start(void* argument) noexcept;
    void request_stop(execution::Stop& request) noexcept;
    void finish_terminal(
        fault::Reason reason,
        myos_status_t status) noexcept;
    void finish_stop() noexcept;
    void finish_exit(myos_status_t status = MYOS_STATUS_OK) noexcept;
    void publish_operation(
        operation::Key key,
        operation::Result result,
        CpuRegistry& cpus) noexcept;
    // state_lock_ owner only: clear one exact operation generation and its
    // user event projection before the Completion owner is released.
    void clear_operation_locked(usize index) noexcept;
    void cancel_operations() noexcept;
    [[nodiscard]] auto attach_tunnel_source(ipc::TunnelLink& link) noexcept
        -> bool;
    [[nodiscard]] auto attach_tunnel_target(
        ipc::TunnelLink& link,
        usize slot,
        usize tag) noexcept -> libk::optional<u64>;
    void detach_tunnel_source(ipc::TunnelLink& link) noexcept;
    void detach_tunnel_target(
        ipc::TunnelLink& link,
        usize slot,
        u64 binding_generation) noexcept;
    void publish_tunnel(
        ipc::TunnelLink& link,
        usize slot,
        u64 binding_generation,
        u64 signal_sequence,
        usize tag,
        CpuRegistry& cpus) noexcept;
    void clear_tunnel(
        ipc::TunnelLink& link,
        usize slot,
        u64 binding_generation,
        u64 signal_sequence) noexcept;
    void close_tunnels() noexcept;
    [[nodiscard]] auto attach_notification(
        ipc::NotificationLink& link,
        usize slot,
        usize tag) noexcept -> libk::optional<u64>;
    void detach_notification(
        ipc::NotificationLink& link,
        usize slot,
        u64 binding_generation) noexcept;
    void publish_notification(
        ipc::NotificationLink& link,
        usize slot,
        u64 binding_generation,
        u64 signal_sequence,
        usize tag,
        CpuRegistry& cpus) noexcept;
    void clear_notification(
        ipc::NotificationLink& link,
        usize slot,
        u64 binding_generation,
        u64 signal_sequence) noexcept;
    void close_notifications() noexcept;
    /*luna change: serialize runtime entry and optional frame capture in one owner lock, reason: pending delivery and Armed->Active must share one linearization*/
    [[nodiscard]] auto enter_runtime(
        arch::TrapContext& trap,
        bool capture) noexcept -> bool;
    /*luna change: deliver finalized page outcomes through Vproc state ownership, reason: callback lifetime and event projection must share one generation check*/
    static void publish_fault(void* owner, mm::PageWaitResult result) noexcept;
    void publish_fault(mm::PageWaitResult result) noexcept;
    void project_fault_locked() noexcept;
    /*luna change: close an admitted fault on stop without holding Vproc lock across MemoryObject operations, reason: foreign cancellation must race callback by state and publisher count*/
    void close_fault() noexcept;
    [[nodiscard]] auto pending_operations() const noexcept -> bool;
    [[nodiscard]] auto pending_events() const noexcept -> bool;
    [[nodiscard]] auto activation_quiescent() const noexcept -> bool;
    void activation_publisher_done() noexcept;
    [[nodiscard]] auto activation_request_posted(bool posted) noexcept -> bool;
    [[nodiscard]] auto activation_request_consumed() noexcept -> bool;
    void retry_stop_if_ready() noexcept;

    using StopList = libk::IntrusiveList<
        execution::Stop, &execution::Stop::hook_>;

    Execution execution_;
    execution::Authority authority_;
    fault::TerminalRecord terminal_{};
    fault::TerminalObservation* terminal_observation_{};
    arch::UserStart bootstrap_entry_{};
    VprocRuntime runtime_{};
    VprocArm arm_{};
    FaultSlot fault_slot_{};
    pager::ClaimIndex pager_claims_{};
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::Vproc>
        state_lock_{};
    OperationSlot operations_[max_operations]{};
    using TunnelLinks = libk::IntrusiveList<
        ipc::TunnelLink, &ipc::TunnelLink::hook>;
    TunnelLinks outgoing_tunnels_{};
    IngressSlot ingresses_[MYOS_VPROC_MAX_INGRESS]{};
    NotificationSlot notifications_[MYOS_VPROC_MAX_NOTIFICATIONS]{};
    StopList stops_{};
    u64 pending_sequence_{};
    u64 ready_mask_{};
    u64 ingress_mask_{};
    u64 notification_mask_{};
    u64 upcall_generation_{};
    u64 park_sequence_{};
    UpcallState upcall_state_{UpcallState::Unarmed};
    bool arm_attaching_{};
    bool stop_requested_{};
    bool stop_dispatched_{};
    bool stopped_{};
    bool normal_exit_requested_{};
    myos_status_t normal_exit_status_{MYOS_STATUS_OK};
    bool park_requested_{};
    usize activation_publishers_{};
    ActivationPost activation_post_{ActivationPost::Idle};
    bool activation_dirty_{};
    mutable bool relation_admission_closed_{};
    sched::RemoteRequest activation_{sched::RemoteKind::Activation, this};
};

} // namespace kernel
