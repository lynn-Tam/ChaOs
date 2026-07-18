#pragma once

#include <arch/user.hpp>
#include <core/types.hpp>
#include <execution/authority.hpp>
#include <execution/execution.hpp>
#include <execution/stop.hpp>
#include <ipc/tunnel_link.hpp>
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
    [[nodiscard]] auto take_operation(operation::Key key) noexcept
        -> libk::Expected<operation::Result, VprocError>;
    [[nodiscard]] auto pending_sequence() const noexcept -> u64;
    void on_trap_exit(arch::TrapContext& trap) noexcept;
    [[nodiscard]] auto resume(
        arch::TrapContext& trap,
        u64 generation) noexcept -> libk::Expected<void, VprocError>;
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

    enum class OperationState : u8 {
        Free,
        Pending,
        Ready,
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
        u64 delivery_generation{};
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
        u64 delivery_generation,
        usize tag,
        CpuRegistry& cpus) noexcept;
    void clear_tunnel(
        ipc::TunnelLink& link,
        usize slot,
        u64 binding_generation,
        u64 delivery_generation) noexcept;
    void close_tunnels() noexcept;
    [[nodiscard]] auto pending_operations() const noexcept -> bool;
    [[nodiscard]] auto pending_events() const noexcept -> bool;

    using StopList = libk::IntrusiveList<
        execution::Stop, &execution::Stop::hook_>;

    Execution execution_;
    execution::Authority authority_;
    arch::UserStart runtime_entry_{};
    VprocRuntime runtime_{};
    mutable libk::TicketSpinLock operation_lock_{};
    OperationSlot operations_[max_operations]{};
    using TunnelLinks = libk::IntrusiveList<
        ipc::TunnelLink, &ipc::TunnelLink::hook>;
    mutable libk::TicketSpinLock tunnel_lock_{};
    TunnelLinks outgoing_tunnels_{};
    IngressSlot ingresses_[MYOS_VPROC_MAX_INGRESS]{};
    mutable bool tunnel_admission_closed_{};
    mutable libk::TicketSpinLock stop_lock_{};
    StopList stops_{};
    u64 pending_sequence_{};
    u64 ready_mask_{};
    u64 ingress_mask_{};
    u64 upcall_generation_{};
    bool upcall_active_{};
    bool stop_requested_{};
    bool stopped_{};
    sched::RemoteRequest activation_{sched::RemoteKind::Activation, this};
};

} // namespace kernel
