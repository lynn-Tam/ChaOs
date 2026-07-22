#include <object/endpoint_pool.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <libk/limits.hpp>
#include <libk/mem.h>
#include <libk/scope_guard.hpp>
#include <libk/utility.hpp>
#include <mm/virtual_layout.hpp>
#include <sched/binding.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <sync/irq_lock_guard.hpp>
#include <thread/thread.hpp>

namespace kernel::ipc {

const cap::GrantAttachmentOps Call::authority_ops_{
    .invalidate = &Call::invalidate_authority,
    .released = &Call::authority_released,
};

Activation::Activation(
    Endpoint& endpoint,
    ExecutionBinding& service,
    kernel::resource::Charge&& stack_charge,
    KernelStack&& kernel_stack,
    kernel::mm::UserView&& user_stack,
    libk::optional<Buffer>&& ipc,
    StackPages&& resident,
    kernel::mm::VirtAddr user_stack_top) noexcept
    : endpoint_(&endpoint),
      stack_charge_(libk::move(stack_charge)),
      kernel_stack_(libk::move(kernel_stack)),
      user_stack_(libk::move(user_stack)),
      ipc_(libk::move(ipc)),
      resident_(libk::move(resident)),
      frame_(kernel_stack_, service, ipc_ ? &*ipc_ : nullptr, wait_,
          execution::Frame::Kind::Endpoint,
          this, &Endpoint::unwind_frame, &Endpoint::frame_cancel_pending),
      user_stack_top_(user_stack_top) {}

Activation::~Activation() noexcept {
    KASSERT(state_ == State::Free || state_ == State::Complete);
    KASSERT(call_ == nullptr && frame_.previous() == nullptr);
}

Call::Call(Endpoint& endpoint) noexcept
    : endpoint_(&endpoint),
      completion_(operation::Completion::bind_resume<
          Call,
          &Call::complete,
          &Call::read,
          &Call::release,
          &Call::cancel,
          &Call::resume>(*this)),
      deadline_(sched::Deadline::Callback::bind<&Call::expire>(*this)) {}

Call::~Call() noexcept {
    KASSERT(state_ == State::Free);
    KASSERT(!caller_ && !caller_frame_ && activation_ == nullptr);
    KASSERT(!authority_);
    KASSERT(!deadline_.armed());
}

auto Call::complete() const noexcept -> bool {
    return endpoint_->call_complete(*this);
}

auto Call::read() noexcept -> operation::Result {
    return endpoint_->read_call(*this);
}

void Call::release() noexcept {
    endpoint_->release_call(*this);
}

auto Call::cancel() noexcept -> bool {
    return endpoint_->cancel_call(*this);
}

void Call::resume(arch::TrapContext& trap) noexcept {
    endpoint_->resume_call(*this, trap);
}

void Call::expire() noexcept {
    endpoint_->expire_call(*this);
}

void Call::invalidate_authority(
    void* context,
    cap::GrantWork&& work,
    cap::GrantInvalidation reason) noexcept {
    KASSERT(context != nullptr && reason == cap::GrantInvalidation::Revoke);
    auto& call = *static_cast<Call*>(context);
    call.endpoint_->invalidate_call(call);
    // Call remains attached until its terminal transition. Grant revoke thus
    // waits for queued or active execution to unwind, while this callback work
    // only protects the invalidation publication itself.
    work.reset();
    call.endpoint_->authority_quiesced(call);
}

void Call::authority_released(void* context) noexcept {
    // invalidate_authority() retries reaping after GrantWork::reset() returns.
    // Ending the attachment lifetime from this callback would destroy it while
    // GrantAttachment::drop_work() is still on the stack.
    KASSERT(context != nullptr);
}

Endpoint::Endpoint(
    kernel::mm::Pmm& pmm,
    ExecutionBinding&& service,
    kernel::mm::UserView&& code,
    CodePages&& resident_code,
    EndpointConfig config) noexcept
    : service_(libk::move(service)),
      code_(libk::move(code)),
      resident_code_(libk::move(resident_code)),
      config_(config),
      activations_(pmm, kernel::mm::NodePool<Activation>::Quota{
          .nodes = config.capacity,
          .pages = config.capacity,
      }),
      calls_(pmm, kernel::mm::NodePool<Call>::Quota{
          .nodes = config.call_capacity,
          .pages = config.call_capacity,
      }) {}

Endpoint::~Endpoint() noexcept {
    KASSERT(state_ == State::Constructing || state_ == State::Closed);
    KASSERT(outstanding_ == 0 && !cleanup_);
    while (slot_count_ != 0) {
        Activation* const slot = slots_[--slot_count_];
        slots_[slot_count_] = nullptr;
        activations_.destroy(*slot);
    }
    while (call_count_ != 0) {
        Call* const call = call_slots_[--call_count_];
        call_slots_[call_count_] = nullptr;
        calls_.destroy(*call);
    }
}

void Endpoint::bind_sponsor(
    kernel::resource::Sponsorship& sponsor) noexcept {
    activations_.bind_sponsor(sponsor);
    calls_.bind_sponsor(sponsor);
}

auto Endpoint::add_call() noexcept
    -> libk::Expected<void, EndpointError> {
    if (state_ != State::Constructing
        || call_count_ >= config_.call_capacity
        || call_count_ >= MYOS_ENDPOINT_MAX_CALLS) {
        return libk::unexpected(EndpointError::InvalidConfig);
    }
    auto made = calls_.create(*this);
    if (!made) {
        return libk::unexpected(EndpointError::InvalidConfig);
    }
    call_slots_[call_count_++] = made.value().object;
    return libk::expected();
}

auto Endpoint::add_activation(
    kernel::resource::Charge&& stack_charge,
    KernelStack&& kernel_stack,
    kernel::mm::UserView&& user_stack,
    libk::optional<Buffer>&& ipc,
    StackPages&& resident,
    kernel::mm::VirtAddr user_stack_top) noexcept
    -> libk::Expected<void, EndpointError> {
    if (state_ != State::Constructing
        || slot_count_ >= config_.capacity
        || slot_count_ >= max_activations || !user_stack.valid()
        || !kernel::mm::layout::is_user(user_stack_top)
        || (user_stack_top.raw() & 0xfU) != 0) {
        return libk::unexpected(EndpointError::InvalidConfig);
    }
    auto made = activations_.create(
        *this,
        service_,
        libk::move(stack_charge),
        libk::move(kernel_stack),
        libk::move(user_stack),
        libk::move(ipc),
        libk::move(resident),
        user_stack_top);
    if (!made) {
        return libk::unexpected(EndpointError::InvalidConfig);
    }
    slots_[slot_count_++] = made.value().object;
    return libk::expected();
}

auto Endpoint::open() noexcept -> libk::Expected<void, EndpointError> {
    if (state_ != State::Constructing || !service_.user_bound()
        || !code_.valid() || config_.capacity == 0
        || config_.capacity > max_activations
        || config_.call_capacity < config_.capacity
        || config_.call_capacity > MYOS_ENDPOINT_MAX_CALLS
        || config_.max_depth == 0
        || config_.max_depth > MYOS_ENDPOINT_MAX_DEPTH
        || slot_count_ != config_.capacity
        || call_count_ != config_.call_capacity
        || !arch::valid_user_start(config_.entry)) {
        return libk::unexpected(EndpointError::InvalidConfig);
    }
    state_ = State::Open;
    return libk::expected();
}

auto Endpoint::hold(Thread& thread) noexcept
    -> libk::Expected<object::ThreadHold, EndpointError> {
    sched::Binding* const binding = thread.binding();
    if (binding == nullptr) {
        return libk::unexpected(EndpointError::InvalidCaller);
    }
    auto reference = binding->target_reference();
    if (!reference) {
        return libk::unexpected(EndpointError::InvalidCaller);
    }
    auto held = libk::move(reference).value().into_hold<Thread>();
    return held
        ? libk::Expected<object::ThreadHold, EndpointError>{
              libk::expected(libk::move(held).value())}
        : libk::unexpected(EndpointError::InvalidCaller);
}

auto Endpoint::depth(const Thread& thread) const noexcept -> usize {
    return thread.frame_depth();
}

auto Endpoint::snapshot_caps(
    const Buffer* buffer,
    usize limit,
    Transfer::Specs& specs,
    usize& receive_limit) noexcept -> bool {
    specs.clear();
    receive_limit = 0;
    if (buffer == nullptr) {
        return true;
    }
    myos_ipc_caps message{};
    if (!buffer->read(0, libk::Span<byte>{
            reinterpret_cast<byte*>(&message), sizeof(message)})
        || message.version != MYOS_IPC_CAPS_VERSION
        || message.flags != MYOS_IPC_CAPS_FLAGS_NONE
        || message.reserved != 0
        || message.send_count > MYOS_IPC_MAX_CAPS
        || message.receive_limit > MYOS_IPC_MAX_CAPS
        || message.send_count > limit || message.receive_limit > limit) {
        return false;
    }
    for (usize index = 0; index < message.send_count; ++index) {
        const myos_cap_transfer& wire = message.send[index];
        const auto rights = cap::Rights::from_raw(wire.rights);
        TransferKind kind{};
        switch (wire.operation) {
        case MYOS_CAP_COPY:
            kind = TransferKind::Copy;
            break;
        case MYOS_CAP_MOVE:
            kind = TransferKind::Move;
            break;
        case MYOS_CAP_DELEGATE:
            kind = TransferKind::Delegate;
            break;
        default:
            return false;
        }
        const cap::CapHandle source = cap::CapHandle::from_raw(wire.source);
        if (wire.flags != 0 || !source || !rights
            || (kind == TransferKind::Move && !rights->empty())
            || !specs.try_push_back(TransferSpec{
                source, *rights, kind})) {
            return false;
        }
    }
    receive_limit = message.receive_limit;
    return true;
}

auto Endpoint::commit_caps(
    Transfer& transfer,
    cap::CSpace& source,
    cap::CSpace& destination,
    const Transfer::Specs& specs,
    Buffer* receiver,
    Transfer::Handles& installed) noexcept -> bool {
    installed.clear();
    if (specs.empty()) {
        if (receiver == nullptr) {
            return true;
        }
        auto access = receiver->access();
        if (!access) {
            return false;
        }
        myos_ipc_caps projection{};
        projection.version = MYOS_IPC_CAPS_VERSION;
        return access.value().write(0, libk::Span<const byte>{
            reinterpret_cast<const byte*>(&projection), sizeof(projection)});
    }
    if (receiver == nullptr) {
        return false;
    }
    if (!Transfer::prepare(transfer, source, destination, specs)) {
        return false;
    }
    auto access = receiver->access();
    if (!access) {
        transfer.abort();
        return false;
    }
    myos_ipc_caps projection{};
    projection.version = MYOS_IPC_CAPS_VERSION;
    const Transfer::Handles reserved = transfer.handles();
    for (usize index = 0; index < reserved.size(); ++index) {
        projection.received[index] = reserved[index].raw();
    }
    if (!access.value().write(0, libk::Span<const byte>{
            reinterpret_cast<const byte*>(&projection), sizeof(projection)})) {
        transfer.abort();
        return false;
    }
    auto committed = transfer.commit();
    if (!committed) {
        projection = {};
        projection.version = MYOS_IPC_CAPS_VERSION;
        static_cast<void>(access.value().write(0, libk::Span<const byte>{
            reinterpret_cast<const byte*>(&projection), sizeof(projection)}));
        return false;
    }
    installed = libk::move(committed).value();
    projection.received_count = static_cast<uint32_t>(installed.size());
    KASSERT(access.value().write(0, libk::Span<const byte>{
        reinterpret_cast<const byte*>(&projection), sizeof(projection)}));
    return true;
}

void Endpoint::close_installed(Call& call) noexcept {
    cap::CSpace* const service = service_.cspace();
    KASSERT(service != nullptr);
    for (const cap::CapHandle handle : call.installed_caps_) {
        static_cast<void>(service->close(handle));
    }
    call.installed_caps_.clear();
}

auto Endpoint::call(
    const cap::Resolved<Endpoint>& authority,
    Thread& caller,
    arch::TrapContext& trap,
    sched::CpuDispatcher& dispatcher,
    CpuRegistry& cpus,
    const usize (&arguments)[3],
    libk::optional<time::Instant> deadline) noexcept
    -> libk::Expected<CallResult, EndpointError> {
    if (&authority.object() != this
        || dispatcher.current().thread() != &caller) {
        return libk::unexpected(EndpointError::InvalidCaller);
    }
    const cap::EffectiveAuthority effective = authority.authority();
    const auto* const endpoint_authority =
        libk::get_if<cap::EndpointAuthority>(&effective.data);
    if (endpoint_authority == nullptr || !endpoint_authority->callable()
        || !effective.rights.contains(cap::Right::Call)) {
        return libk::unexpected(EndpointError::Denied);
    }
    Transfer::Specs request_caps{};
    usize receive_limit{};
    if (!snapshot_caps(
            caller.ipc_buffer(), endpoint_authority->cap_limit,
            request_caps, receive_limit)) {
        return libk::unexpected(EndpointError::TransferFailed);
    }
    const usize call_depth = depth(caller);
    if (call_depth >= config_.max_depth) {
        return libk::unexpected(EndpointError::DepthExceeded);
    }
    if (dispatcher.remaining_budget() < config_.budget_floor) {
        return libk::unexpected(EndpointError::BudgetTooLow);
    }
    for (execution::Frame* frame = caller.active_frame(); frame != nullptr;
         frame = frame->previous()) {
        if (frame->kind() == execution::Frame::Kind::Endpoint
            && &static_cast<Activation*>(frame->owner())->endpoint()
                == this) {
            return libk::unexpected(EndpointError::DepthExceeded);
        }
    }
    auto caller_hold = hold(caller);
    if (!caller_hold) {
        return libk::unexpected(caller_hold.error());
    }

    Call* call{};
    Activation* activation{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Open) {
            return libk::unexpected(EndpointError::Closed);
        }
        for (usize index = 0; index < call_count_; ++index) {
            if (call_slots_[index]->state_ == Call::State::Free) {
                call = call_slots_[index];
                break;
            }
        }
        if (call == nullptr) {
            return libk::unexpected(EndpointError::QueueFull);
        }
        for (usize index = 0; index < slot_count_; ++index) {
            if (slots_[index]->state_ == Activation::State::Free) {
                activation = slots_[index];
                break;
            }
        }
        if (generation_ == libk::numeric_limits<u64>::max()) {
            return libk::unexpected(EndpointError::GenerationExhausted);
        }
        const u64 generation = ++generation_;
        call->state_ = Call::State::Preparing;
        call->generation_ = generation;
        call->sequence_ = generation;
        call->depth_ = call_depth + 1;
        const usize caller_urgency = dispatcher.current_urgency().value();
        const usize ceiling = config_.urgency_ceiling.value();
        call->urgency_ = caller_urgency < ceiling
            ? caller_urgency : ceiling;
        call->badge_ = endpoint_authority->badge;
        call->receive_limit_ = receive_limit;
        call->request_caps_ = libk::move(request_caps);
        call->cpus_ = &cpus;
        KASSERT(call->publishers_ == 0);
        call->publishers_ = 1; // admission producer lease
        if (activation != nullptr) {
            activation->state_ = Activation::State::Preparing;
            activation->call_ = call;
            call->activation_ = activation;
        }
        ++outstanding_;
    }

