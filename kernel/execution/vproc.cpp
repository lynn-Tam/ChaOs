#include <execution/vproc.hpp>

#include <fault/observation.hpp>
#include <pager/pager.hpp>
#include <arch/cpu.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <libk/limits.hpp>
#include <libk/utility.hpp>
#include <mm/vspace.hpp>
#include <operation/completion.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel {

auto Vproc::observe_terminal(
    ipc::Notification& notification,
    u64 badge) noexcept -> bool {
    if (terminal_observation_ != nullptr) {
        return false;
    }
    auto* const observation = fault::allocate_observation();
    if (observation == nullptr
        || !observation->bind(terminal_, notification, badge)) {
        if (observation != nullptr) {
            fault::release_observation(*observation);
        }
        return false;
    }
    terminal_observation_ = observation;
    return true;
}

void Vproc::clear_terminal_observation() noexcept {
    auto* const observation = libk::exchange(terminal_observation_, nullptr);
    if (observation != nullptr) {
        observation->reset();
        fault::release_observation(*observation);
    }
}

namespace {

[[nodiscard]] auto valid_runtime(const VprocRuntime& runtime) noexcept -> bool {
    const auto control_access = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Write);
    const auto event_access = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read);
    return runtime.control != nullptr && runtime.events != nullptr
        && runtime.control_page && runtime.event_page
        && runtime.control_view.valid() && runtime.event_view.valid()
        && runtime.control_view.access() == control_access
        && runtime.event_view.access() == event_access
        && runtime.control_view.virtual_range().base()
            == runtime.control_address
        && runtime.event_view.virtual_range().base() == runtime.event_address
        && runtime.control_view.virtual_range().size() == kernel::mm::page_size
        && runtime.event_view.virtual_range().size() == kernel::mm::page_size
        && runtime.control_page.page().access.contains(control_access)
        && runtime.event_page.page().access.contains(
            kernel::mm::Access::Write);
}

} // namespace

Vproc::Vproc(
    kernel::resource::Charge&& stack_charge,
    KernelStack&& home_stack,
    ExecutionBinding&& binding,
    arch::UserStart runtime_entry,
    VprocRuntime&& runtime) noexcept
    : execution_(
          libk::move(stack_charge),
          libk::move(home_stack),
          libk::move(binding)),
      authority_(*this),
      bootstrap_entry_(runtime_entry),
      runtime_(libk::move(runtime)) {
    KASSERT(execution_.binding().user_bound());
    KASSERT(arch::valid_user_start(bootstrap_entry_));
    KASSERT(valid_runtime(runtime_));

    *runtime_.control = {};
    runtime_.control->version = MYOS_VPROC_RUNTIME_VERSION;
    *runtime_.events = {};
    runtime_.events->version = MYOS_VPROC_RUNTIME_VERSION;

    /*luna change: build the Vproc bootstrap frame below the reserved cell, reason: startup and user-trap entry share one adjusted active top*/
    const usize active_stack_top = arch::vproc_stack_top(
        execution_.stack_top());
    KASSERT(active_stack_top != 0
        && active_stack_top > execution_.stack_base());
    const auto kernel_stack_top = arch::prepare_user_stack(
        active_stack_top, bootstrap_entry_);
    KASSERT(kernel_stack_top);
    KASSERT(*kernel_stack_top >= execution_.stack_base());
    execution_.prepare(&Vproc::start, this, *kernel_stack_top);
}

Vproc::~Vproc() noexcept {
    clear_terminal_observation();
    KASSERT(execution_.state_ != State::Running);
    KASSERT(execution_.scheduler_binding_ == nullptr);
    KASSERT(execution_.home_ == nullptr);
    KASSERT(stops_.empty());
    KASSERT(outgoing_tunnels_.empty());
    KASSERT(!arm_attaching_ && activation_publishers_ == 0
        && activation_post_ == ActivationPost::Idle && !activation_dirty_);
    /*luna change: require the fault continuation to be fully settled before destruction, reason: ObjectPool reuse cannot race a retained frame or callback publisher*/
    KASSERT(fault_slot_.state == FaultSlot::State::Free
        && fault_slot_.publishers == 0);
    for (const IngressSlot& ingress : ingresses_) {
        KASSERT(ingress.link == nullptr);
    }
    for (const NotificationSlot& notification : notifications_) {
        KASSERT(notification.link == nullptr);
    }
    for (const OperationSlot& slot : operations_) {
        KASSERT(slot.state == OperationState::Free
            && slot.completion == nullptr);
    }
}

auto Vproc::authorize(
    const cap::Resolved<kernel::mm::VSpace>& vspace,
    const cap::Resolved<cap::CSpace>& cspace,
    const cap::Resolved<kernel::mm::MemoryObject>& control,
    const cap::Resolved<kernel::mm::MemoryObject>& events) noexcept
    -> libk::Expected<void, cap::GrantError> {
    if (execution_.state_ != State::Prepared
        || execution_.scheduler_binding_ != nullptr
        || !execution_.binding().user_bound()) {
        return libk::unexpected(cap::GrantError::InvalidState);
    }
    auto attached = authority_.attach(vspace, cspace);
    if (!attached) {
        return attached;
    }
    attached = authority_.attach_runtime(control, events);
    if (!attached) {
        authority_.target_stopped();
    }
    return attached;
}

auto Vproc::begin_operation(
    operation::Completion& completion,
    CpuRegistry& cpus,
    usize cookie) noexcept -> libk::Expected<operation::Key, VprocError> {
    kernel::sync::IrqLockGuard guard{state_lock_};
    if (stop_requested_ || stopped_ || execution_.state_ == State::Exited
        || upcall_state_ != UpcallState::Armed) {
        return libk::unexpected(VprocError::InvalidState);
    }
    for (usize index = 0; index < max_operations; ++index) {
        OperationSlot& slot = operations_[index];
        if (slot.state != OperationState::Free) {
            continue;
        }
        const u64 maximum = libk::numeric_limits<u64>::max()
            >> MYOS_OPERATION_SLOT_BITS;
        if (slot.generation == maximum) {
            continue;
        }
        ++slot.generation;
        KASSERT(slot.generation != 0);
        const operation::Key key{
            (slot.generation << MYOS_OPERATION_SLOT_BITS) | index};
        slot.completion = &completion;
        slot.status = MYOS_STATUS_PENDING;
        slot.value = 0;
        slot.cookie = cookie;
        slot.state = OperationState::Pending;
        completion.attach(*this, cpus, key);
        return libk::expected(key);
    }
    return libk::unexpected(VprocError::TableFull);
}

