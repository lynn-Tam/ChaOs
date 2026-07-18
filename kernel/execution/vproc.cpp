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
      runtime_entry_(runtime_entry),
      runtime_(libk::move(runtime)) {
    KASSERT(execution_.binding().user_bound());
    KASSERT(arch::valid_user_start(runtime_entry_));
    KASSERT(valid_runtime(runtime_));

    *runtime_.control = {};
    runtime_.control->version = MYOS_VPROC_RUNTIME_VERSION;
    *runtime_.events = {};
    runtime_.events->version = MYOS_VPROC_RUNTIME_VERSION;

    const auto kernel_stack_top = arch::prepare_user_stack(
        execution_.stack_top(), runtime_entry_);
    KASSERT(kernel_stack_top);
    execution_.prepare(&Vproc::start, this, *kernel_stack_top);
}

Vproc::~Vproc() noexcept {
    KASSERT(execution_.state_ != State::Running);
    KASSERT(execution_.scheduler_binding_ == nullptr);
    KASSERT(execution_.home_ == nullptr);
    KASSERT(stops_.empty());
    KASSERT(outgoing_tunnels_.empty() && !activation_.pending());
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
    kernel::sync::IrqLockGuard guard{operation_lock_};
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
    sched::Binding* scheduler_binding{};
    bool pending{};
    {
        kernel::sync::IrqLockGuard guard{operation_lock_};
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
        scheduler_binding = execution_.scheduler_binding_;
        for (const OperationSlot& current : operations_) {
            pending = pending || current.state == OperationState::Pending;
        }
    }
    sched::CpuDispatcher* stop_home{};
    {
        kernel::sync::IrqLockGuard guard{stop_lock_};
        if (stop_requested_ && !pending) {
            stop_home = execution_.home_;
        }
    }
    if (stop_home != nullptr) {
        stop_home->request_stop(*this);
        return;
    }
    if (scheduler_binding != nullptr) {
        // Readiness is canonical in the operation table.  A failed kick can
        // delay delivery but cannot lose the result.
        static_cast<void>(sched::wake(cpus, *scheduler_binding));
    }
}