    call->caller_ = libk::move(caller_hold).value();
    for (usize index = 0; index < 3; ++index) {
        call->arguments_[index] = arguments[index];
    }
    auto& grant = call->authority_.emplace(call, Call::authority_ops_);
    if (!authority.attach(grant)) {
        {
            kernel::sync::IrqLockGuard guard{lock_};
            call->result_ = operation::Result{MYOS_STATUS_DENIED, 0};
            call->state_ = Call::State::Complete;
            if (activation != nullptr) {
                activation->state_ = Activation::State::Complete;
            }
        }
        publisher_done(*call);
        return libk::unexpected(EndpointError::Closed);
    }
    if (deadline && !dispatcher.arm(call->deadline_, *deadline)) {
        {
            kernel::sync::IrqLockGuard guard{lock_};
            call->result_ = operation::Result{MYOS_STATUS_INTERNAL, 0};
            call->state_ = Call::State::Complete;
            if (activation != nullptr) {
                activation->state_ = Activation::State::Complete;
            }
        }
        publisher_done(*call);
        return libk::unexpected(EndpointError::Busy);
    }

    if (activation == nullptr) {
        sched::Binding* const binding = caller.binding();
        if (binding == nullptr
            || !caller.current_wait().begin(
                call->completion_, cpus, *binding)) {
            {
                kernel::sync::IrqLockGuard guard{lock_};
                call->result_ = operation::Result{
                    MYOS_STATUS_WOULD_BLOCK, 0};
                call->state_ = Call::State::Complete;
            }
            if (call->deadline_.armed()) {
                dispatcher.disarm(call->deadline_);
            }
            publisher_done(*call);
            return libk::unexpected(EndpointError::Busy);
        }
        bool ready{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            if (state_ != State::Open || call->cancel_pending_) {
                if (!call->cancel_pending_) {
                    call->result_ = operation::Result{
                        MYOS_STATUS_CLOSED, 0};
                }
                call->state_ = Call::State::Complete;
                ready = true;
            } else {
                call->state_ = Call::State::Queued;
                for (usize index = 0; index < slot_count_; ++index) {
                    if (slots_[index]->state_ == Activation::State::Free) {
                        ready = next_call_locked(*slots_[index]) == call;
                        break;
                    }
                }
            }
        }
        publisher_done(*call);
        if (ready) {
            call->completion_.signal();
        }
        return libk::expected(CallResult{CallDisposition::Blocking});
    }

    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Open || call->cancel_pending_) {
            if (!call->cancel_pending_) {
                call->result_ = operation::Result{MYOS_STATUS_CLOSED, 0};
            }
            call->state_ = Call::State::Complete;
            activation->state_ = Activation::State::Complete;
        } else {
            call->state_ = Call::State::Ready;
        }
    }

    if (!enter(*call, trap, dispatcher)) {
        EndpointError error{EndpointError::Closed};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            if (call->state_ != Call::State::Complete) {
                call->result_ = operation::Result{MYOS_STATUS_CLOSED, 0};
                call->state_ = Call::State::Complete;
                activation->state_ = Activation::State::Complete;
            }
            if (call->result_.status == MYOS_STATUS_TRANSFER_FAILED) {
                error = EndpointError::TransferFailed;
            }
        }
        if (call->deadline_.armed()) {
            dispatcher.disarm(call->deadline_);
        }
        publisher_done(*call);
        return libk::unexpected(error);
    }
    publisher_done(*call);
    return libk::expected(CallResult{CallDisposition::Entered});
}

