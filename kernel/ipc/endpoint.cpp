#include <expected>
#include <optional>
#include <ipc/endpoint.hpp>
#include <object/ref.hpp>
#include <task/thread.hpp>

#include <libk/assert.hpp>
#include <base/types.hpp>
#include <cpu/cpu.hpp>
#include <limits>
#include <libk/mem.h>
#include <libk/scope_guard.hpp>
#include <utility>
#include <mm/table.hpp>
#include <sched/sc.hpp>
#include <sched/dispatcher.hpp>
#include <sync.hpp>

namespace ipc {

const cap::GrantAttachmentOps Call::grant_ops_{
    .invalidate = &Call::revoke,
    .released = &Call::grant_done,
};

Activation::Activation(
    Endpoint& endpoint,
    resource::Charge&& stack_charge,
    mm::Stack&& kernel_stack,
    mm::View&& user_stack,
    std::optional<Buffer>&& ipc,
    StackPages&& resident,
    mm::Virt user_stack_top) noexcept
    : endpoint_(&endpoint),
      stack_charge_(std::move(stack_charge)),
      kernel_stack_(std::move(kernel_stack)),
      user_stack_(std::move(user_stack)),
      ipc_(std::move(ipc)),
      resident_(std::move(resident)),
      user_stack_top_(user_stack_top) {}

Activation::~Activation() noexcept {
    libk_assert(call_ == nullptr && previous() == nullptr);
}

Call::Call(Endpoint& endpoint) noexcept
    : endpoint_(&endpoint),
      completion_(Completion::bind<Call, &Call::release, &Call::cancel>(*this)),
      deadline_(sched::Deadline::Callback::bind<&Call::expire>(*this)) {

}

Call::~Call() noexcept {
    libk_assert(state_ == State::Free);
    libk_assert(!caller_ && !caller_frame_ && activation_ == nullptr);
    libk_assert(!grant_);
    libk_assert(!deadline_.armed());
}

void Call::release() noexcept {
    endpoint_->release_call(*this);
}

auto Call::cancel() noexcept -> bool {
    return endpoint_->cancel_call(*this);
}

void Call::expire() noexcept {
    endpoint_->expire_call(*this);
}

void Call::revoke(
    void* context,
    cap::GrantWork&& work) noexcept {
    libk_assert(context != nullptr);
    auto& call = *static_cast<Call*>(context);
    call.endpoint_->invalidate_call(call);
    // Call remains attached until its terminal transition. Grant revoke thus
    // waits for queued or active execution to unwind, while this callback work
    // only protects the invalidation publication itself.
    work.reset();
    call.endpoint_->grant_drained(call);
}

void Call::grant_done(void* context) noexcept {
    // revoke() retries reaping after GrantWork::reset() returns.
    // Ending the attachment lifetime from this callback would destroy it while
    // GrantAttachment::drop_work() is still on the stack.
    libk_assert(context != nullptr);
}

Endpoint::Endpoint(
    mm::Pmm& pmm,
    Env&& service,
    mm::View&& code,
    CodePages&& resident_code,
    EndpointConfig config) noexcept
    : service_(std::move(service)),
      code_(std::move(code)),
      resident_code_(std::move(resident_code)),
      config_(config),
      activations_(pmm, mm::Slab<Activation, false>::Quota{
          .nodes = config.capacity,
          .pages = config.capacity,
      }),
      calls_(pmm, mm::Slab<Call, false>::Quota{
          .nodes = config.call_capacity,
          .pages = config.call_capacity,
      }) {}

Endpoint::~Endpoint() noexcept {
    libk_assert(state_ == State::Constructing || state_ == State::Closed);
    libk_assert(outstanding_ == 0 && !cleanup_);
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
    resource::Sponsorship& sponsor) noexcept {
    libk_assert(!payer_);
    auto source = sponsor.payer().clone();
    libk_assert(source);
    payer_ = std::move(*source);
}

auto Endpoint::add_call() noexcept
    -> std::expected<void, EndpointError> {
    if (state_ != State::Constructing
        || call_count_ >= config_.call_capacity
        || call_count_ >= ENDPOINT_MAX_CALLS) {
        return std::unexpected(EndpointError::InvalidConfig);
    }
    auto made = calls_.create(payer_, *this);
    if (!made) {
        return std::unexpected(EndpointError::InvalidConfig);
    }
    call_slots_[call_count_++] = made.value();
    return {};
}

auto Endpoint::add_activation(
    resource::Charge&& stack_charge,
    mm::Stack&& kernel_stack,
    mm::View&& user_stack,
    std::optional<Buffer>&& ipc,
    StackPages&& resident,
    mm::Virt user_stack_top) noexcept
    -> std::expected<void, EndpointError> {
    if (state_ != State::Constructing
        || slot_count_ >= config_.capacity
        || slot_count_ >= max_activations || !user_stack.valid()
        || !mm::is_user(user_stack_top)
        || (user_stack_top.raw() & 0xfU) != 0) {
        return std::unexpected(EndpointError::InvalidConfig);
    }
    auto made = activations_.create(payer_,
        *this,
        std::move(stack_charge),
        std::move(kernel_stack),
        std::move(user_stack),
        std::move(ipc),
        std::move(resident),
        user_stack_top);
    if (!made) {
        return std::unexpected(EndpointError::InvalidConfig);
    }
    slots_[slot_count_++] = made.value();
    return {};
}

auto Endpoint::open() noexcept -> std::expected<void, EndpointError> {
    if (state_ != State::Constructing || !service_.user_bound()
        || !code_.valid() || config_.capacity == 0
        || config_.capacity > max_activations
        || config_.call_capacity < config_.capacity
        || config_.call_capacity > ENDPOINT_MAX_CALLS
        || config_.max_depth == 0
        || config_.max_depth > ENDPOINT_MAX_DEPTH
        || slot_count_ != config_.capacity
        || call_count_ != config_.call_capacity
        || !arch::valid_user_start(config_.entry)) {
        return std::unexpected(EndpointError::InvalidConfig);
    }
    state_ = State::Open;
    return {};
}

auto Endpoint::hold(Thread& thread) noexcept
    -> std::expected<object::ref<Thread>, EndpointError> {
    sched::Sc* const binding = thread.sc();
    if (binding == nullptr) {
        return std::unexpected(EndpointError::InvalidCaller);
    }
    auto reference = binding->reference();
    if (!reference) {
        return std::unexpected(EndpointError::InvalidCaller);
    }
    auto held = std::move(reference).value().as<Thread>();
    return held
        ? std::expected<object::ref<Thread>, EndpointError>{
              (std::move(held).value())}
        : std::unexpected(EndpointError::InvalidCaller);
}

auto Endpoint::depth(const Thread& thread) const noexcept -> usize {
    return thread.call_depth();
}

auto Endpoint::snapshot_caps(
    const Buffer* buffer,
    usize limit,
    cap::Batch::Specs& specs,
    usize& receive_limit) noexcept -> bool {
    specs.clear();
    receive_limit = 0;
    if (buffer == nullptr) {
        return true;
    }
    IpcCaps message{};
    if (!buffer->read(0, libk::Span<byte>{
            reinterpret_cast<byte*>(&message), sizeof(message)})
        || message.version != IPC_CAPS_VERSION
        || message.flags != IPC_CAPS_FLAGS_NONE
        || message.reserved != 0
        || message.send_count > IPC_MAX_CAPS
        || message.receive_limit > IPC_MAX_CAPS
        || message.send_count > limit || message.receive_limit > limit) {
        return false;
    }
    for (usize index = 0; index < message.send_count; ++index) {
        const CapXfer& wire = message.send[index];
        const auto rights = cap::Rights::parse(wire.rights, RIGHT_MASK);
        cap::XferOp kind{};
        switch (wire.operation) {
        case CAP_COPY:
            kind = cap::XferOp::Copy;
            break;
        case CAP_MOVE:
            kind = cap::XferOp::Move;
            break;
        case CAP_DELEGATE:
            kind = cap::XferOp::Derive;
            break;
        default:
            return false;
        }
        const cap::Handle source = cap::Handle::from_raw(wire.source);
        if (wire.flags != 0 || !source || !rights
            || (kind == cap::XferOp::Move && !rights->empty())
            || !specs.try_push_back(cap::XferSpec{
                source, *rights, kind})) {
            return false;
        }
    }
    receive_limit = message.receive_limit;
    return true;
}

auto Endpoint::commit_caps(
    cap::Batch& transfer,
    cap::CSpace& source,
    cap::CSpace& destination,
    const cap::Batch::Specs& specs,
    Buffer* receiver,
    cap::Batch::Handles& installed) noexcept -> bool {
    installed.clear();
    if (specs.empty()) {
        if (receiver == nullptr) {
            return true;
        }
        auto access = receiver->access();
        if (!access) {
            return false;
        }
        IpcCaps projection{};
        projection.version = IPC_CAPS_VERSION;
        return access.value().write(0, libk::Span<const byte>{
            reinterpret_cast<const byte*>(&projection), sizeof(projection)});
    }
    if (receiver == nullptr) {
        return false;
    }
    if (!cap::Batch::prepare(transfer, source, destination, specs)) {
        return false;
    }
    auto access = receiver->access();
    if (!access) {
        transfer.abort();
        return false;
    }
    IpcCaps projection{};
    projection.version = IPC_CAPS_VERSION;
    const cap::Batch::Handles reserved = transfer.handles();
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
        projection.version = IPC_CAPS_VERSION;
        static_cast<void>(access.value().write(0, libk::Span<const byte>{
            reinterpret_cast<const byte*>(&projection), sizeof(projection)}));
        return false;
    }
    installed = std::move(committed).value();
    projection.received_count = static_cast<uint32_t>(installed.size());
    libk_assert(access.value().write(0, libk::Span<const byte>{
        reinterpret_cast<const byte*>(&projection), sizeof(projection)}));
    return true;
}