void Vproc::clear_operation_locked(usize index) noexcept {
    KASSERT(index < max_operations);
    OperationSlot& slot = operations_[index];
    slot.completion = nullptr;
    slot.status = MYOS_STATUS_OK;
    slot.value = 0;
    slot.cookie = 0;
    slot.state = OperationState::Free;
    const u64 bit = u64{1} << index;
    ready_mask_ &= ~bit;
    __atomic_fetch_and(
        &runtime_.events->ready_mask, ~bit, __ATOMIC_RELEASE);
    __atomic_store_n(
        &runtime_.events->operation_key[index], 0, __ATOMIC_RELEASE);
    __atomic_store_n(
        &runtime_.events->operation_cookie[index], 0, __ATOMIC_RELEASE);
}

void Vproc::publish_operation(
    operation::Key key,
    operation::Result result,
    CpuRegistry& cpus) noexcept {
    bool pending{};
    bool activate{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        KASSERT(key.slot() < max_operations);
        OperationSlot& slot = operations_[key.slot()];
        KASSERT(slot.state == OperationState::Pending
            && slot.generation == key.generation());
        slot.completion = nullptr;
        slot.status = result.status;
        slot.value = result.value;
        slot.state = OperationState::Ready;
        ++pending_sequence_;
        KASSERT(pending_sequence_ != 0);
        ready_mask_ |= u64{1} << key.slot();
        __atomic_store_n(
            &runtime_.events->operation_key[key.slot()],
            key.raw,
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &runtime_.events->operation_cookie[key.slot()],
            slot.cookie,
            __ATOMIC_RELEASE);
        __atomic_fetch_or(
            &runtime_.events->ready_mask,
            u64{1} << key.slot(),
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &runtime_.events->pending_sequence,
            pending_sequence_,
            __ATOMIC_RELEASE);
        activate = !stop_requested_ && !stopped_;
        for (const OperationSlot& current : operations_) {
            pending = pending || current.state == OperationState::Pending;
        }
    }
    if (!activate && !pending) {
        retry_stop_if_ready();
        return;
    }
    if (activate) {
        // The operation table is canonical. Activation is only a retained
        // edge that asks the home dispatcher to establish a safe boundary.
        static_cast<void>(sched::activate(cpus, *this));
    }
}

void Vproc::cancel_operations() noexcept {
    for (usize index = 0; index < max_operations; ++index) {
        operation::Completion* completion{};
        operation::Key key{};
        {
            kernel::sync::IrqLockGuard guard{state_lock_};
            OperationSlot& slot = operations_[index];
            if (slot.state != OperationState::Pending) {
                continue;
            }
            KASSERT(slot.completion != nullptr);
            completion = slot.completion;
            key = operation::Key{
                (slot.generation << MYOS_OPERATION_SLOT_BITS) | index};
            // Claim Delivery while the slot pointer and generation are
            // stable.  A producer that already owns publication leaves this
            // operation for its normal Ready path.
            if (!completion->try_claim_cancel()) {
                continue;
            }
        }

        operation::Completion::CancelResult resolution =
            completion->resolve_cancel();
        if (resolution == operation::Completion::CancelResult::Reopen) {
            bool reopened{};
            {
                kernel::sync::IrqLockGuard guard{state_lock_};
                OperationSlot& slot = operations_[index];
                KASSERT(slot.state == OperationState::Pending
                    && slot.generation == key.generation()
                    && slot.completion == completion);
                // The Pending slot is the complete Vproc edge projection;
                // restore it before publishing Delivery::Attached.
                reopened = completion->try_reopen_cancel();
            }
            if (reopened) {
                continue;
            }
            // CancelRaced is the producer's durable handoff.  Cancellation
            // remains the terminal owner and drains the completed result.
            resolution = operation::Completion::CancelResult::Completed;
        }

        {
            kernel::sync::IrqLockGuard guard{state_lock_};
            OperationSlot& slot = operations_[index];
            KASSERT(slot.state == OperationState::Pending
                && slot.generation == key.generation()
                && slot.completion == completion);
            clear_operation_locked(index);
        }
        completion->finalize_cancel(resolution);
    }
}

auto Vproc::pending_operations() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{state_lock_};
    for (const OperationSlot& slot : operations_) {
        if (slot.state == OperationState::Pending) {
            return true;
        }
    }
    return false;
}

auto Vproc::poll_operation(operation::Key key) const noexcept
    -> libk::Expected<operation::Result, VprocError> {
    kernel::sync::IrqLockGuard guard{state_lock_};
    if (!key.valid() || key.slot() >= max_operations) {
        return libk::unexpected(VprocError::InvalidKey);
    }
    const OperationSlot& slot = operations_[key.slot()];
    if (slot.generation != key.generation()
        || slot.state == OperationState::Free) {
        return libk::unexpected(VprocError::InvalidKey);
    }
    return slot.state == OperationState::Ready
        ? libk::Expected<operation::Result, VprocError>{
              libk::expected(operation::Result{slot.status, slot.value})}
        : libk::Expected<operation::Result, VprocError>{
              libk::expected(operation::Result{MYOS_STATUS_PENDING, 0})};
}

auto Vproc::cancel_operation(operation::Key key) noexcept
    -> libk::Expected<void, VprocError> {
    operation::Completion* completion{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        if (!key.valid() || key.slot() >= max_operations) {
            return libk::unexpected(VprocError::InvalidKey);
        }
        OperationSlot& slot = operations_[key.slot()];
        if (slot.generation != key.generation()
            || slot.state != OperationState::Pending
            || slot.completion == nullptr) {
            return libk::unexpected(VprocError::InvalidState);
        }
        completion = slot.completion;
        if (!completion->try_claim_cancel()) {
            return libk::unexpected(VprocError::InvalidState);
        }
    }

    operation::Completion::CancelResult resolution =
        completion->resolve_cancel();
    if (resolution == operation::Completion::CancelResult::Reopen) {
        kernel::sync::IrqLockGuard guard{state_lock_};
        OperationSlot& slot = operations_[key.slot()];
        KASSERT(slot.generation == key.generation()
            && slot.state == OperationState::Pending
            && slot.completion == completion);
        if (completion->try_reopen_cancel()) {
            return libk::unexpected(VprocError::InvalidState);
        }
        // The only legal CAS loser is CancelRaced.  Keep the exact Pending
        // edge and let this cancellation owner drain the completed result.
        resolution = operation::Completion::CancelResult::Completed;
    }
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        OperationSlot& slot = operations_[key.slot()];
        KASSERT(slot.generation == key.generation()
            && slot.state == OperationState::Pending
            && slot.completion == completion);
        clear_operation_locked(key.slot());
    }
    completion->finalize_cancel(resolution);
    retry_stop_if_ready();
    return libk::expected();
}

auto Vproc::finish_operation(operation::Key key) noexcept
    -> libk::Expected<operation::Result, VprocError> {
    kernel::sync::IrqLockGuard guard{state_lock_};
    if (!key.valid() || key.slot() >= max_operations) {
        return libk::unexpected(VprocError::InvalidKey);
    }
    OperationSlot& slot = operations_[key.slot()];
    if (slot.generation != key.generation()
        || slot.state != OperationState::Ready) {
        return libk::unexpected(VprocError::InvalidKey);
    }
    const operation::Result result{slot.status, slot.value};
    clear_operation_locked(key.slot());
    return libk::expected(result);
}