auto Endpoint::enter(
    Call& call,
    arch::TrapContext& trap,
    sched::CpuDispatcher& dispatcher) noexcept -> bool {
    Activation* const activation = call.activation_;
    Thread* const caller = dispatcher.current().thread();
    if (activation == nullptr || caller == nullptr
        || &call.caller_.get() != caller) {
        return false;
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Open || call.cancel_pending_
            || call.state_ != Call::State::Ready
            || activation->state_ != Activation::State::Preparing
            || activation->call_ != &call) {
            return false;
        }
        call.state_ = Call::State::Committing;
        activation->state_ = Activation::State::Committing;
    }
    arch::UserStart entry = config_.entry;
    entry.stack = activation->user_stack_top_;
    for (usize index = 0; index < 3; ++index) {
        entry.arguments[index] = call.arguments_[index];
    }
    entry.arguments[3] = call.badge_;
    entry.arguments[4] = 0;
    entry.arguments[5] = call.generation_;
    auto callee = arch::prepare_user_frame(
        activation->kernel_stack_.top(), entry);
    cap::CSpace* const source = caller->effective_binding().cspace();
    cap::CSpace* const destination = service_.cspace();
    const bool transferred = callee && source != nullptr
        && destination != nullptr
        && commit_caps(
            call.transfer_, *source, *destination, call.request_caps_,
            activation->ipc_ ? &*activation->ipc_ : nullptr,
            call.installed_caps_);
    if (!transferred) {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(call.state_ == Call::State::Committing
            && activation->state_ == Activation::State::Committing);
        call.result_ = operation::Result{MYOS_STATUS_TRANSFER_FAILED, 0};
        call.state_ = Call::State::Complete;
        activation->state_ = Activation::State::Complete;
        return false;
    }
    call.caller_frame_ = trap.frame();
    activation->generation_ = call.generation_;
    activation->depth_ = call.depth_;
    bool entered{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Open || call.cancel_pending_
            || call.state_ != Call::State::Committing
            || activation->state_ != Activation::State::Committing
            || activation->call_ != &call) {
            call.result_ = operation::Result{
                call.cancel_pending_
                    ? static_cast<myos_status_t>(call.cancel_status_)
                    : MYOS_STATUS_CLOSED,
                0};
            call.state_ = Call::State::Complete;
            activation->state_ = Activation::State::Complete;
            call.caller_frame_ = {};
        } else {
            call.state_ = Call::State::Active;
            activation->state_ = Activation::State::Active;
            entered = true;
        }
    }
    if (!entered) {
        close_installed(call);
        return false;
    }
    caller->push(activation->frame_);
    trap.redirect(*callee);
    dispatcher.refresh();
    return true;
}