void Endpoint::close_installed(Call& call) noexcept {
    cap::CSpace* const service = service_.cspace();
    libk_assert(service != nullptr);
    for (const cap::Handle handle : call.installed_caps_) {
        static_cast<void>(service->close(handle));
    }
    call.installed_caps_.clear();
}

auto Endpoint::call(
    cap::Resolved<Endpoint>&& view,
    Thread& caller,
    arch::TrapCtx& trap,
    sched::Dispatcher& dispatcher,
    Cpus& cpus,
    const usize (&arguments)[3],
    std::optional<time::Instant> deadline) noexcept
    -> std::expected<void, EndpointError> {
    if (&view.object() != this
        || dispatcher.current() != &caller) {
        return std::unexpected(EndpointError::InvalidCaller);
    }
    const cap::View effective = view.view();
    const auto* const limit =
        std::get_if<cap::EpLimit>(&effective.data);
    if (limit == nullptr || !limit->callable()
        || !effective.rights.contains(cap::Right::Call)) {
        return std::unexpected(EndpointError::Denied);
    }
    cap::Batch::Specs request_caps{};
    usize receive_limit{};
    if (!snapshot_caps(
            caller.ipc_buffer(), limit->cap_limit,
            request_caps, receive_limit)) {
        return std::unexpected(EndpointError::TransferFailed);
    }
    const usize call_depth = depth(caller);
    if (call_depth >= config_.max_depth) {
        return std::unexpected(EndpointError::DepthExceeded);
    }
    if (dispatcher.remaining_budget() < config_.budget_floor) {
        return std::unexpected(EndpointError::BudgetTooLow);
    }
    for (ipc::Activation* frame = caller.activation(); frame != nullptr;
         frame = frame->previous()) {
        if (&frame->endpoint()
                == this) {
            return std::unexpected(EndpointError::DepthExceeded);
        }
    }
    auto caller_hold = hold(caller);
    if (!caller_hold) {
        return std::unexpected(caller_hold.error());
    }

    Call* call{};
    Activation* activation{};
    {
        sync::Lock guard{lock_};
        if (state_ != State::Open) {
            return std::unexpected(EndpointError::Closed);
        }
        for (usize index = 0; index < call_count_; ++index) {
            if (call_slots_[index]->state_ == Call::State::Free) {
                call = call_slots_[index];
                break;
            }
        }
        if (call == nullptr) {
            return std::unexpected(EndpointError::QueueFull);
        }
        for (usize index = 0; index < slot_count_; ++index) {
            if (slots_[index]->call_ == nullptr) {
                activation = slots_[index];
                break;
            }
        }
        if (generation_ == std::numeric_limits<u64>::max()) {
            return std::unexpected(EndpointError::GenerationExhausted);
        }
        const u64 generation = ++generation_;
        call->state_ = Call::State::Preparing;
        call->generation_ = generation;

        const usize caller_urgency = dispatcher.current_urgency().value();
        const usize ceiling = config_.urgency_ceiling.value();
        call->urgency_ = caller_urgency < ceiling
            ? caller_urgency : ceiling;
        call->badge_ = limit->badge;
        call->receive_limit_ = receive_limit;
        call->request_caps_ = std::move(request_caps);
        call->cpus_ = &cpus;
        libk_assert(call->publishers_ == 0);
        call->publishers_ = 1; // admission producer lease
        if (activation != nullptr) {
            activation->call_ = call;
            call->activation_ = activation;
        }
        ++outstanding_;
    }

    call->caller_ = std::move(caller_hold).value();
    for (usize index = 0; index < 3; ++index) {
        call->arguments_[index] = arguments[index];
    }
    auto& grant = call->grant_.emplace(call, Call::grant_ops_);
    if (!view.attach(grant)) {
        {
            sync::Lock guard{lock_};
            call->result_ = WaitResult{STATUS_DENIED, 0};
            call->state_ = Call::State::Complete;
        }
        publisher_done(*call);
        return std::unexpected(EndpointError::Closed);
    }
    if (deadline && !dispatcher.arm(call->deadline_, *deadline)) {
        {
            sync::Lock guard{lock_};
            call->result_ = WaitResult{STATUS_INTERNAL, 0};
            call->state_ = Call::State::Complete;
        }
        publisher_done(*call);
        return std::unexpected(EndpointError::Busy);
    }

    if (activation == nullptr) {
        sched::Sc* const binding = caller.sc();
        if (binding == nullptr
            || !caller.current_wait().begin(
                call->completion_, cpus, *binding)) {
            {
                sync::Lock guard{lock_};
                call->result_ = WaitResult{
                    STATUS_WOULD_BLOCK, 0};
                call->state_ = Call::State::Complete;
            }
            if (call->deadline_.armed()) {
                dispatcher.disarm(call->deadline_);
            }
            publisher_done(*call);
            return std::unexpected(EndpointError::Busy);
        }
        bool ready{};
        {
            sync::Lock guard{lock_};
            if (state_ != State::Open || call->cancel_pending_) {
                if (!call->cancel_pending_) {
                    call->result_ = WaitResult{
                        STATUS_CLOSED, 0};
                }
                call->state_ = Call::State::Complete;
                ready = true;
            } else {
                call->state_ = Call::State::Queued;
                for (usize index = 0; index < slot_count_; ++index) {
                    if (slots_[index]->call_ == nullptr) {
                        ready = next_call_locked(*slots_[index]) == call;
                        break;
                    }
                }
            }
        }
        // Keep the caller's publisher until its ordinary stack resumes.
        // Grant revocation must see operations=0 while this call is queued.
        view.reset();
        if (ready) call->completion_.signal();
        caller.block();
        if (caller.stop_requested()) static_cast<void>(cancel_call(*call));
        if (!enter(*call, trap, dispatcher)) {
            WaitResult result{};
            {
                sync::Lock guard{lock_};
                if (call->state_ == Call::State::Ready) {
                    call->state_ = Call::State::Complete;
                    call->result_ = {STATUS_CLOSED, 0};
                }
                libk_assert(call->state_ == Call::State::Complete);
                result = call->result_;
            }
            if (call->deadline_.armed()) dispatcher.disarm(call->deadline_);
            trap.set_result(0, static_cast<usize>(static_cast<isize>(result.status)));
            trap.set_result(1, result.value);
        }
        publisher_done(*call);
        return {};
    }

    {
        sync::Lock guard{lock_};
        if (state_ != State::Open || call->cancel_pending_) {
            if (!call->cancel_pending_) {
                call->result_ = WaitResult{STATUS_CLOSED, 0};
            }
            call->state_ = Call::State::Complete;

        } else {
            call->state_ = Call::State::Ready;
        }
    }

    if (!enter(*call, trap, dispatcher)) {
        EndpointError error{EndpointError::Closed};
        {
            sync::Lock guard{lock_};
            if (call->state_ != Call::State::Complete) {
                call->result_ = WaitResult{STATUS_CLOSED, 0};
                call->state_ = Call::State::Complete;

            }
            if (call->result_.status == STATUS_TRANSFER_FAILED) {
                error = EndpointError::TransferFailed;
            }
        }
        if (call->deadline_.armed()) {
            dispatcher.disarm(call->deadline_);
        }
        publisher_done(*call);
        return std::unexpected(error);
    }
    publisher_done(*call);
    return {};
}