auto Vproc::pending_sequence() const noexcept -> u64 {
    kernel::sync::IrqLockGuard guard{state_lock_};
    return pending_sequence_;
}

auto Vproc::request_park(u64 observed_sequence) noexcept
    -> libk::Expected<void, VprocError> {
    kernel::sync::IrqLockGuard guard{state_lock_};
    if (stop_requested_ || stopped_ || park_requested_
        || execution_.state_ != State::Running
        || upcall_state_ != UpcallState::Armed
        || observed_sequence != pending_sequence_
        || ready_mask_ != 0 || ingress_mask_ != 0
        || notification_mask_ != 0
        /*luna change: keep ready fault projection in canonical park admission, reason: event-page fields are only a view of FaultSlot state*/
        || fault_slot_.state == FaultSlot::State::PageReady
        || fault_slot_.state == FaultSlot::State::PageFailed) {
        return libk::unexpected(VprocError::InvalidState);
    }
    park_sequence_ = observed_sequence;
    park_requested_ = true;
    return libk::expected();
}

auto Vproc::pending_events() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{state_lock_};
    /*luna change: derive pending events from PageReady/PageFailed state, reason: runtime projection must not become a second wake truth*/
    return ready_mask_ != 0 || ingress_mask_ != 0
        || notification_mask_ != 0
        || fault_slot_.state == FaultSlot::State::PageReady
        || fault_slot_.state == FaultSlot::State::PageFailed;
}

auto Vproc::arm(
    const cap::Resolved<kernel::mm::MemoryObject>& code,
    const cap::Resolved<kernel::mm::MemoryObject>& stack,
    VprocArm&& registration) noexcept
    -> libk::Expected<void, VprocError> {
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        if (stop_requested_ || stopped_
            || upcall_state_ != UpcallState::Unarmed || arm_attaching_) {
            return libk::unexpected(VprocError::InvalidState);
        }
        arm_attaching_ = true;
    }
    auto attached = authority_.attach_arm(code, stack);
    bool committed{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        KASSERT(arm_attaching_);
        if (attached && !stop_requested_ && !stopped_
            && upcall_state_ == UpcallState::Unarmed) {
            arm_ = libk::move(registration);
            upcall_state_ = UpcallState::Armed;
            committed = true;
        }
        arm_attaching_ = false;
    }
    if (attached && !committed) {
        authority_.detach_arm();
    }
    retry_stop_if_ready();
    if (!committed) {
        return libk::unexpected(VprocError::InvalidState);
    }
    return libk::expected();
}

auto Vproc::enter_runtime(
    arch::TrapContext& trap,
    bool capture) noexcept -> bool {
    const myos_word_t disabled = __atomic_load_n(
        &runtime_.control->upcall_disable_depth, __ATOMIC_ACQUIRE);
    if (disabled != 0) {
        return false;
    }
    arch::UserStart entry{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        if (upcall_state_ != UpcallState::Armed || stop_requested_ || stopped_
            || (capture
                && ready_mask_ == 0 && ingress_mask_ == 0
                && notification_mask_ == 0
                && fault_slot_.state != FaultSlot::State::PageReady
                && fault_slot_.state != FaultSlot::State::PageFailed)) {
            return false;
        }
        if (capture) {
            /*luna change: capture only the ordinary return frame under the entry lock, reason: a stop winner must not invalidate a prechecked Armed state*/
            trap.save_user(runtime_.events->delivered);
        }
        ++upcall_generation_;
        KASSERT(upcall_generation_ != 0);
        upcall_state_ = UpcallState::Active;
        __atomic_store_n(
            &runtime_.events->active_generation,
            upcall_generation_,
            __ATOMIC_RELEASE);
        entry = arm_.entry;
        entry.arguments[0] = upcall_generation_;
        entry.arguments[1] = runtime_.event_address.raw();
        entry.arguments[2] = runtime_.control_address.raw();
        entry.arguments[3] = pending_sequence_;
    }
    KASSERT(trap.load_user_start(entry));
    return true;
}

void Vproc::on_trap_exit(arch::TrapContext& trap) noexcept {
    /*luna change: let the shared entry operation decide the losing race, reason: ordinary delivery must not precheck state before Armed->Active claim*/
    static_cast<void>(enter_runtime(trap, true));
}

/*luna change: publish a finalized fault projection from canonical FaultSlot state, reason: the event page is a one-way ABI view and never admission truth*/
void Vproc::project_fault_locked() noexcept {
    KASSERT(fault_slot_.state == FaultSlot::State::PageReady
        || fault_slot_.state == FaultSlot::State::PageFailed);
    const auto kind = static_cast<mm::FaultKind>(fault_slot_.kind);
    u32 public_kind = MYOS_VPROC_FAULT_KIND_NONE;
    switch (kind) {
    case mm::FaultKind::NoMapping:
        public_kind = MYOS_VPROC_FAULT_KIND_NO_MAPPING;
        break;
    case mm::FaultKind::Guard:
        public_kind = MYOS_VPROC_FAULT_KIND_GUARD;
        break;
    case mm::FaultKind::AccessDenied:
        public_kind = MYOS_VPROC_FAULT_KIND_ACCESS_DENIED;
        break;
    case mm::FaultKind::ResourceExhausted:
        public_kind = MYOS_VPROC_FAULT_KIND_RESOURCE_EXHAUSTED;
        break;
    case mm::FaultKind::OutOfMemory:
        public_kind = MYOS_VPROC_FAULT_KIND_OUT_OF_MEMORY;
        break;
    case mm::FaultKind::BackingFailed:
        public_kind = MYOS_VPROC_FAULT_KIND_BACKING_FAILED;
        break;
    case mm::FaultKind::Ready:
    case mm::FaultKind::Materialized:
        public_kind = MYOS_VPROC_FAULT_KIND_PAGE_READY;
        break;
    case mm::FaultKind::Busy:
    case mm::FaultKind::Pressure:
    case mm::FaultKind::Pending:
        KASSERT(false);
        break;
    }
    u32 public_access = MYOS_VPROC_FAULT_ACCESS_NONE;
    switch (fault_slot_.access) {
    case mm::Access::Read:
        public_access = MYOS_VPROC_FAULT_ACCESS_READ;
        break;
    case mm::Access::Write:
        public_access = MYOS_VPROC_FAULT_ACCESS_WRITE;
        break;
    case mm::Access::Execute:
        public_access = MYOS_VPROC_FAULT_ACCESS_EXECUTE;
        break;
    }
    /*luna change: publish fault payload before releasing its key and sequence, reason: readers must never observe a durable identity for half-written facts*/
    __atomic_store_n(
        &runtime_.events->fault_kind, public_kind, __ATOMIC_RELAXED);
    __atomic_store_n(
        &runtime_.events->fault_access, public_access, __ATOMIC_RELAXED);
    __atomic_store_n(
        &runtime_.events->fault_address,
        fault_slot_.address.raw(),
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &runtime_.events->fault_pc,
        fault_slot_.pc,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &runtime_.events->fault_key,
        static_cast<myos_fault_key_t>(fault_slot_.generation),
        __ATOMIC_RELEASE);
    ++pending_sequence_;
    KASSERT(pending_sequence_ != 0);
    __atomic_store_n(
        &runtime_.events->pending_sequence,
        pending_sequence_,
        __ATOMIC_RELEASE);
}