auto Endpoint::reply(
    Thread& caller,
    arch::TrapContext& trap,
    sched::CpuDispatcher& dispatcher,
    isize status,
    usize value) noexcept -> libk::Expected<void, EndpointError> {
    if (dispatcher.current().thread() != &caller) {
        return libk::unexpected(EndpointError::InvalidCaller);
    }
    execution::Frame* const top = caller.active_frame();
    if (top == nullptr || top->kind() != execution::Frame::Kind::Endpoint) {
        return libk::unexpected(EndpointError::InvalidCaller);
    }
    auto& activation = *static_cast<Activation*>(top->owner());
    Call* const call = activation.call_;
    if (activation.endpoint_ != this || call == nullptr
        || &call->caller_.get() != &caller) {
        return libk::unexpected(EndpointError::InvalidCaller);
    }
    return finish_active(
               activation, trap, dispatcher, status, value, true)
        ? libk::Expected<void, EndpointError>{libk::expected()}
        : libk::Expected<void, EndpointError>{
              libk::unexpected(EndpointError::Busy)};
}

auto Endpoint::abort(
    Thread& caller,
    arch::TrapContext& trap,
    sched::CpuDispatcher& dispatcher,
    isize status) noexcept -> libk::Expected<void, EndpointError> {
    if (dispatcher.current().thread() != &caller) {
        return libk::unexpected(EndpointError::InvalidCaller);
    }
    execution::Frame* const top = caller.active_frame();
    if (top == nullptr || top->kind() != execution::Frame::Kind::Endpoint) {
        return libk::unexpected(EndpointError::InvalidCaller);
    }
    auto& activation = *static_cast<Activation*>(top->owner());
    Call* const call = activation.call_;
    if (activation.endpoint_ != this || call == nullptr
        || &call->caller_.get() != &caller) {
        return libk::unexpected(EndpointError::InvalidCaller);
    }
    // The callee detail is returned in value; the status remains a stable
    // kernel reason and cannot impersonate success or another terminal cause.
    return finish_active(
               activation,
               trap,
               dispatcher,
               MYOS_STATUS_PEER_ABORTED,
               static_cast<usize>(status),
               false)
        ? libk::Expected<void, EndpointError>{libk::expected()}
        : libk::Expected<void, EndpointError>{
              libk::unexpected(EndpointError::Busy)};
}