void Vproc::cancel_operations() noexcept {
    for (usize index = 0; index < max_operations; ++index) {
        operation::Completion* completion{};
        operation::Key key{};
        {
            kernel::sync::IrqLockGuard guard{operation_lock_};
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
        kernel::sync::IrqLockGuard guard{operation_lock_};
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
    kernel::sync::IrqLockGuard guard{operation_lock_};
    for (const OperationSlot& slot : operations_) {
        if (slot.state == OperationState::Pending) {
            return true;
        }
    }
    return false;
}

auto Vproc::take_operation(operation::Key key) noexcept
    -> libk::Expected<operation::Result, VprocError> {
    kernel::sync::IrqLockGuard guard{operation_lock_};
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
    kernel::sync::IrqLockGuard guard{operation_lock_};
    return pending_sequence_;
}

auto Vproc::pending_events() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{operation_lock_};
    return ready_mask_ != 0 || ingress_mask_ != 0;
}

void Vproc::on_trap_exit(arch::TrapContext& trap) noexcept {
    if (upcall_active_ || !pending_events()) {
        return;
    }
    const myos_word_t disabled = __atomic_load_n(
        &runtime_.control->upcall_disable_depth, __ATOMIC_ACQUIRE);
    if (disabled != 0) {
        return;
    }

    trap.save_user(runtime_.events->delivered);
    ++upcall_generation_;
    KASSERT(upcall_generation_ != 0);
    upcall_active_ = true;
    __atomic_store_n(
        &runtime_.events->active_generation,
        upcall_generation_,
        __ATOMIC_RELEASE);

    arch::UserStart entry = runtime_entry_;
    entry.arguments[0] = upcall_generation_;
    entry.arguments[1] = runtime_.event_address.raw();
    entry.arguments[2] = runtime_.control_address.raw();
    {
        kernel::sync::IrqLockGuard guard{operation_lock_};
        entry.arguments[3] = ready_mask_;
    }
    KASSERT(trap.load_user_start(entry));
}

auto Vproc::resume(
    arch::TrapContext& trap,
    u64 generation) noexcept -> libk::Expected<void, VprocError> {
    if (!upcall_active_ || generation == 0
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
    upcall_active_ = false;
    __atomic_store_n(
        &runtime_.events->active_generation, 0, __ATOMIC_RELEASE);
    return libk::expected();
}

auto Vproc::prepare_retire() const noexcept -> bool {
    kernel::sync::IrqLockGuard stop_guard{stop_lock_};
    if ((execution_.state_ != State::Prepared
            && execution_.state_ != State::Exited)
        || execution_.scheduler_binding_ != nullptr
        || execution_.home_ != nullptr
        || authority_.active()
        || (!execution_.binding().detached()
            && !execution_.binding().kernel_bound())) {
        return false;
    }
    kernel::sync::IrqLockGuard operation_guard{operation_lock_};
    for (const OperationSlot& slot : operations_) {
        if (slot.state != OperationState::Free) {
            return false;
        }
    }
    kernel::sync::IrqLockGuard tunnel_guard{tunnel_lock_};
    if (!outgoing_tunnels_.empty()) {
        return false;
    }
    for (const IngressSlot& ingress : ingresses_) {
        if (ingress.link != nullptr) {
            return false;
        }
    }
    // ObjectPool changes lifecycle to Retiring before this callback. Closing
    // admission here prevents an already pinned constructor from attaching a
    // new relation after the empty-list check has linearized.
    tunnel_admission_closed_ = true;
    return true;
}

void Vproc::request_stop(execution::Stop& request) noexcept {
    sched::CpuDispatcher* home{};
    sched::SchedulingContext* context{};
    bool finish{};
    bool initiate{};
    {
        kernel::sync::IrqLockGuard guard{stop_lock_};
        auto** const target = libk::get_if<Vproc*>(&request.target_);
        KASSERT(request.started_ && target != nullptr && *target == this);
        stops_.push_back(request);
        if (stopped_) {
            stops_.erase(request);
            finish = true;
        } else if (!stop_requested_) {
            stop_requested_ = true;
            initiate = true;
            home = execution_.home_;
            if (home == nullptr) {
                KASSERT(execution_.state_ == State::Prepared
                    || execution_.state_ == State::Exited);
                execution_.state_ = State::Exited;
                if (execution_.scheduler_binding_ == nullptr) {
                    stopped_ = true;
                    stops_.erase(request);
                    finish = true;
                } else {
                    context = &execution_.scheduler_binding_->context();
                }
            }
        }
    }
    if (initiate) {
        {
            kernel::sync::IrqLockGuard guard{tunnel_lock_};
            tunnel_admission_closed_ = true;
        }
        cancel_operations();
    }
    if (finish) {
        request.finish(*this);
    } else if (!initiate) {
        return;
    } else if (context != nullptr) {
        KASSERT(context->unbind());
        finish_stop();
    } else {
        KASSERT(home != nullptr);
        home->request_stop(*this);
    }
}

void Vproc::finish_stop() noexcept {
    close_tunnels();
    {
        kernel::sync::IrqLockGuard operation_guard{operation_lock_};
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
    }
    {
        kernel::sync::IrqLockGuard guard{stop_lock_};
        KASSERT(execution_.state_ == State::Exited
            && execution_.scheduler_binding_ == nullptr);
        execution_.home_ = nullptr;
        stopped_ = true;
    }
    execution_.binding().detach_user();
    runtime_.control_page.reset();
    runtime_.event_page.reset();
    runtime_.control_view.reset();
    runtime_.event_view.reset();
    runtime_.control = nullptr;
    runtime_.events = nullptr;
    authority_.target_stopped();

    for (;;) {
        execution::Stop* request{};
        {
            kernel::sync::IrqLockGuard guard{stop_lock_};
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