void Vproc::publish_fault(
    void* owner,
    mm::PageWaitResult result) noexcept {
    KASSERT(owner != nullptr);
    static_cast<Vproc*>(owner)->publish_fault(result);
}

void Vproc::publish_fault(mm::PageWaitResult result) noexcept {
    CpuRegistry* cpus{};
    bool activate{};
    bool claimed{};
    bool callback_owner{};
    bool relation_owner{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        FaultSlot& slot = fault_slot_;
        const FaultSlot::State before = slot.state;
        /*luna change: reject callbacks outside the retained admission states, reason: a stale callback must never be mistaken for a new slot generation*/
        KASSERT(before == FaultSlot::State::Attaching
            || before == FaultSlot::State::PendingPage
            || before == FaultSlot::State::Resuming
            || before == FaultSlot::State::Dropping);
        /*luna change: hold a callback publisher before inspecting the slot, reason: a finalized foreign callback must not enter after Free can be reused*/
        KASSERT(slot.publishers != libk::numeric_limits<u8>::max());
        ++slot.publishers;
        callback_owner = true;
        relation_owner = before == FaultSlot::State::PendingPage
            || (before == FaultSlot::State::Dropping
                && slot.relation_generation != 0);
        if (before == FaultSlot::State::Dropping) {
            /*luna change: identify the Dropping relation owner by generation, reason: close_fault now owns and clears the sole MemoryObject pin before callback entry*/
            KASSERT(slot.relation_generation != 0
                && slot.page_wait.generation == slot.relation_generation);
        } else {
            const u64 relation_generation = slot.page_wait.generation;
            /*luna change: keep the generation zero until relation-publisher handoff, reason: early Attaching/Resuming callbacks have no retained relation owner*/
            KASSERT(before == FaultSlot::State::PendingPage
                ? relation_generation != 0
                    && slot.relation_generation == relation_generation
                : slot.relation_generation == 0);
            /*luna change: turn a pressure wake into the existing retry-ready
              projection, reason: the ABI exposes no pressure event state and
              resume_fault revalidates the canonical VSpace relation*/
            slot.kind = static_cast<u8>(
                result == mm::PageWaitResult::OutOfMemory
                    ? mm::FaultKind::OutOfMemory
                    : result == mm::PageWaitResult::Ready
                        ? mm::FaultKind::Ready
                        : mm::FaultKind::BackingFailed);
            slot.state = result == mm::PageWaitResult::Ready
                ? FaultSlot::State::PageReady
                : FaultSlot::State::PageFailed;
            claimed = true;
            if (before == FaultSlot::State::PendingPage) {
                project_fault_locked();
                cpus = slot.cpus;
                activate = !stop_requested_ && !stopped_;
            }
        }
    }
    if (claimed && activate && cpus != nullptr) {
        static_cast<void>(sched::activate(*cpus, *this));
    }
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        FaultSlot& slot = fault_slot_;
        /*luna change: retire callback and relation publishers only after foreign work returns, reason: exactly one winner releases the retained pin and enables reset*/
        if (callback_owner) {
            KASSERT(slot.publishers != 0);
            --slot.publishers;
        }
        if (relation_owner) {
            KASSERT(slot.publishers != 0);
            /*luna change: retire relation ownership with its generation, reason: nonzero relation_generation is the sole publisher ownership fact*/
            slot.relation_generation = 0;
            --slot.publishers;
        }
        if (slot.state == FaultSlot::State::Dropping
            && slot.publishers == 0) {
            slot.reset();
        }
    }
    retry_stop_if_ready();
}