void Endpoint::unwind_frame(
    void* owner,
    arch::TrapContext& trap,
    sched::CpuDispatcher& dispatcher,
    isize status) noexcept {
    KASSERT(owner != nullptr);
    auto& activation = *static_cast<Activation*>(owner);
    KASSERT(activation.endpoint_ != nullptr);
    static_cast<void>(activation.endpoint_->finish_active(
        activation, trap, dispatcher, status, 0, false));
}

auto Endpoint::frame_cancel_pending(const void* owner) noexcept -> bool {
    KASSERT(owner != nullptr);
    const auto& activation = *static_cast<const Activation*>(owner);
    const Endpoint* const endpoint = activation.endpoint_;
    KASSERT(endpoint != nullptr);
    kernel::sync::IrqLockGuard guard{endpoint->lock_};
    return activation.call_ != nullptr
        && activation.call_->cancel_pending_;
}

auto Endpoint::finish_active(
    Activation& activation,
    arch::TrapContext& trap,
    sched::CpuDispatcher& dispatcher,
    isize status,
    usize value,
    bool reply) noexcept -> bool {
    Thread* const caller_thread = dispatcher.current().thread();
    Call* const call = activation.call_;
    if (call == nullptr || caller_thread == nullptr
        || caller_thread->active_frame() != &activation.frame_) {
        return false;
    }

    // Terminal ownership is claimed before any externally visible reply
    // transfer.  Once Replying wins, cancellation can no longer turn a
    // committed capability batch into an orphaned side effect.  A malformed
    // reply is therefore a terminal reply failure: the foreign activation is
    // unwound and the caller observes the error instead of resuming the
    // callee after a partially attempted reply.
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (activation.state_ != Activation::State::Active
            || call->state_ != Call::State::Active
            || (reply && call->cancel_pending_)) {
            return false;
        }
        if (!reply && call->cancel_pending_) {
            status = call->cancel_status_;
        }
        call->state_ = reply
            ? Call::State::Replying : Call::State::Canceling;
        activation.state_ = reply
            ? Activation::State::Replying : Activation::State::Canceling;
    }
    if (call->deadline_.armed()) {
        dispatcher.disarm(call->deadline_);
    }

    Transfer::Specs reply_caps{};
    usize ignored_receive_limit{};
    Buffer* const callee_buffer = activation.ipc_ ? &*activation.ipc_ : nullptr;
    Buffer* const caller_buffer = caller_thread->ipc_before(activation.frame_);
    cap::CSpace* const source = service_.cspace();
    cap::CSpace* const destination =
        caller_thread->binding_before(activation.frame_).cspace();
    Transfer::Handles reply_handles{};
    if (reply && (!snapshot_caps(
            callee_buffer, call->receive_limit_,
            reply_caps, ignored_receive_limit)
        || source == nullptr || destination == nullptr
        || !commit_caps(
            call->transfer_,
            *source, *destination, reply_caps,
            caller_buffer, reply_handles))) {
        status = MYOS_STATUS_TRANSFER_FAILED;
        value = 0;
    }

    trap.redirect(call->caller_frame_);
    trap.set_result(0, static_cast<usize>(status));
    trap.set_result(1, value);
    caller_thread->pop(activation.frame_);
    dispatcher.refresh();
    close_installed(*call);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        call->result_ = operation::Result{
            static_cast<myos_status_t>(status), value};
        call->state_ = Call::State::Complete;
        activation.state_ = Activation::State::Complete;
    }
    release_call(*call);
    return true;
}