auto Endpoint::enter(
    Call& call,
    arch::TrapCtx& trap,
    sched::Dispatcher& dispatcher) noexcept -> bool {
    Activation* const activation = call.activation_;
    Thread* const caller = dispatcher.current();
    if (activation == nullptr || caller == nullptr
        || &call.caller_.get() != caller) {
        return false;
    }
    {
        sync::Lock guard{lock_};
        if (state_ != State::Open || call.cancel_pending_
            || call.state_ != Call::State::Ready
            || activation->call_ != &call) {
            return false;
        }
        call.state_ = Call::State::Committing;

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
    cap::CSpace* const source = caller->env().cspace();
    cap::CSpace* const destination = service_.cspace();
    const bool transferred = callee && source != nullptr
        && destination != nullptr
        && commit_caps(
            call.transfer_, *source, *destination, call.request_caps_,
            activation->ipc_ ? &*activation->ipc_ : nullptr,
            call.installed_caps_);
    if (!transferred) {
        sync::Lock guard{lock_};
        libk_assert(call.state_ == Call::State::Committing && activation->call_ == &call);
        call.result_ = WaitResult{STATUS_TRANSFER_FAILED, 0};
        call.state_ = Call::State::Complete;

        return false;
    }
    call.caller_frame_ = trap.frame();

    bool entered{};
    {
        sync::Lock guard{lock_};
        if (state_ != State::Open || call.cancel_pending_
            || call.state_ != Call::State::Committing
            || activation->call_ != &call) {
            call.result_ = WaitResult{
                call.cancel_pending_
                    ? static_cast<status_t>(call.cancel_status_)
                    : STATUS_CLOSED,
                0};
            call.state_ = Call::State::Complete;

            call.caller_frame_ = {};
        } else {
            call.state_ = Call::State::Active;

            ++call.publishers_; // Actual stack use ends at the return handoff.
            entered = true;
        }
    }
    if (!entered) {
        close_installed(call);
        return false;
    }
    caller->push(*activation);
    trap.redirect(*callee);
    dispatcher.refresh();
    return true;
}

auto Endpoint::reply(
    Thread& caller,
    arch::TrapCtx& trap,
    sched::Dispatcher& dispatcher,
    isize status,
    usize value) noexcept -> std::expected<void, EndpointError> {
    if (dispatcher.current() != &caller) {
        return std::unexpected(EndpointError::InvalidCaller);
    }
    ipc::Activation* const top = caller.activation();
    if (top == nullptr) {
        return std::unexpected(EndpointError::InvalidCaller);
    }
    auto& activation = *top;
    Call* const call = activation.call_;
    if (activation.endpoint_ != this || call == nullptr
        || &call->caller_.get() != &caller) {
        return std::unexpected(EndpointError::InvalidCaller);
    }
    return finish_active(
               activation, trap, dispatcher, status, value, true)
        ? std::expected<void, EndpointError>{}
        : std::expected<void, EndpointError>{
              std::unexpected(EndpointError::Busy)};
}

auto Endpoint::abort(
    Thread& caller,
    arch::TrapCtx& trap,
    sched::Dispatcher& dispatcher,
    isize status) noexcept -> std::expected<void, EndpointError> {
    if (dispatcher.current() != &caller) {
        return std::unexpected(EndpointError::InvalidCaller);
    }
    ipc::Activation* const top = caller.activation();
    if (top == nullptr) {
        return std::unexpected(EndpointError::InvalidCaller);
    }
    auto& activation = *top;
    Call* const call = activation.call_;
    if (activation.endpoint_ != this || call == nullptr
        || &call->caller_.get() != &caller) {
        return std::unexpected(EndpointError::InvalidCaller);
    }
    // The callee detail is returned in value; the status remains a stable
    // kernel reason and cannot impersonate success or another terminal cause.
    return finish_active(
               activation,
               trap,
               dispatcher,
               STATUS_PEER_ABORTED,
               static_cast<usize>(status),
               false)
        ? std::expected<void, EndpointError>{}
        : std::expected<void, EndpointError>{
              std::unexpected(EndpointError::Busy)};
}

auto Activation::env() noexcept -> Env& {
    return endpoint_->service_;
}

void Activation::release() noexcept {
    endpoint_->publisher_done(*call_);
}

void Activation::unwind(
    arch::TrapCtx& trap,
    sched::Dispatcher& dispatcher,
    isize status) noexcept {
    static_cast<void>(endpoint_->finish_active(*this, trap, dispatcher, status, 0, false));
}

auto Activation::cancel_pending() const noexcept -> bool {
    sync::Lock guard{endpoint_->lock_};
    return call_ != nullptr && call_->cancel_pending_;
}

auto Endpoint::finish_active(
    Activation& activation,
    arch::TrapCtx& trap,
    sched::Dispatcher& dispatcher,
    isize status,
    usize value,
    bool reply) noexcept -> bool {
    Thread* const caller_thread = dispatcher.current();
    Call* const call = activation.call_;
    if (call == nullptr || caller_thread == nullptr
        || caller_thread->activation() != &activation) {
        return false;
    }

    // Terminal ownership is claimed before any externally visible reply
    // transfer.  Once Replying wins, cancellation can no longer turn a
    // committed capability batch into an orphaned side effect.  A malformed
    // reply is therefore a terminal reply failure: the foreign activation is
    // unwound and the caller observes the error instead of resuming the
    // callee after a partially attempted reply.
    {
        sync::Lock guard{lock_};
        if (call->state_ != Call::State::Active
            || (reply && call->cancel_pending_)) {
            return false;
        }
        if (!reply && call->cancel_pending_) {
            status = call->cancel_status_;
        }
        call->state_ = reply
            ? Call::State::Replying : Call::State::Canceling;
    }
    if (call->deadline_.armed()) {
        dispatcher.disarm(call->deadline_);
    }

    cap::Batch::Specs reply_caps{};
    usize ignored_receive_limit{};
    Buffer* const callee_buffer = activation.ipc_ ? &*activation.ipc_ : nullptr;
    Buffer* const caller_buffer = caller_thread->ipc_before(activation);
    cap::CSpace* const source = service_.cspace();
    cap::CSpace* const destination =
        caller_thread->env_before(activation).cspace();
    cap::Batch::Handles reply_handles{};
    if (reply && (!snapshot_caps(
            callee_buffer, call->receive_limit_,
            reply_caps, ignored_receive_limit)
        || source == nullptr || destination == nullptr
        || !commit_caps(
            call->transfer_,
            *source, *destination, reply_caps,
            caller_buffer, reply_handles))) {
        status = STATUS_TRANSFER_FAILED;
        value = 0;
    }

    trap.redirect(call->caller_frame_);
    trap.set_result(0, static_cast<usize>(status));
    trap.set_result(1, value);
    caller_thread->pop(activation);
    dispatcher.refresh();
    close_installed(*call);
    {
        sync::Lock guard{lock_};
        call->result_ = WaitResult{
            static_cast<status_t>(status), value};
        call->state_ = Call::State::Complete;

    }
    return true;
}

void Endpoint::release_call(Call& call) noexcept {
    Activation* released{};
    {
        sync::Lock guard{lock_};
        if (call.state_ == Call::State::Active || call.state_ == Call::State::Ready) {
            return;
        }
        libk_assert(call.state_ == Call::State::Complete);
        if (call.publishers_ != 0) return;
        libk_assert(!call.deadline_.armed());
        call.state_ = Call::State::Reaping;
        released = call.activation_;
        if (released != nullptr) {
            libk_assert(released->call_ == &call && released->previous_ == nullptr);
            released->call_ = nullptr;
            call.activation_ = nullptr;
        }
    }

    close_installed(call);
    call.caller_ = object::ref<Thread>{};
    call.caller_frame_ = {};
    Call* ready{};
    {
        sync::Lock guard{lock_};
        libk_assert(call.state_ == Call::State::Reaping);
        if (released != nullptr && state_ == State::Open) {
            ready = next_call_locked(*released);
        }
    }
    if (ready != nullptr) {
        publish_ready(*ready);
    }

    bool quiescent = true;
    if (call.grant_ && call.grant_->attached()) {
        quiescent = call.grant_->detach();
    }
    if (quiescent && call.grant_
        && !call.grant_->attached() && !call.grant_->busy()) {
        finish_reap(call);
    }
}

void Endpoint::publish_ready(Call& call) noexcept {
    call.completion_.signal();
}

void Endpoint::finish_reap(Call& call) noexcept {
    bool finish{};
    {
        sync::Lock guard{lock_};
        if (call.state_ != Call::State::Reaping || !call.grant_
            || call.grant_->attached() || call.grant_->busy()) {
            return;
        }
        call.grant_.reset();
        libk_assert(outstanding_ != 0);
        --outstanding_;
        reset_call_locked(call);
        finish = state_ == State::Draining && outstanding_ == 0;
    }
    if (finish) {
        try_finish_retire();
    }
}

void Endpoint::grant_drained(Call& call) noexcept {
    publisher_done(call);
}

void Endpoint::publisher_done(Call& call) noexcept {
    bool release{};
    bool reap{};
    {
        sync::Lock guard{lock_};
        libk_assert(call.publishers_ != 0);
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
        sync::Lock guard{lock_};
        if (call.state_ == Call::State::Complete) {
            complete = true;
        } else if (call.state_ == Call::State::Committing) {
            call.cancel_pending_ = true;
            call.cancel_status_ = STATUS_CANCELED;
            return false;
        } else if (call.state_ == Call::State::Queued
            || call.state_ == Call::State::Ready) {
            call.result_ = WaitResult{STATUS_CANCELED, 0};
            call.state_ = Call::State::Complete;
            complete = true;
        } else {
            return false;
        }
    }
    if (complete && call.deadline_.armed()) {
        Cpu& cpu = current_cpu();
        cpu.dispatcher().disarm(call.deadline_);
    }
    return complete;
}

void Endpoint::publish_cancel(Call& call) noexcept {
    libk_assert(call.activation_ != nullptr && call.cpus_ != nullptr);
    Wait& wait = call.activation_->wait_;
    // Cancellation itself is the atomic edge owner.  A separate
    // attached() precheck would reopen the finish/cancel TOCTOU window; a
    // false result is the normal producer/finisher race outcome.
    static_cast<void>(wait.cancel());
    sched::Sc* const binding =
        call.caller_.get().sc();
    if (binding != nullptr) {
        static_cast<void>(sched::wake(*call.cpus_, *binding));
    }
}

void Endpoint::expire_call(Call& call) noexcept {
    bool ready{};
    bool cancel{};
    {
        sync::Lock guard{lock_};
        switch (call.state_) {
        case Call::State::Preparing:
        case Call::State::Committing:
            call.cancel_pending_ = true;
            call.cancel_status_ = STATUS_TIMED_OUT;
            call.result_ = WaitResult{STATUS_TIMED_OUT, 0};
            break;
        case Call::State::Queued:
        case Call::State::Ready:
            call.result_ = WaitResult{STATUS_TIMED_OUT, 0};
            call.state_ = Call::State::Complete;
            ready = call.completion_.attached();
            break;
        case Call::State::Active:
            call.cancel_pending_ = true;
            call.cancel_status_ = STATUS_TIMED_OUT;
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
        sync::Lock guard{lock_};
        libk_assert(call.state_ != Call::State::Free
            && call.publishers_ != std::numeric_limits<usize>::max());
        // Retain this Call through GrantWork::reset(). Otherwise a concurrent
        // terminal producer could recycle the fixed slot before the
        // invalidation callback finishes using its context pointer.
        ++call.publishers_;
        switch (call.state_) {
        case Call::State::Preparing:
            call.cancel_pending_ = true;
            call.cancel_status_ = STATUS_DENIED;
            call.result_ = WaitResult{STATUS_DENIED, 0};
            break;
        case Call::State::Queued:
        case Call::State::Ready:
            call.result_ = WaitResult{STATUS_DENIED, 0};
            call.state_ = Call::State::Complete;
            ready = call.completion_.attached();
            break;
        case Call::State::Active:
            call.cancel_pending_ = true;
            call.cancel_status_ = STATUS_DENIED;
            cancel = true;
            break;
        case Call::State::Committing:
            call.cancel_pending_ = true;
            call.cancel_status_ = STATUS_DENIED;
            call.result_ = WaitResult{STATUS_DENIED, 0};
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
    libk_assert(lock_.held());
    libk_assert(slot.call_ == nullptr);
    Call* selected{};
    for (usize index = 0; index < call_count_; ++index) {
        Call& candidate = *call_slots_[index];
        if (candidate.state_ != Call::State::Queued) {
            continue;
        }
        if (selected == nullptr || candidate.urgency_ > selected->urgency_
            || (candidate.urgency_ == selected->urgency_
                && candidate.generation_ < selected->generation_)) {
            selected = &candidate;
        }
    }
    if (selected == nullptr) {
        return nullptr;
    }
    slot.call_ = selected;
    selected->activation_ = &slot;
    selected->state_ = Call::State::Ready;
    return selected;
}

void Endpoint::reset_call_locked(Call& call) noexcept {
    libk_assert(lock_.held());
    libk_assert(!call.caller_ && !call.caller_frame_
        && call.activation_ == nullptr && !call.completion_.attached()
        && !call.grant_ && !call.deadline_.armed());
    for (usize& argument : call.arguments_) {
        argument = 0;
    }
    call.result_ = {};
    call.generation_ = 0;

    call.urgency_ = 0;
    call.badge_ = 0;
    call.receive_limit_ = 0;
    call.request_caps_.clear();
    libk_assert(call.installed_caps_.empty());
    call.transfer_.abort();
    call.cancel_status_ = STATUS_CANCELED;
    call.cpus_ = nullptr;
    libk_assert(call.publishers_ == 0);
    call.cancel_pending_ = false;
    call.state_ = Call::State::Free;
}

void Endpoint::close() noexcept {
    libk::InplaceVector<Call*, ENDPOINT_MAX_CALLS> ready{};
    libk::InplaceVector<Call*, ENDPOINT_MAX_CALLS> cancel{};
    bool finish{};
    {
        sync::Lock guard{lock_};
        if (state_ == State::Closed || state_ == State::Draining) {
            return;
        }
        state_ = State::Draining;
        for (usize index = 0; index < call_count_; ++index) {
            Call& call = *call_slots_[index];
            if ((call.state_ == Call::State::Queued
                    || call.state_ == Call::State::Ready)
                && call.completion_.attached()) {
                call.result_ = WaitResult{STATUS_CLOSED, 0};
                call.state_ = Call::State::Complete;
                libk_assert(ready.try_push_back(&call));
            } else if (call.state_ == Call::State::Preparing
                || call.state_ == Call::State::Committing
                || call.state_ == Call::State::Ready) {
                call.cancel_pending_ = true;
                call.cancel_status_ = STATUS_CLOSED;
                call.result_ = WaitResult{STATUS_CLOSED, 0};
            } else if (call.state_ == Call::State::Active) {
                call.cancel_pending_ = true;
                call.cancel_status_ = STATUS_CLOSED;
                libk_assert(call.publishers_
                    != std::numeric_limits<usize>::max());
                ++call.publishers_;
                libk_assert(cancel.try_push_back(&call));
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

void Endpoint::retire(object::cleanup&& cleanup) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(!cleanup_);
        cleanup_ = std::move(cleanup);
    }
    close();
    try_finish_retire();
}

void Endpoint::try_finish_retire() noexcept {
    object::cleanup cleanup{};
    {
        sync::Lock guard{lock_};
        if (state_ != State::Draining || outstanding_ != 0 || !cleanup_) {
            return;
        }
        state_ = State::Closed;
        cleanup = std::move(cleanup_);
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

} // namespace ipc