/*luna change: use the shared mm fault classifier, reason: Vproc and PageFault must preserve identical resource outcomes*/
auto Vproc::fault(
    arch::TrapContext& trap,
    CpuRegistry& cpus,
    CpuId local,
    mm::VirtAddr address,
    mm::Access access) noexcept -> mm::FaultKind {
    mm::VSpace* const vspace = execution_.binding().vspace();
    FaultSlot* slot = &fault_slot_;
    bool sync{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        if (vspace == nullptr || stop_requested_ || stopped_
            || relation_admission_closed_) {
            return mm::FaultKind::BackingFailed;
        }
        /*luna change: resolve control-context faults synchronously without FaultSlot ownership, reason: only Armed admits a managed continuation while Unarmed and Active cannot poll relation-free Pending*/
        if (upcall_state_ == UpcallState::Unarmed
            || upcall_state_ == UpcallState::Active) {
            sync = true;
        } else if (upcall_state_ != UpcallState::Armed
            || slot->state != FaultSlot::State::Free
            || slot->generation == libk::numeric_limits<u64>::max()) {
            return mm::FaultKind::BackingFailed;
        }
        if (!sync) {
            const usize raw_top = execution_.stack_top();
            const usize active_top = arch::vproc_stack_top(raw_top);
            const usize frame_bytes = raw_top >= active_top
                ? raw_top - active_top : 0;
            if (active_top == 0 || active_top <= execution_.stack_base()
                || active_top - execution_.stack_base() < frame_bytes) {
                return mm::FaultKind::BackingFailed;
            }
            const arch::UserFrame frame = trap.save_frame(raw_top);
            if (!frame) {
                return mm::FaultKind::BackingFailed;
            }
            ++slot->generation;
            KASSERT(slot->generation != 0);
            slot->frame = frame;
            slot->memory = nullptr;
            slot->cpus = &cpus;
            slot->address = address;
            slot->access = access;
            slot->pc = trap.pc();
            slot->kind = static_cast<u8>(mm::FaultKind::Pending);
            slot->relation_generation = 0;
            slot->publishers = 1;
            slot->state = FaultSlot::State::Attaching;
        }
    }

    if (sync) {
        const auto result = vspace->fault(
            mm::VmContext{.cpus = &cpus, .local = local},
            address,
            access);
        if (!result) {
            return mm::fault_kind(result.error());
        }
        return result.value().kind == mm::FaultKind::Pending
            ? mm::FaultKind::BackingFailed
            : result.value().kind;
    }

    const auto result = vspace->fault(
        mm::VmContext{.cpus = &cpus, .local = local},
        address,
        access,
        &slot->page_wait,
        this,
        &Vproc::publish_fault,
        &slot->demand);
    mm::FaultKind outcome = result
        ? result.value().kind : mm::fault_kind(result.error());
    bool enter{};
    bool stop_fault{};
    mm::MemoryObject* drop_memory{};
    if (result && (result.value().kind == mm::FaultKind::Pending
                   || result.value().kind == mm::FaultKind::Pressure)) {
        KASSERT(result.value().memory != nullptr);
        {
            kernel::sync::IrqLockGuard guard{state_lock_};
            FaultSlot& current = fault_slot_;
            const bool stopping = stop_requested_ || stopped_
                || relation_admission_closed_;
            current.memory = result.value().memory;
            if (current.state == FaultSlot::State::Attaching) {
                /*luna change: consume the backing-owned relation handoff for
                  Pending or Pressure, reason: materialize_impl already
                  attached durable pressure before returning*/
                if (result.value().kind == mm::FaultKind::Pressure) {
                    current.kind = static_cast<u8>(mm::FaultKind::Pressure);
                }
                KASSERT(current.page_wait.generation != 0);
                current.relation_generation = current.page_wait.generation;
                current.state = FaultSlot::State::PendingPage;
                KASSERT(current.publishers
                    != libk::numeric_limits<u8>::max());
                ++current.publishers;
                stop_fault = stopping;
                enter = !stopping;
            } else if (current.state == FaultSlot::State::PageReady
                || current.state == FaultSlot::State::PageFailed) {
                KASSERT(current.relation_generation == 0);
                project_fault_locked();
                enter = true;
            } else {
                KASSERT(current.state == FaultSlot::State::Dropping);
                KASSERT(current.relation_generation == 0);
                /*luna change: settle an early-ready pin after stop won before
                  admission returned, reason: no relation publisher remains
                  once the callback finalized the handoff*/
                drop_memory = current.memory;
                current.memory = nullptr;
                stop_fault = true;
            }
        }
    } else {
        kernel::sync::IrqLockGuard guard{state_lock_};
        FaultSlot& current = fault_slot_;
        KASSERT(current.state == FaultSlot::State::Attaching);
        current.kind = static_cast<u8>(outcome);
        current.state = FaultSlot::State::Dropping;
        KASSERT(current.publishers == 1);
    }
    if (drop_memory != nullptr) {
        drop_memory->release_fault();
    }
    if (stop_fault) {
        close_fault();
        outcome = mm::FaultKind::BackingFailed;
    } else if (enter) {
        if (!enter_runtime(trap, false)) {
            close_fault();
            outcome = mm::FaultKind::BackingFailed;
        }
    }
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        FaultSlot& current = fault_slot_;
        KASSERT(current.publishers != 0);
        --current.publishers;
        if (current.state == FaultSlot::State::Dropping
            && current.publishers == 0) {
            current.reset();
        }
    }
    retry_stop_if_ready();
    return outcome;
}

auto Vproc::claim_fault(u64 key) noexcept
    -> libk::Expected<mm::FaultKind, VprocError> {
    kernel::sync::IrqLockGuard guard{state_lock_};
    if (upcall_state_ != UpcallState::Active) {
        return libk::unexpected(VprocError::InvalidState);
    }
    if (key == 0 || key != fault_slot_.generation) {
        return libk::unexpected(VprocError::InvalidKey);
    }
    if (fault_slot_.state != FaultSlot::State::PageReady
        && fault_slot_.state != FaultSlot::State::PageFailed) {
        return libk::unexpected(VprocError::InvalidState);
    }
    /*luna change: accept a terminal snapshot while callback retirement is still published, reason: PageReady/Claimed may briefly retain a detached relation publisher*/
    if (fault_slot_.relation_generation != 0) {
        KASSERT(fault_slot_.publishers != 0
            && !fault_slot_.page_wait.attached());
    }
    KASSERT(fault_slot_.memory != nullptr);
    KASSERT(__atomic_load_n(
                &runtime_.events->fault_key, __ATOMIC_ACQUIRE)
        == static_cast<myos_fault_key_t>(key));
    const auto kind = static_cast<mm::FaultKind>(fault_slot_.kind);
    fault_slot_.state = FaultSlot::State::Claimed;
    /*luna change: acknowledge only the matching fault identity, reason: claim consumes the projected edge without rewinding the monotonic event sequence*/
    __atomic_store_n(
        &runtime_.events->fault_key, 0, __ATOMIC_RELEASE);
    __atomic_store_n(
        &runtime_.events->fault_kind,
        MYOS_VPROC_FAULT_KIND_NONE,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &runtime_.events->fault_access,
        MYOS_VPROC_FAULT_ACCESS_NONE,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &runtime_.events->fault_address, 0, __ATOMIC_RELAXED);
    __atomic_store_n(
        &runtime_.events->fault_pc, 0, __ATOMIC_RELAXED);
    return libk::expected(kind);
}