auto Endpoint::call_complete(const Call& call) const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return call.state_ == Call::State::Ready
        || call.state_ == Call::State::Complete;
}

auto Endpoint::read_call(Call& call) noexcept -> operation::Result {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(call.state_ == Call::State::Complete);
    return call.result_;
}

void Endpoint::release_call(Call& call) noexcept {
    Activation* released{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (call.state_ == Call::State::Active) {
            return;
        }
        KASSERT(call.state_ == Call::State::Complete);
        KASSERT(!call.deadline_.armed());
        if (call.publishers_ != 0) {
            return;
        }
        call.state_ = Call::State::Reaping;
        released = call.activation_;
        if (released != nullptr) {
            KASSERT(released->call_ == &call
                && (released->state_ == Activation::State::Preparing
                    || released->state_ == Activation::State::Complete));
            released->call_ = nullptr;
            released->state_ = Activation::State::Free;
            call.activation_ = nullptr;
        }
    }

    close_installed(call);
    call.caller_ = object::ThreadHold{};
    call.caller_frame_ = {};
    Call* ready{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(call.state_ == Call::State::Reaping);
        if (released != nullptr && state_ == State::Open) {
            ready = next_call_locked(*released);
        }
    }
    if (ready != nullptr) {
        publish_ready(*ready);
    }

    bool quiescent = true;
    if (call.authority_ && call.authority_->attached()) {
        quiescent = call.authority_->detach();
    }
    if (quiescent && call.authority_
        && !call.authority_->attached() && !call.authority_->busy()) {
        finish_reap(call);
    }
}

void Endpoint::publish_ready(Call& call) noexcept {
    call.completion_.signal();
}

void Endpoint::finish_reap(Call& call) noexcept {
    bool finish{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (call.state_ != Call::State::Reaping || !call.authority_
            || call.authority_->attached() || call.authority_->busy()) {
            return;
        }
        call.authority_.reset();
        KASSERT(outstanding_ != 0);
        --outstanding_;
        reset_call_locked(call);
        finish = state_ == State::Draining && outstanding_ == 0;
    }
    if (finish) {
        try_finish_retire();
    }
}

void Endpoint::authority_quiesced(Call& call) noexcept {
    publisher_done(call);
}

