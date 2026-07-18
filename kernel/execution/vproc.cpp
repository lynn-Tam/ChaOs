#include <execution/vproc.hpp>

#include <arch/cpu.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <libk/limits.hpp>
#include <libk/utility.hpp>
#include <operation/completion.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel {
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

    const auto kernel_stack_top = arch::prepare_user_stack(
        execution_.stack_top(), bootstrap_entry_);
    KASSERT(kernel_stack_top);
    execution_.prepare(&Vproc::start, this, *kernel_stack_top);
}

Vproc::~Vproc() noexcept {
    KASSERT(execution_.state_ != State::Running);
    KASSERT(execution_.scheduler_binding_ == nullptr);
    KASSERT(execution_.home_ == nullptr);
    KASSERT(stops_.empty());
    KASSERT(outgoing_tunnels_.empty() && !activation_.pending());
    KASSERT(!arm_attaching_ && activation_publishers_ == 0
        && !activation_request_held_ && !activation_posting_);
    for (const IngressSlot& ingress : ingresses_) {
        KASSERT(ingress.link == nullptr);
    }
    for (const OperationSlot& slot : operations_) {
        KASSERT(slot.state == OperationState::Free && slot.completion == nullptr);
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
    if (stop_requested_ || stopped_ || execution_.state_ == State::Exited) {
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
        }
        if (!completion->cancel()) {
            continue;
        }
        kernel::sync::IrqLockGuard guard{state_lock_};
        OperationSlot& slot = operations_[index];
        KASSERT(slot.state == OperationState::Pending
            && slot.generation == key.generation()
            && slot.completion == completion);
        slot.completion = nullptr;
        slot.status = MYOS_STATUS_OK;
        slot.value = 0;
        slot.cookie = 0;
        slot.state = OperationState::Free;
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

auto Vproc::take_operation(operation::Key key) noexcept
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
    slot.status = MYOS_STATUS_OK;
    slot.value = 0;
    slot.cookie = 0;
    slot.state = OperationState::Free;
    ready_mask_ &= ~(u64{1} << key.slot());
    __atomic_fetch_and(
        &runtime_.events->ready_mask,
        ~(u64{1} << key.slot()),
        __ATOMIC_RELEASE);
    return libk::expected(result);
}

auto Vproc::pending_sequence() const noexcept -> u64 {
    kernel::sync::IrqLockGuard guard{state_lock_};
    return pending_sequence_;
}

auto Vproc::pending_events() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{state_lock_};
    return ready_mask_ != 0 || ingress_mask_ != 0;
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

void Vproc::on_trap_exit(arch::TrapContext& trap) noexcept {
    const myos_word_t disabled = __atomic_load_n(
        &runtime_.control->upcall_disable_depth, __ATOMIC_ACQUIRE);
    if (disabled != 0) {
        return;
    }
    arch::UserStart entry{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        if (upcall_state_ != UpcallState::Armed
            || (ready_mask_ == 0 && ingress_mask_ == 0)
            || stop_requested_) {
            return;
        }
        trap.save_user(runtime_.events->delivered);
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
    if (arm_attaching_ || activation_publishers_ != 0
        || activation_request_held_
        || activation_posting_ || activation_.pending()) {
        return false;
    }
    // ObjectPool changes lifecycle to Retiring before this callback. Closing
    // admission here prevents an already pinned constructor from attaching a
    // new relation after the empty-list check has linearized.
    tunnel_admission_closed_ = true;
    return true;
}

void Vproc::request_stop(execution::Stop& request) noexcept {
    bool finish{};
    bool initiate{};
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        auto** const target = libk::get_if<Vproc*>(&request.target_);
        KASSERT(request.started_ && target != nullptr && *target == this);
        stops_.push_back(request);
        if (stopped_) {
            stops_.erase(request);
            finish = true;
        } else if (!stop_requested_) {
            stop_requested_ = true;
            tunnel_admission_closed_ = true;
            initiate = true;
        }
    }
    if (initiate) {
        cancel_operations();
        close_tunnels();
    }
    if (finish) {
        request.finish(*this);
        return;
    }
    retry_stop_if_ready();
}

auto Vproc::activation_quiescent() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{state_lock_};
    return activation_publishers_ == 0 && !activation_request_held_
        && !activation_posting_ && !activation_.pending();
}

void Vproc::activation_publisher_done() noexcept {
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        KASSERT(activation_publishers_ != 0);
        --activation_publishers_;
    }
    retry_stop_if_ready();
}

void Vproc::activation_request_posted(bool posted) noexcept {
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        KASSERT(activation_posting_ && activation_request_held_);
        activation_posting_ = false;
        if (!posted) {
            activation_request_held_ = false;
        }
    }
    retry_stop_if_ready();
}

void Vproc::activation_request_consumed() noexcept {
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        KASSERT(activation_request_held_ && !activation_posting_);
        activation_request_held_ = false;
    }
    retry_stop_if_ready();
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
            || activation_request_held_
            || activation_posting_ || activation_.pending()) {
            return;
        }
        home = execution_.home_;
        stop_dispatched_ = true;
        if (home == nullptr) {
            KASSERT(execution_.state_ == State::Prepared
                || execution_.state_ == State::Exited);
            execution_.state_ = State::Exited;
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

void Vproc::finish_stop() noexcept {
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        KASSERT(!arm_attaching_ && activation_publishers_ == 0
            && !activation_request_held_ && !activation_posting_
            && !activation_.pending());
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
        __atomic_store_n(&runtime_.events->ready_mask, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&runtime_.events->ingress_mask, 0, __ATOMIC_RELEASE);
        KASSERT(execution_.state_ == State::Exited
            && execution_.scheduler_binding_ == nullptr);
        execution_.home_ = nullptr;
        stopped_ = true;
        upcall_state_ = UpcallState::Unarmed;
    }
    execution_.binding().detach_user();
    authority_.target_stopped();
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

[[noreturn]] void Vproc::start(void* argument) noexcept {
    auto* const vproc = static_cast<Vproc*>(argument);
    KASSERT(vproc != nullptr && vproc->execution_.state_ == State::Running);
    CpuLocal& cpu = current_cpu();
    KASSERT(cpu.dispatcher() != nullptr);
    cpu.dispatcher()->on_context_enter();
    arch::resume_user(vproc->execution_.stack_top());
}

} // namespace kernel