auto Vproc::resume_fault(
    arch::TrapContext& trap,
    u64 key) noexcept -> libk::Expected<mm::FaultKind, VprocError> {
    mm::VSpace* vspace{};
    CpuRegistry* cpus{};
    CpuId local{};
    mm::VirtAddr address{};
    mm::Access access{mm::Access::Read};
    mm::MemoryObject* old_memory{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        if (upcall_state_ != UpcallState::Active) {
            return libk::unexpected(VprocError::InvalidState);
        }
        if (key == 0 || key != fault_slot_.generation) {
            return libk::unexpected(VprocError::InvalidKey);
        }
        if (fault_slot_.state != FaultSlot::State::Claimed) {
            return libk::unexpected(VprocError::InvalidState);
        }
        vspace = execution_.binding().vspace();
        if (vspace == nullptr || stop_requested_ || stopped_
            || relation_admission_closed_) {
            return libk::unexpected(VprocError::InvalidState);
        }
        /*luna change: defer resume while an earlier callback publisher still owns the slot, reason: the new admission must linearize only after callback and relation publishers drain*/
        if (fault_slot_.publishers != 0) {
            return libk::expected(mm::FaultKind::Busy);
        }
        /*luna change: rearm only after terminal callback publishers drain, reason: a new relation generation must be installed from a detached state*/
        KASSERT(fault_slot_.relation_generation == 0);
        KASSERT(!fault_slot_.page_wait.attached()
            && fault_slot_.cpus != nullptr
            && current_cpu().descriptor != nullptr);
        cpus = fault_slot_.cpus;
        local = current_cpu().descriptor->logical_id();
        address = fault_slot_.address;
        access = fault_slot_.access;
        old_memory = fault_slot_.memory;
        fault_slot_.memory = nullptr;
        /*luna change: retire the previous relation generation before a new admission, reason: callback validation must accept only the new embedded WaitRelation generation*/
        fault_slot_.relation_generation = 0;
        KASSERT(fault_slot_.publishers != libk::numeric_limits<u8>::max());
        ++fault_slot_.publishers; // resume publisher
        fault_slot_.state = FaultSlot::State::Resuming;
    }
    /*luna change: release the old claimed pin before revalidation, reason: retry must transfer exactly one existing operations_ hold rather than accumulate pins*/
    if (old_memory != nullptr) {
        old_memory->release_fault();
    }

    const auto result = vspace->fault(
        mm::VmContext{.cpus = cpus, .local = local},
        address,
        access,
        &fault_slot_.page_wait,
        this,
        &Vproc::publish_fault,
        &fault_slot_.demand);
    mm::FaultKind outcome = result
        ? result.value().kind : mm::fault_kind(result.error());
    bool stop_fault{};
    bool consume{};
    mm::MemoryObject* drop_memory{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        FaultSlot& current = fault_slot_;
        const bool stopping = stop_requested_ || stopped_
            || relation_admission_closed_;
        if (result && (result.value().kind == mm::FaultKind::Pending
                       || result.value().kind == mm::FaultKind::Pressure)) {
            KASSERT(result.value().memory != nullptr);
            current.memory = result.value().memory;
            if (current.state == FaultSlot::State::Resuming) {
                /*luna change: consume the backing-owned relation handoff for
                  Pending or Pressure, reason: materialize_impl already
                  attached durable pressure before returning*/
                if (result.value().kind == mm::FaultKind::Pressure) {
                    current.kind = static_cast<u8>(mm::FaultKind::Pressure);
                }
                KASSERT(current.page_wait.generation != 0);
                current.relation_generation = current.page_wait.generation;
                current.state = FaultSlot::State::PendingPage;
                KASSERT(current.publishers
                    != libk::numeric_limits<u8>::max());
                ++current.publishers;
                stop_fault = stopping;
            } else if (current.state == FaultSlot::State::PageReady
                || current.state == FaultSlot::State::PageFailed) {
                KASSERT(current.relation_generation == 0);
                project_fault_locked();
                stop_fault = stopping;
            } else {
                KASSERT(current.state == FaultSlot::State::Dropping);
                KASSERT(current.relation_generation == 0);
                drop_memory = current.memory;
                current.memory = nullptr;
                stop_fault = true;
            }
        } else {
            KASSERT(current.state == FaultSlot::State::Resuming);
            if (stopping) {
                current.state = FaultSlot::State::Dropping;
                outcome = mm::FaultKind::BackingFailed;
            } else if (outcome == mm::FaultKind::Ready
                || outcome == mm::FaultKind::Materialized) {
                /*luna change: consume the exact frame only after the
                  revalidation winner, reason: a successful resume is the
                  sole path that redirects and frees FaultSlot*/
                trap.redirect(current.frame);
                KASSERT(current.publishers == 1);
                --current.publishers; // resume publisher
                current.reset();
                upcall_state_ = UpcallState::Armed;
                __atomic_store_n(
                    &runtime_.events->active_generation,
                    0,
                    __ATOMIC_RELEASE);
                consume = true;
            } else {
                current.state = FaultSlot::State::Claimed;
            }
        }
    }
    if (drop_memory != nullptr) {
        drop_memory->release_fault();
    }
    if (stop_fault) {
        close_fault();
        outcome = mm::FaultKind::BackingFailed;
    }
    if (!consume) {
        kernel::sync::IrqLockGuard guard{state_lock_};
        FaultSlot& current = fault_slot_;
        KASSERT(current.publishers != 0);
        --current.publishers; // resume publisher
        if (current.state == FaultSlot::State::Dropping
            && current.publishers == 0) {
            current.reset();
        }
    }
    retry_stop_if_ready();
    return libk::expected(outcome);
}

auto Vproc::drop_fault(u64 key) noexcept
    -> libk::Expected<void, VprocError> {
    mm::MemoryObject* memory{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        if (upcall_state_ != UpcallState::Active) {
            return libk::unexpected(VprocError::InvalidState);
        }
        if (key == 0 || key != fault_slot_.generation) {
            return libk::unexpected(VprocError::InvalidKey);
        }
        if (fault_slot_.state != FaultSlot::State::Claimed) {
            return libk::unexpected(VprocError::InvalidState);
        }
        /*luna change: let drop settle a claimed pin while callback retirement is live, reason: the callback owns only the detached relation publisher until it clears its generation*/
        if (fault_slot_.relation_generation != 0) {
            KASSERT(fault_slot_.publishers != 0
                && !fault_slot_.page_wait.attached());
        }
        fault_slot_.state = FaultSlot::State::Dropping;
        KASSERT(fault_slot_.publishers != libk::numeric_limits<u8>::max());
        ++fault_slot_.publishers;
        memory = fault_slot_.memory;
        fault_slot_.memory = nullptr;
    }
    /*luna change: settle the claimed continuation outside Vproc ownership, reason: MemoryObject lifetime release must never run under state_lock_*/
    if (memory != nullptr) {
        memory->release_fault();
    }
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        FaultSlot& slot = fault_slot_;
        KASSERT(slot.state == FaultSlot::State::Dropping
            && slot.publishers != 0);
        slot.demand.reset();
        --slot.publishers;
        if (slot.publishers == 0) {
            slot.reset();
        }
    }
    retry_stop_if_ready();
    return libk::expected();
}