void Endpoint::publisher_done(Call& call) noexcept {
    bool release{};
    bool reap{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(call.publishers_ != 0);
        --call.publishers_;
        release = call.publishers_ == 0
            && call.state_ == Call::State::Complete
            && !call.completion_.attached();
        reap = call.publishers_ == 0
            && call.state_ == Call::State::Reaping;
    }
    if (release) {
        release_call(call);
    } else if (reap) {
        finish_reap(call);
    }
}

auto Endpoint::cancel_call(Call& call) noexcept -> bool {
    bool complete{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (call.state_ == Call::State::Complete) {
            complete = true;
        } else if (call.state_ == Call::State::Committing) {
            call.cancel_pending_ = true;
            call.cancel_status_ = MYOS_STATUS_CANCELED;
            return false;
        } else if (call.state_ == Call::State::Queued
            || call.state_ == Call::State::Ready) {
            call.result_ = operation::Result{MYOS_STATUS_CANCELED, 0};
            call.state_ = Call::State::Complete;
            complete = true;
        } else {
            return false;
        }
    }
    if (complete && call.deadline_.armed()) {
        CpuLocal& cpu = current_cpu();
        KASSERT(cpu.dispatcher() != nullptr);
        cpu.dispatcher()->disarm(call.deadline_);
    }
    return complete;
}

void Endpoint::resume_call(Call& call, arch::TrapContext& trap) noexcept {
    CpuLocal& cpu = current_cpu();
    KASSERT(cpu.dispatcher() != nullptr);
    bool ready{};
    operation::Result terminal{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        ready = call.state_ == Call::State::Ready;
        if (!ready) {
            KASSERT(call.state_ == Call::State::Complete);
            terminal = call.result_;
        }
    }
    if (ready && enter(call, trap, *cpu.dispatcher())) {
        return;
    }
    if (ready) {
        kernel::sync::IrqLockGuard guard{lock_};
        if (call.state_ == Call::State::Ready) {
            call.result_ = operation::Result{MYOS_STATUS_CLOSED, 0};
            call.state_ = Call::State::Complete;
        }
        terminal = call.result_;
    }
    if (call.deadline_.armed()) {
        cpu.dispatcher()->disarm(call.deadline_);
    }
    trap.set_result(0, static_cast<usize>(
        static_cast<isize>(terminal.status)));
    trap.set_result(1, terminal.value);
}

void Endpoint::publish_cancel(Call& call) noexcept {
    KASSERT(call.activation_ != nullptr && call.cpus_ != nullptr);
    operation::Wait& wait = call.activation_->wait_;
    if (wait.attached()) {
        static_cast<void>(wait.cancel());
    }
    sched::Binding* const binding =
        call.caller_.get().binding();
    if (binding != nullptr) {
        static_cast<void>(sched::wake(*call.cpus_, *binding));
    }
}

void Endpoint::expire_call(Call& call) noexcept {
    bool ready{};
    bool cancel{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        switch (call.state_) {
        case Call::State::Preparing:
        case Call::State::Committing:
            call.cancel_pending_ = true;
            call.cancel_status_ = MYOS_STATUS_TIMED_OUT;
            call.result_ = operation::Result{MYOS_STATUS_TIMED_OUT, 0};
            break;
        case Call::State::Queued:
        case Call::State::Ready:
            call.result_ = operation::Result{MYOS_STATUS_TIMED_OUT, 0};
            call.state_ = Call::State::Complete;
            ready = call.completion_.attached();
            break;
        case Call::State::Active:
            call.cancel_pending_ = true;
            call.cancel_status_ = MYOS_STATUS_TIMED_OUT;
            cancel = true;
            break;
        case Call::State::Replying:
        case Call::State::Canceling:
        case Call::State::Reaping:
        case Call::State::Complete:
        case Call::State::Free:
            break;
        }
    }
    if (ready) {
        call.completion_.signal();
    }
    if (cancel) {
        publish_cancel(call);
    }
}

void Endpoint::invalidate_call(Call& call) noexcept {
    bool ready{};
    bool cancel{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(call.state_ != Call::State::Free
            && call.publishers_ != libk::numeric_limits<usize>::max());
        // Retain this Call through GrantWork::reset(). Otherwise a concurrent
        // terminal producer could recycle the fixed slot before the
        // invalidation callback finishes using its context pointer.
        ++call.publishers_;
        switch (call.state_) {
        case Call::State::Preparing:
            call.cancel_pending_ = true;
            call.cancel_status_ = MYOS_STATUS_DENIED;
            call.result_ = operation::Result{MYOS_STATUS_DENIED, 0};
            break;
        case Call::State::Queued:
        case Call::State::Ready:
            call.result_ = operation::Result{MYOS_STATUS_DENIED, 0};
            call.state_ = Call::State::Complete;
            ready = call.completion_.attached();
            break;
        case Call::State::Active:
            call.cancel_pending_ = true;
            call.cancel_status_ = MYOS_STATUS_DENIED;
            cancel = true;
            break;
        case Call::State::Committing:
            call.cancel_pending_ = true;
            call.cancel_status_ = MYOS_STATUS_DENIED;
            call.result_ = operation::Result{MYOS_STATUS_DENIED, 0};
            break;
        case Call::State::Replying:
        case Call::State::Canceling:
        case Call::State::Reaping:
        case Call::State::Complete:
        case Call::State::Free:
            break;
        }
    }
    if (ready) {
        call.completion_.signal();
    }
    if (cancel) {
        publish_cancel(call);
    }
}

