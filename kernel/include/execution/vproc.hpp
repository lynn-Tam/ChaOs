#pragma once

#include <arch/user.hpp>
#include <core/types.hpp>
#include <execution/authority.hpp>
#include <execution/execution.hpp>
#include <execution/stop.hpp>
#include <ipc/tunnel_link.hpp>
#include <ipc/notification_link.hpp>
#include <libk/expected.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <mm/direct_map.hpp>
#include <mm/memory_object.hpp>
#include <mm/user_view.hpp>
#include <operation/completion.hpp>
#include <operation/key.hpp>
#include <sched/remote_queue.hpp>
#include <uapi/status.h>
#include <uapi/vproc.h>

namespace kernel {

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
    [[nodiscard]] auto resume(
        arch::TrapContext& trap,
        u64 generation) noexcept -> libk::Expected<void, VprocError>;
    [[nodiscard]] auto in_upcall() const noexcept -> bool;
    void request_exit() noexcept;
    [[nodiscard]] auto prepare_retire() const noexcept -> bool;

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
        myos_status_t status{MYOS_STATUS_OK};
        usize value{};
        usize cookie{};
        u64 generation{};
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
    void finish_stop() noexcept;
    void publish_operation(
        operation::Key key,
        operation::Result result,
        CpuRegistry& cpus) noexcept;
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
    arch::UserStart bootstrap_entry_{};
    VprocRuntime runtime_{};
    VprocArm arm_{};
    mutable libk::TicketSpinLock state_lock_{};
    OperationSlot operations_[max_operations]{};
    using TunnelLinks = libk::IntrusiveList<
        ipc::TunnelLink, &ipc::TunnelLink::hook>;
    TunnelLinks outgoing_tunnels_{};
    IngressSlot ingresses_[MYOS_VPROC_MAX_INGRESS]{};
    NotificationSlot notifications_[MYOS_VPROC_MAX_NOTIFICATIONS]{};
    mutable bool relation_admission_closed_{};
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
    bool park_requested_{};
    usize activation_publishers_{};
    ActivationPost activation_post_{ActivationPost::Idle};
    bool activation_dirty_{};
    sched::RemoteRequest activation_{sched::RemoteKind::Activation, this};
};

} // namespace kernel