void Vproc::close_fault() noexcept {
    mm::MemoryObject* memory{};
    u64 generation{};
    bool cancelable{};
    bool relation_live{};
    bool pressure{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        FaultSlot& slot = fault_slot_;
        if (slot.state == FaultSlot::State::Free
            || slot.state == FaultSlot::State::Attaching
            || slot.state == FaultSlot::State::Resuming
            || slot.state == FaultSlot::State::Dropping) {
            return;
        }
        relation_live = slot.relation_generation != 0;
        pressure = static_cast<mm::FaultKind>(slot.kind)
            == mm::FaultKind::Pressure;
        cancelable = slot.state == FaultSlot::State::PendingPage
            && relation_live
            && slot.page_wait.generation == slot.relation_generation
            && slot.page_wait.attached();
        /*luna change: separate cancel eligibility from relation publisher life, reason: terminal callback retirement may keep a detached generation after PageReady or Claimed*/
        if (relation_live) {
            KASSERT(slot.publishers != 0);
        }
        if (slot.state == FaultSlot::State::PendingPage) {
            KASSERT(relation_live
                && slot.page_wait.generation == slot.relation_generation);
        }
        slot.state = FaultSlot::State::Dropping;
        KASSERT(slot.publishers != libk::numeric_limits<u8>::max());
        ++slot.publishers;
        /*luna change: move the sole fault pin before foreign cancellation, reason: callback races must not invalidate close's MemoryObject lifetime owner*/
        memory = slot.memory;
        slot.memory = nullptr;
        slot.demand.reset();
        generation = slot.relation_generation;
        KASSERT(!cancelable || memory != nullptr);
        /*luna change: withdraw the ready fault projection when stop wins, reason: a closed lane cannot leave a stale FaultKey visible across reuse*/
        __atomic_store_n(
            &runtime_.events->fault_key, 0, __ATOMIC_RELEASE);
        __atomic_store_n(
            &runtime_.events->fault_kind,
            MYOS_VPROC_FAULT_KIND_NONE,
            __ATOMIC_RELAXED);
        __atomic_store_n(
            &runtime_.events->fault_access,
            MYOS_VPROC_FAULT_ACCESS_NONE,
            __ATOMIC_RELAXED);
        __atomic_store_n(
            &runtime_.events->fault_address, 0, __ATOMIC_RELAXED);
        __atomic_store_n(
            &runtime_.events->fault_pc, 0, __ATOMIC_RELAXED);
    }
    bool canceled{};
    if (cancelable) {
        canceled = pressure
            ? memory->release_pressure(fault_slot_.page_wait, generation)
            : memory->cancel_fault(fault_slot_.page_wait, generation);
        if (pressure && canceled) {
            /*luna change: drop the pressure pin after relation unlink wins,
              reason: PageReclaimer release owns no MemoryObject operation*/
            memory->release_fault();
        }
        if (!canceled) {
            /*luna change: settle the moved pin when cancel loses, reason: the callback retains only relation publisher ownership after host detach*/
            memory->release_fault();
        }
    } else if (memory != nullptr) {
        memory->release_fault();
    }
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        FaultSlot& slot = fault_slot_;
        if (slot.state == FaultSlot::State::Dropping) {
            KASSERT(slot.publishers != 0);
            /*luna change: settle stop and relation owners by cancellation outcome, reason: a losing cancel must wait for the terminal callback instead of freeing early*/
            --slot.publishers; // close publisher
            if (canceled) {
                KASSERT(slot.publishers != 0);
                /*luna change: clear the relation owner only when cancel wins, reason: a failed cancel leaves publisher retirement to the finalized callback*/
                slot.relation_generation = 0;
                --slot.publishers; // relation publisher
            }
            if (slot.publishers == 0) {
                slot.reset();
            }
        }
    }
    retry_stop_if_ready();
}

auto Vproc::resume(
    arch::TrapContext& trap,
    u64 generation) noexcept -> libk::Expected<void, VprocError> {
    kernel::sync::IrqLockGuard guard{state_lock_};
    if (upcall_state_ != UpcallState::Active || generation == 0
        || generation != upcall_generation_
        || __atomic_load_n(
               &runtime_.control->resume_generation,
               __ATOMIC_ACQUIRE) != generation) {
        return libk::unexpected(VprocError::InvalidState);
    }
    const myos_user_context submitted = runtime_.control->resume;
    if (!trap.load_user(submitted)) {
        return libk::unexpected(VprocError::InvalidRuntime);
    }
    upcall_state_ = UpcallState::Armed;
    __atomic_store_n(
        &runtime_.events->active_generation, 0, __ATOMIC_RELEASE);
    return libk::expected();
}

auto Vproc::prepare_retire() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{state_lock_};
    if ((execution_.state_ != State::Prepared
            && execution_.state_ != State::Exited)
        || execution_.scheduler_binding_ != nullptr
        || execution_.home_ != nullptr
        || authority_.active()
        || (!execution_.binding().detached()
            && !execution_.binding().kernel_bound())) {
        return false;
    }
    for (const OperationSlot& slot : operations_) {
        if (slot.state != OperationState::Free) {
            return false;
        }
    }
    if (!outgoing_tunnels_.empty()) {
        return false;
    }
    for (const IngressSlot& ingress : ingresses_) {
        if (ingress.link != nullptr) {
            return false;
        }
    }
    for (const NotificationSlot& notification : notifications_) {
        if (notification.link != nullptr) {
            return false;
        }
    }
    /*luna change: block retire until FaultSlot and its callback publishers drain, reason: stop/reuse must not release an admitted MemoryObject pin*/
    if (arm_attaching_ || activation_publishers_ != 0
        || activation_post_ != ActivationPost::Idle || activation_dirty_
        || fault_slot_.state != FaultSlot::State::Free
        || fault_slot_.publishers != 0 || !pager_claims_.empty()) {
        return false;
    }
    // ObjectPool changes lifecycle to Retiring before this callback. Closing
    // admission here prevents an already pinned constructor from attaching a
    // new relation after the empty-list check has linearized.
    relation_admission_closed_ = true;
    return true;
}

auto Vproc::in_upcall() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{state_lock_};
    return upcall_state_ == UpcallState::Active;
}

void Vproc::request_stop(execution::Stop& request) noexcept {
    bool finish{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        KASSERT(request.started_ && request.target_ == &execution_);
        stops_.push_back(request);
        if (stopped_) {
            stops_.erase(request);
            finish = true;
        }
    }
    if (finish) {
        request.finish(*this);
        return;
    }
    request_exit();
}

void Vproc::release_pager_claims() noexcept {
    for (usize ticket = 0; ticket < pager::claims_per_execution; ++ticket) {
        const pager::ClaimIndex::Entry entry = pager_claims_.entries[ticket];
        if (entry.pager == nullptr) {
            continue;
        }
        /*luna change: invalidate through the exact requeue owner edge,
          reason: terminal claim release must never become a page terminal
          winner and the entry clears regardless of the outcome*/
        static_cast<void>(entry.pager->invalidate_claim(entry));
        pager_claims_.clear(ticket);
    }
}

void Vproc::request_exit() noexcept {
    bool initiate{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        if (!stopped_ && !stop_requested_) {
            stop_requested_ = true;
            relation_admission_closed_ = true;
            initiate = true;
        }
    }
    if (initiate) {
        cancel_operations();
        close_tunnels();
        close_notifications();
        /*luna change: settle the fixed fault continuation during stop, reason: relation cancellation and pin release must precede Vproc reuse*/
        close_fault();
        /*luna change: settle service claims at the lane terminal, reason: a
          stopped worker lane must not hold a Pager claim hostage against
          graceful close*/
        release_pager_claims();
    }
    retry_stop_if_ready();
}

void Vproc::request_normal_exit(myos_status_t status) noexcept {
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        // A stop request that already won admission remains the terminal
        // owner.  NormalExit is recorded only when this syscall is the first
        // request to close the execution.
        if (stopped_ || stop_requested_) {
            return;
        }
        normal_exit_requested_ = true;
        normal_exit_status_ = status;
    }
    request_exit();
}

auto Vproc::activation_quiescent() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{state_lock_};
    return activation_publishers_ == 0
        && activation_post_ == ActivationPost::Idle && !activation_dirty_;
}