auto Endpoint::next_call_locked(Activation& slot) noexcept -> Call* {
    kernel::sync::LockAccess::assert_held(lock_);
    KASSERT(slot.state_ == Activation::State::Free && slot.call_ == nullptr);
    Call* selected{};
    for (usize index = 0; index < call_count_; ++index) {
        Call& candidate = *call_slots_[index];
        if (candidate.state_ != Call::State::Queued) {
            continue;
        }
        if (selected == nullptr || candidate.urgency_ > selected->urgency_
            || (candidate.urgency_ == selected->urgency_
                && candidate.sequence_ < selected->sequence_)) {
            selected = &candidate;
        }
    }
    if (selected == nullptr) {
        return nullptr;
    }
    slot.call_ = selected;
    slot.state_ = Activation::State::Preparing;
    selected->activation_ = &slot;
    selected->state_ = Call::State::Ready;
    return selected;
}

void Endpoint::reset_call_locked(Call& call) noexcept {
    kernel::sync::LockAccess::assert_held(lock_);
    KASSERT(!call.caller_ && !call.caller_frame_
        && call.activation_ == nullptr && !call.completion_.attached()
        && !call.authority_ && !call.deadline_.armed());
    for (usize& argument : call.arguments_) {
        argument = 0;
    }
    call.result_ = {};
    call.generation_ = 0;
    call.sequence_ = 0;
    call.depth_ = 0;
    call.urgency_ = 0;
    call.badge_ = 0;
    call.receive_limit_ = 0;
    call.request_caps_.clear();
    KASSERT(call.installed_caps_.empty());
    call.transfer_.abort();
    call.cancel_status_ = MYOS_STATUS_CANCELED;
    call.cpus_ = nullptr;
    KASSERT(call.publishers_ == 0);
    call.cancel_pending_ = false;
    call.state_ = Call::State::Free;
}

void Endpoint::close() noexcept {
    libk::InplaceVector<Call*, MYOS_ENDPOINT_MAX_CALLS> ready{};
    libk::InplaceVector<Call*, MYOS_ENDPOINT_MAX_CALLS> cancel{};
    bool finish{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ == State::Closed || state_ == State::Draining) {
            return;
        }
        state_ = State::Draining;
        for (usize index = 0; index < call_count_; ++index) {
            Call& call = *call_slots_[index];
            if ((call.state_ == Call::State::Queued
                    || call.state_ == Call::State::Ready)
                && call.completion_.attached()) {
                call.result_ = operation::Result{MYOS_STATUS_CLOSED, 0};
                call.state_ = Call::State::Complete;
                KASSERT(ready.try_push_back(&call));
            } else if (call.state_ == Call::State::Preparing
                || call.state_ == Call::State::Committing
                || call.state_ == Call::State::Ready) {
                call.cancel_pending_ = true;
                call.cancel_status_ = MYOS_STATUS_CLOSED;
                call.result_ = operation::Result{MYOS_STATUS_CLOSED, 0};
            } else if (call.state_ == Call::State::Active) {
                call.cancel_pending_ = true;
                call.cancel_status_ = MYOS_STATUS_CLOSED;
                KASSERT(call.publishers_
                    != libk::numeric_limits<usize>::max());
                ++call.publishers_;
                KASSERT(cancel.try_push_back(&call));
            }
        }
        finish = outstanding_ == 0;
    }
    for (Call* call : ready) {
        call->completion_.signal();
    }
    for (Call* call : cancel) {
        publish_cancel(*call);
        publisher_done(*call);
    }
    if (finish) {
        try_finish_retire();
    }
}

void Endpoint::retire(object::ObjectCleanup&& cleanup) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(!cleanup_);
        cleanup_ = libk::move(cleanup);
    }
    close();
    try_finish_retire();
}

void Endpoint::try_finish_retire() noexcept {
    object::ObjectCleanup cleanup{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Draining || outstanding_ != 0 || !cleanup_) {
            return;
        }
        state_ = State::Closed;
        cleanup = libk::move(cleanup_);
    }
    while (slot_count_ != 0) {
        Activation* const slot = slots_[--slot_count_];
        slots_[slot_count_] = nullptr;
        activations_.destroy(*slot);
    }
    while (call_count_ != 0) {
        Call* const call = call_slots_[--call_count_];
        call_slots_[call_count_] = nullptr;
        calls_.destroy(*call);
    }
    code_.reset();
    service_.detach_user();
    cleanup.complete();
}

} // namespace kernel::ipc