void Vproc::activation_publisher_done() noexcept {
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        KASSERT(activation_publishers_ != 0);
        --activation_publishers_;
    }
    retry_stop_if_ready();
}

auto Vproc::activation_request_posted(bool posted) noexcept -> bool {
    bool repost{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        if (!posted) {
            KASSERT(activation_post_ == ActivationPost::Posting);
            activation_post_ = ActivationPost::Idle;
            activation_dirty_ = false;
        } else if (activation_post_ == ActivationPost::Posting) {
            activation_post_ = ActivationPost::Pending;
        } else {
            KASSERT(activation_post_ == ActivationPost::Consumed);
            if (activation_dirty_) {
                activation_dirty_ = false;
                activation_post_ = ActivationPost::Posting;
                repost = true;
            } else {
                activation_post_ = ActivationPost::Idle;
            }
        }
    }
    retry_stop_if_ready();
    return repost;
}

auto Vproc::activation_request_consumed() noexcept -> bool {
    bool retry{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        KASSERT(activation_post_ == ActivationPost::Posting
            || activation_post_ == ActivationPost::Pending);
        if (activation_dirty_) {
            activation_dirty_ = false;
            retry = true;
        } else {
            activation_post_ = activation_post_ == ActivationPost::Posting
                ? ActivationPost::Consumed : ActivationPost::Idle;
        }
    }
    retry_stop_if_ready();
    return retry;
}

void Vproc::retry_stop_if_ready() noexcept {
    sched::CpuDispatcher* home{};
    sched::SchedulingContext* context{};
    bool finish{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        if (!stop_requested_ || stop_dispatched_ || stopped_) {
            return;
        }
        for (const OperationSlot& slot : operations_) {
            if (slot.state == OperationState::Pending) {
                return;
            }
        }
        if (arm_attaching_ || activation_publishers_ != 0
            || activation_post_ != ActivationPost::Idle
            || activation_dirty_
            || fault_slot_.state != FaultSlot::State::Free
            || fault_slot_.publishers != 0) {
            return;
        }
        home = execution_.home_;
        stop_dispatched_ = true;
        if (home == nullptr) {
            KASSERT(execution_.state_ == State::Prepared
                || execution_.state_ == State::Exited);
            execution_.set_state(State::Exited);
            if (execution_.scheduler_binding_ != nullptr) {
                context = &execution_.scheduler_binding_->context();
            } else {
                finish = true;
            }
        }
    }
    if (home != nullptr) {
        home->request_stop(*this);
    } else if (context != nullptr) {
        KASSERT(context->unbind());
        finish_stop();
    } else if (finish) {
        finish_stop();
    }
}

void Vproc::finish_terminal(
    fault::Reason reason,
    myos_status_t status) noexcept {
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        KASSERT(!arm_attaching_ && activation_publishers_ == 0
            && activation_post_ == ActivationPost::Idle
            && !activation_dirty_);
        KASSERT(fault_slot_.state == FaultSlot::State::Free
            && fault_slot_.publishers == 0);
        KASSERT(stop_requested_ && stop_dispatched_ && !stopped_);
        for (OperationSlot& slot : operations_) {
            KASSERT(slot.state != OperationState::Pending);
            slot.completion = nullptr;
            slot.status = MYOS_STATUS_OK;
            slot.value = 0;
            slot.cookie = 0;
            slot.state = OperationState::Free;
        }
        ready_mask_ = 0;
        ingress_mask_ = 0;
        notification_mask_ = 0;
        park_sequence_ = 0;
        park_requested_ = false;
        __atomic_store_n(&runtime_.events->ready_mask, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&runtime_.events->ingress_mask, 0, __ATOMIC_RELEASE);
        __atomic_store_n(
            &runtime_.events->notification_mask, 0, __ATOMIC_RELEASE);
        /*luna change: clear the fault projection with its canonical slot, reason: a stopped Vproc must not expose a stale generation to a reused runtime page*/
        __atomic_store_n(
            &runtime_.events->fault_key, 0, __ATOMIC_RELEASE);
        __atomic_store_n(
            &runtime_.events->fault_kind,
            MYOS_VPROC_FAULT_KIND_NONE,
            __ATOMIC_RELAXED);
        __atomic_store_n(
            &runtime_.events->fault_access,
            MYOS_VPROC_FAULT_ACCESS_NONE,
            __ATOMIC_RELAXED);
        __atomic_store_n(
            &runtime_.events->fault_address, 0, __ATOMIC_RELAXED);
        __atomic_store_n(
            &runtime_.events->fault_pc, 0, __ATOMIC_RELAXED);
        KASSERT(execution_.state_ == State::Exited
            && execution_.scheduler_binding_ == nullptr);
        execution_.home_ = nullptr;
        stopped_ = true;
        upcall_state_ = UpcallState::Unarmed;
    }
    execution_.binding().detach_user();
    authority_.target_stopped();
    static_cast<void>(terminal_.claim(
        reason, status));
    arm_ = {};
    runtime_.control_page.reset();
    runtime_.event_page.reset();
    runtime_.control_view.reset();
    runtime_.event_view.reset();
    runtime_.control = nullptr;
    runtime_.events = nullptr;

    for (;;) {
        execution::Stop* request{};
        {
            kernel::sync::IrqLockGuard guard{state_lock_};
            if (stops_.empty()) {
                return;
            }
            request = &stops_.pop_front();
        }
        request->finish(*this);
    }
}

void Vproc::finish_stop() noexcept {
    bool normal{};
    myos_status_t status{MYOS_STATUS_CANCELED};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        normal = normal_exit_requested_;
        if (normal) {
            status = normal_exit_status_;
        }
    }
    finish_terminal(
        normal && status == MYOS_STATUS_OK
            ? fault::Reason::NormalExit
            : normal ? fault::Reason::ExitFailure : fault::Reason::Stop,
        status);
}

void Vproc::finish_exit(myos_status_t status) noexcept {
    finish_terminal(
        status == MYOS_STATUS_OK
            ? fault::Reason::NormalExit : fault::Reason::ExitFailure,
        status);
}

[[noreturn]] void Vproc::start(void* argument) noexcept {
    auto* const vproc = static_cast<Vproc*>(argument);
    KASSERT(vproc != nullptr && vproc->execution_.state_ == State::Running);
    CpuLocal& cpu = current_cpu();
    KASSERT(cpu.dispatcher() != nullptr);
    cpu.dispatcher()->on_context_enter();
    /*luna change: resume the ordinary Vproc frame below the reserved cell, reason: the top cell is retained exclusively for FaultSlot*/
    const usize active_stack_top = arch::vproc_stack_top(
        vproc->execution_.stack_top());
    KASSERT(active_stack_top != 0);
    arch::resume_user(active_stack_top);
}

} // namespace kernel
