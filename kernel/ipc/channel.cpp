#include <object/notification_pool.hpp>
#include <object/channel_pool.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <ipc/buffer.hpp>
#include <libk/limits.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/scope_guard.hpp>
#include <libk/utility.hpp>
#include <sync/irq_lock_guard.hpp>
#include <thread/thread.hpp>
#include <sched/dispatcher.hpp>
#include <uapi/status.h>

namespace kernel::ipc {

namespace {

// Relation handles reserve the low byte for the fixed relation slot.  Keep
// generation exhaustion expressed in the actual encoded width; allowing a
// full-width u64 generation would silently truncate when the handle is
// encoded in usize.
constexpr usize kRelationIndexBits = 8;
constexpr usize kRelationIndexMask =
    (usize{1} << kRelationIndexBits) - usize{1};
constexpr u64 kRelationGenerationMax = static_cast<u64>(
    libk::numeric_limits<usize>::max() >> kRelationIndexBits);

[[nodiscard]] auto cap_error(cap::CSpaceError error) noexcept
    -> ChannelError {
    switch (error) {
    case cap::CSpaceError::Denied:
    case cap::CSpaceError::Amplification:
        return ChannelError::Denied;
    case cap::CSpaceError::OutOfMemory:
    case cap::CSpaceError::SlotQuota:
    case cap::CSpaceError::PageQuota:
    case cap::CSpaceError::ResourceExhausted:
        return ChannelError::ResourceExhausted;
    case cap::CSpaceError::InvalidHandle:
    case cap::CSpaceError::WrongKind:
    case cap::CSpaceError::InvalidDescriptor:
    case cap::CSpaceError::GrantUnavailable:
        return ChannelError::Invalid;
    case cap::CSpaceError::InvalidState:
    case cap::CSpaceError::Contended:
    case cap::CSpaceError::GenerationExhausted:
        return ChannelError::Busy;
    }
    return ChannelError::Invalid;
}

[[nodiscard]] auto wait_status(ChannelError error) noexcept -> myos_status_t {
    switch (error) {
    case ChannelError::Closed:
        return MYOS_STATUS_CLOSED;
    case ChannelError::PeerClosed:
        return MYOS_STATUS_PEER_CLOSED;
    case ChannelError::WouldBlock:
        return MYOS_STATUS_WOULD_BLOCK;
    case ChannelError::Denied:
        return MYOS_STATUS_DENIED;
    case ChannelError::Busy:
        return MYOS_STATUS_BUSY;
    case ChannelError::ResourceExhausted:
        return MYOS_STATUS_NO_MEMORY;
    case ChannelError::TransferFailed:
        return MYOS_STATUS_TRANSFER_FAILED;
    case ChannelError::InvalidRelation:
    case ChannelError::Invalid:
        return MYOS_STATUS_BAD_ARGS;
    case ChannelError::GenerationExhausted:
        return MYOS_STATUS_BUSY;
    }
    return MYOS_STATUS_INTERNAL;
}

} // namespace

const cap::GrantAttachmentOps Channel::channel_ops_{
    .invalidate = &Channel::invalidate,
    .released = &Channel::released,
};

const cap::GrantAttachmentOps Channel::notification_ops_{
    .invalidate = &Channel::invalidate,
    .released = &Channel::released,
};

const cap::GrantAttachmentOps Channel::side_ops_{
    .invalidate = &Channel::invalidate_side,
    .released = &Channel::release_side,
};

const cap::GrantAttachmentOps Channel::waiter_ops_{
    .invalidate = &Channel::invalidate_waiter,
    .released = &Channel::release_waiter,
};

Channel::AuthLink::AuthLink(Relation& owner, bool is_channel) noexcept
    : relation(&owner),
      channel(is_channel),
      attachment(
          this,
          is_channel ? Channel::channel_ops_ : Channel::notification_ops_) {}

Channel::SideLink::SideLink(Channel& channel, ChannelSide value) noexcept
    : owner(&channel),
      side(value),
      attachment(this, Channel::side_ops_) {}

Channel::Relation::Relation() noexcept
    : source(NotificationSource::bind<Relation, &Relation::closed>(*this)),
      channel_link(*this, true),
      notification_link(*this, false) {}

Channel::Relation::~Relation() noexcept {
    KASSERT(state == State::Idle);
    KASSERT(!source.attached());
    KASSERT(!channel_link.attachment.attached()
        && !notification_link.attachment.attached());
    KASSERT(!channel_link.attachment.busy()
        && !notification_link.attachment.busy());
    KASSERT(!channel_link.work && !notification_link.work);
    KASSERT(!notification);
}

ChannelWait::ChannelWait(Channel& channel) noexcept
    : owner(&channel),
      grant_attachment(this, Channel::waiter_ops_),
      completion(kernel::operation::Completion::bind_resume<
          ChannelWait,
          &ChannelWait::complete,
          &ChannelWait::read,
          &ChannelWait::release,
          &ChannelWait::cancel,
          &ChannelWait::resume>(*this)) {}

ChannelWait::~ChannelWait() noexcept {
    KASSERT(state == State::Done && references == 0 && !hook.is_linked());
    KASSERT(!channel_ref);
    KASSERT(!grant_attachment.attached() && !grant_attachment.busy());
    KASSERT(!completion.attached());
}

auto ChannelWait::complete() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{owner->lock_};
    return references == 1 && (state == State::Ready || state == State::Done);
}

auto ChannelWait::read() noexcept -> kernel::operation::Result {
    kernel::sync::IrqLockGuard guard{owner->lock_};
    KASSERT(references == 1 && (state == State::Ready || state == State::Done));
    return result;
}

void ChannelWait::release() noexcept {
    owner->finish_waiter(*this);
}

auto ChannelWait::cancel() noexcept -> bool {
    {
        kernel::sync::IrqLockGuard guard{owner->lock_};
        if (state != State::Awaiting && state != State::Armed) return false;
        result = kernel::operation::Result{MYOS_STATUS_CANCELED, 0};
        state = State::Done;
        ++references;
    }
    owner->detach_waiter_authority(*this);
    owner->drop_waiter(*this);
    // Publication, including any authority callback already in flight, owns
    // completion. Stop retains the continuation until that publication drains.
    return false;
}

void ChannelWait::resume(arch::TrapContext& trap) noexcept {
    static_cast<void>(owner->resume_waiter(*this, trap));
}

void Channel::Relation::closed() noexcept {
    notification_closed();
}

void Channel::Relation::notification_closed() noexcept {
    if (owner != nullptr) {
        owner->detach_relation(*this);
    }
}

auto Channel::storage_bytes(ChannelConfig config) noexcept -> libk::optional<usize> {
    const auto cells = libk::checked_multiply(config.queue_capacity, usize{2});
    if (!cells) return libk::nullopt;
    const auto pages = libk::checked_add(mm::NodePool<Message>::quota_for(*cells).pages,
        mm::NodePool<Relation>::quota_for(config.relation_capacity).pages);
    return pages ? libk::checked_multiply(*pages, mm::page_size) : libk::nullopt;
}

Channel::Channel(kernel::mm::Pmm& pmm, ChannelConfig config) noexcept
    : config_(config),
      messages_(pmm, mm::NodePool<Message>::quota_for(config.queue_capacity * 2)),
      side_links_{
          SideLink{*this, ChannelSide::A},
          SideLink{*this, ChannelSide::B}},
      relation_pool_(pmm, mm::NodePool<Relation>::quota_for(config.relation_capacity)) {}

Channel::~Channel() noexcept {
    KASSERT(!cleanup_ && waiter_count_ == 0);
    clear_queues();
    while (!free_messages_.empty()) {
        auto& message = free_messages_.pop_front();
        messages_.destroy(message);
    }
    KASSERT(messages_.live_count() == 0);
    while (!relations_.empty()) {
        auto& relation = relations_.pop_front();
        KASSERT(relation.state == Relation::State::Idle);
        relation_pool_.destroy(relation);
    }
    for (SideLink& link : side_links_) {
        KASSERT(!link.attachment.attached() && !link.attachment.busy());
        KASSERT(!link.work);
    }
    KASSERT(!opened_ || closing_);
}

void Channel::bind_sponsor(
    kernel::resource::Sponsorship& sponsor) noexcept {
    messages_.bind_sponsor(sponsor);
    relation_pool_.bind_sponsor(sponsor);
}

auto Channel::open() noexcept -> libk::Expected<void, ChannelError> {
    if (config_.queue_capacity == 0
        || config_.queue_capacity > MYOS_CHANNEL_MAX_QUEUE
        || config_.max_words == 0
        || config_.max_words > MYOS_CHANNEL_MAX_WORDS
        || config_.max_caps > MYOS_CHANNEL_MAX_CAPS
        || config_.relation_capacity > MYOS_CHANNEL_MAX_RELATIONS) {
        return libk::unexpected(ChannelError::Invalid);
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (opened_) {
            return libk::unexpected(ChannelError::Busy);
        }
    }

    // Creation pays for every cell and readiness slot. Operational paths only
    // move intrusive links and never grow backing storage.
    const usize required = config_.queue_capacity * 2;
    while (free_messages_.size() < required) {
        auto made = messages_.create();
        if (!made) return libk::unexpected(ChannelError::ResourceExhausted);
        free_messages_.push_back(*made.value().object);
    }
    while (relations_.size() < config_.relation_capacity) {
        auto made = relation_pool_.create();
        if (!made) return libk::unexpected(ChannelError::ResourceExhausted);
        auto& relation = *made.value().object;
        relation.owner = this;
        relation.index = relations_.size();
        relations_.push_back(relation);
    }

    kernel::sync::IrqLockGuard guard{lock_};
    if (opened_) {
        return libk::unexpected(ChannelError::Busy);
    }
    opened_ = true;
    return libk::expected();
}

auto Channel::bind_side_root(
    cap::GrantRef& root,
    ChannelSide value) noexcept -> bool {
    if ((value != ChannelSide::A && value != ChannelSide::B)
        || !root) {
        return false;
    }
    auto target = root.acquire();
    if (!target) {
        return false;
    }
    auto object = target.value().clone_target();
    if (!object) {
        return false;
    }
    auto channel = object.value().pin<Channel>();
    if (!channel || &channel.value().get() != this) {
        return false;
    }
    SideLink& link = side_links_[side_index(value)];
    const auto attached = target.value().attach(link.attachment);
    return static_cast<bool>(attached);
}

auto Channel::side(ChannelSide value) noexcept -> Side& {
    KASSERT(value == ChannelSide::A || value == ChannelSide::B);
    return sides_[side_index(value)];
}

auto Channel::side(ChannelSide value) const noexcept -> const Side& {
    KASSERT(value == ChannelSide::A || value == ChannelSide::B);
    return sides_[side_index(value)];
}

auto Channel::side_index(ChannelSide value) const noexcept -> usize {
    return value == ChannelSide::B ? 1 : 0;
}

auto Channel::peer(ChannelSide value) const noexcept -> ChannelSide {
    return value == ChannelSide::A ? ChannelSide::B : ChannelSide::A;
}

auto Channel::authority_side(
    const cap::Resolved<Channel>& authority,
    cap::Right right) const noexcept -> libk::optional<ChannelSide> {
    if (&authority.object() != this || !authority.rights().contains(right)) {
        return libk::nullopt;
    }
    const auto effective = authority.authority();
    const auto* const data = libk::get_if<cap::ChannelAuthority>(
        &effective.data);
    if (data == nullptr
        || (data->side != ChannelSide::A && data->side != ChannelSide::B)) {
        return libk::nullopt;
    }
    return libk::optional<ChannelSide>{data->side};
}

auto Channel::ready_locked(
    ChannelSide value,
    ChannelCondition condition) const noexcept -> bool {
    const Side& current = side(value);
    const Side& other = side(peer(value));
    switch (condition) {
    case ChannelCondition::Readable:
        return !current.queue.empty() || current.closed || other.closed;
    case ChannelCondition::Writable:
        return current.closed || other.closed
            || (other.occupied < config_.queue_capacity);
    case ChannelCondition::PeerClosed:
        return other.closed;
    }
    return false;
}

auto Channel::sequence_locked(
    ChannelSide value,
    ChannelCondition condition) const noexcept -> u64 {
    return side(value).sequence[static_cast<usize>(condition)];
}

void Channel::notify_ready() {
    Waiter* ready[4]{};
    usize ready_count{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        for (auto& queues : wait_queues_) {
            for (auto& queue : queues) {
                if (queue.empty()) continue;
                Waiter& waiter = queue.front();
                if ((waiter.state == Waiter::State::Awaiting
                        || waiter.state == Waiter::State::Armed)
                    && waiter_ready_locked(waiter)) {
                    waiter.state = Waiter::State::Ready;
                    ++waiter.references;
                    ready[ready_count++] = &waiter;
                }
            }
        }
    }
    // The ABI bounds the readiness fanout independently of waiter count.
    // Signal one source at a time rather than putting the fanout on the stack.
    for (auto& relation : relations_) {
        NotificationSource* pending{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            if (relation.state == Relation::State::Attached && relation.armed
                && ready_locked(relation.side, relation.condition)) {
                relation.observed = sequence_locked(relation.side, relation.condition);
                relation.armed = false;
                pending = &relation.source;
            }
        }
        if (pending != nullptr) (void)pending->signal();
    }
    for (usize index = 0; index < ready_count; ++index) {
        detach_waiter_authority(*ready[index]);
        drop_waiter(*ready[index]);
    }
}

auto Channel::wait_queue(Waiter::Kind kind, ChannelSide value) noexcept -> WaitQueue& {
    return wait_queues_[side_index(value)][static_cast<usize>(kind)];
}

auto Channel::owns_turn_locked(
    Waiter::Kind kind, ChannelSide value, const Waiter* reservation) noexcept -> bool {
    auto& queue = wait_queue(kind, value);
    return reservation == nullptr ? queue.empty()
        : !queue.empty() && &queue.front() == reservation
            && reservation->state == Waiter::State::Ready;
}

auto Channel::waiter_ready_locked(const Waiter& waiter) noexcept -> bool {
    const auto& queue = wait_queue(waiter.kind, waiter.side);
    if (queue.empty() || &queue.front() != &waiter) return false;
    if (closing_ || !opened_) {
        return true;
    }
    const Side& current = side(waiter.side);
    const Side& other = side(peer(waiter.side));
    if (waiter.kind == Waiter::Kind::Send) {
        // A close is a terminal wake condition as well as a queue-state
        // transition. The resume path re-resolves the authority and returns
        // CLOSED/PEER_CLOSED instead of leaving a blocked sender stranded.
        return current.closed || other.closed
            || (other.occupied < config_.queue_capacity);
    }
    return !current.queue.empty() || current.closed || other.closed;
}

auto Channel::arm_waiter(Waiter& waiter) noexcept -> bool {
    bool ready{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (waiter.state == Waiter::State::Done) {
            ready = true;
        } else if (waiter_ready_locked(waiter)) {
            waiter.state = Waiter::State::Ready;
            ready = true;
        } else {
            waiter.state = Waiter::State::Armed;
        }
    }
    if (ready) {
        detach_waiter_authority(waiter);
    }
    return ready;
}

auto Channel::send(
    cap::Resolved<Channel>& authority,
    cap::CSpace& source,
    const ChannelSend& request) noexcept
    -> libk::Expected<u64, ChannelError> {
    return send_impl(authority, source, request, nullptr);
}

auto Channel::send_impl(
    cap::Resolved<Channel>& authority,
    cap::CSpace& source,
    const ChannelSend& request,
    Waiter* reservation) noexcept
    -> libk::Expected<u64, ChannelError> {
    auto side_value = authority_side(authority, cap::Right::Send);
    if (!side_value) {
        return libk::unexpected(ChannelError::Denied);
    }
    const auto effective = authority.authority();
    const auto* const auth = libk::get_if<cap::ChannelAuthority>(
        &effective.data);
    if (auth == nullptr || !auth->exact()) {
        return libk::unexpected(ChannelError::Denied);
    }
    if (request.word_count > config_.max_words
        || request.cap_count > config_.max_caps) {
        return libk::unexpected(ChannelError::Invalid);
    }

    Message* message{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!opened_ || closing_) {
            return libk::unexpected(ChannelError::Closed);
        }
        if (!owns_turn_locked(Waiter::Kind::Send, *side_value, reservation)) {
            return libk::unexpected(ChannelError::WouldBlock);
        }
        const Side& current = side(*side_value);
        Side& target = side(peer(*side_value));
        if (current.closed) {
            return libk::unexpected(ChannelError::Closed);
        }
        if (target.closed) {
            return libk::unexpected(ChannelError::PeerClosed);
        }
        if (target.occupied >= config_.queue_capacity) {
            return libk::unexpected(ChannelError::WouldBlock);
        }
        if (target.sequence[static_cast<usize>(ChannelCondition::Readable)]
                == libk::numeric_limits<u64>::max()
            || current.sequence[static_cast<usize>(ChannelCondition::Writable)]
                == libk::numeric_limits<u64>::max()) {
            return libk::unexpected(ChannelError::GenerationExhausted);
        }
        KASSERT(!free_messages_.empty());
        message = &free_messages_.pop_front();
        message->destination = peer(*side_value);
        ++target.occupied;
    }

    message->transaction = request.transaction;
    message->tag = request.tag;
    message->sender_badge = auth->badge;
    message->word_count = request.word_count;
    for (usize index = 0; index < request.word_count; ++index) {
        message->words[index] = request.words[index];
    }
    for (usize index = 0; index < request.cap_count; ++index) {
        if (!message->escrows.try_emplace_back()) {
            discard_message(*message);
            release_message(*message);
            return libk::unexpected(ChannelError::ResourceExhausted);
        }
        auto& escrow = message->escrows.back();
        auto made = make_escrow(source, request.caps[index], escrow);
        if (!made) {
            discard_message(*message);
            release_message(*message);
            return libk::unexpected(made.error());
        }
    }

    u64 sequence{};
    ChannelError failure{ChannelError::Closed};
    bool enqueued{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!opened_ || closing_) {
            failure = ChannelError::Closed;
        } else {
            if (!owns_turn_locked(Waiter::Kind::Send, *side_value, reservation)) {
                failure = ChannelError::WouldBlock;
            } else {
                Side& target = side(peer(*side_value));
                if (side(*side_value).closed) {
                    failure = ChannelError::Closed;
                } else if (target.closed) {
                    failure = ChannelError::PeerClosed;
                } else {
                    u64& readable_sequence = target.sequence[
                        static_cast<usize>(ChannelCondition::Readable)];
                    u64& writable_sequence = side(*side_value).sequence[
                        static_cast<usize>(ChannelCondition::Writable)];
                    if (readable_sequence
                            == libk::numeric_limits<u64>::max()
                        || writable_sequence
                            == libk::numeric_limits<u64>::max()) {
                        failure = ChannelError::GenerationExhausted;
                    } else {
                        ++readable_sequence;
                        sequence = readable_sequence;
                        message->sequence = sequence;
                        ++writable_sequence;
                        target.queue.push_back(*message);
                        enqueued = true;
                    }
                }
            }
        }
    }
    if (!enqueued) {
        discard_message(*message);
        release_message(*message);
        return libk::unexpected(failure);
    }
    notify_ready();
    return libk::expected(sequence);
}

auto Channel::send_blocking(
    cap::Resolved<Channel>& authority,
    cap::CapHandle authority_handle,
    cap::CSpace& source,
    kernel::Thread& thread,
    kernel::CpuRegistry& cpus,
    const ChannelSend& request) noexcept
    -> libk::Expected<ChannelWaitResult, ChannelError> {
    auto sent = send(authority, source, request);
    if (sent) {
        return libk::expected(ChannelWaitResult{
            kernel::operation::State::Complete, sent.value()});
    }
    if (sent.error() != ChannelError::WouldBlock) {
        return libk::unexpected(sent.error());
    }
    auto queued = enqueue_waiter(authority, authority_handle, source, thread, Waiter::Kind::Send);
    if (!queued) return libk::unexpected(queued.error());
    Waiter& waiter = *queued.value();
    waiter.send = request;
    return begin_wait(waiter, authority, thread, cpus);
}

auto Channel::receive(
    cap::Resolved<Channel>& authority,
    cap::CSpace& destination,
    ChannelRecv& result) noexcept
    -> libk::Expected<void, ChannelError> {
    return receive_impl(authority, destination, result, nullptr);
}

auto Channel::receive_impl(
    cap::Resolved<Channel>& authority,
    cap::CSpace& destination,
    ChannelRecv& result,
    Waiter* reservation) noexcept
    -> libk::Expected<void, ChannelError> {
    auto side_value = authority_side(authority, cap::Right::Receive);
    if (!side_value) {
        return libk::unexpected(ChannelError::Denied);
    }

    for (usize attempt = 0; attempt != 2; ++attempt) {
        usize cap_count{};
        u64 expected_sequence{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            if (!opened_ || closing_) {
                return libk::unexpected(ChannelError::Closed);
            }
            if (!owns_turn_locked(Waiter::Kind::Receive, *side_value, reservation)) {
                return libk::unexpected(ChannelError::WouldBlock);
            }
            Side& current = side(*side_value);
            if (current.queue.empty()) {
                if (current.closed) {
                    return libk::unexpected(ChannelError::Closed);
                }
                return side(peer(*side_value)).closed
                    ? libk::Expected<void, ChannelError>{
                          libk::unexpected(ChannelError::PeerClosed)}
                    : libk::Expected<void, ChannelError>{
                          libk::unexpected(ChannelError::WouldBlock)};
            }
            const Message& message = current.queue.front();
            cap_count = message.escrows.size();
            if (cap_count > result.receive_limit) {
                return libk::unexpected(ChannelError::ResourceExhausted);
            }
            expected_sequence = message.sequence;
        }

        libk::InplaceVector<cap::CSpace::Reservation,
            MYOS_CHANNEL_MAX_CAPS> reservations{};
        for (usize index = 0; index < cap_count; ++index) {
            auto reserved = destination.reserve();
            if (!reserved) {
                return libk::unexpected(cap_error(reserved.error()));
            }
            KASSERT(reservations.try_push_back(libk::move(reserved).value()));
        }

        ChannelRecv received{};
        Message* consumed{};
        CommitResult commit = CommitResult::Capacity;
        {
            kernel::sync::IrqLockGuard guard{lock_};
            Side& current = side(*side_value);
            if (!owns_turn_locked(Waiter::Kind::Receive, *side_value, reservation)) {
                return libk::unexpected(ChannelError::WouldBlock);
            }
            if (current.queue.empty()
                || current.queue.front().sequence != expected_sequence) {
                continue;
            }
            Message& message = current.queue.front();
            received.transaction = message.transaction;
            received.tag = message.tag;
            received.sender_badge = message.sender_badge;
            received.word_count = message.word_count;
            for (usize index = 0; index < message.word_count; ++index) {
                received.words[index] = message.words[index];
            }
            received.cap_count = cap_count;
            commit = commit_escrows(
                message, destination, reservations, received);
            if (commit == CommitResult::Capacity) {
                return libk::unexpected(ChannelError::ResourceExhausted);
            }
            (void)current.queue.pop_front();
            const usize readable = static_cast<usize>(
                ChannelCondition::Readable);
            const usize writable = static_cast<usize>(
                ChannelCondition::Writable);
            ++current.sequence[readable];
            ++side(peer(*side_value)).sequence[
                writable];
            received.sequence = current.sequence[readable];
            consumed = &message;
        }
        KASSERT(consumed != nullptr);
        if (commit == CommitResult::Invalid) {
            discard_message(*consumed);
            release_message(*consumed);
            notify_ready();
            return libk::unexpected(ChannelError::TransferFailed);
        }
        result = received;
        discard_message(*consumed);
        release_message(*consumed);
        notify_ready();
        return libk::expected();
    }
    return libk::unexpected(ChannelError::Busy);
}

auto Channel::receive_blocking(
    cap::Resolved<Channel>& authority,
    cap::CapHandle authority_handle,
    cap::CSpace& destination,
    kernel::Thread& thread,
    kernel::CpuRegistry& cpus,
    Buffer* buffer,
    ChannelRecv& result) noexcept
    -> libk::Expected<ChannelWaitResult, ChannelError> {
    auto received = receive(authority, destination, result);
    if (received) {
        return libk::expected(ChannelWaitResult{
            kernel::operation::State::Complete, result.sequence});
    }
    if (received.error() != ChannelError::WouldBlock) {
        return libk::unexpected(received.error());
    }
    auto queued = enqueue_waiter(authority, authority_handle, destination, thread, Waiter::Kind::Receive);
    if (!queued) return libk::unexpected(queued.error());
    Waiter& waiter = *queued.value();
    waiter.buffer = buffer;
    waiter.receive_limit = result.receive_limit;
    return begin_wait(waiter, authority, thread, cpus);
}

auto Channel::enqueue_waiter(
    cap::Resolved<Channel>& authority, cap::CapHandle handle,
    cap::CSpace& cspace, kernel::Thread& thread, Waiter::Kind kind) noexcept
    -> libk::Expected<Waiter*, ChannelError> {
    auto reference = authority.reference();
    if (!reference) return libk::unexpected(ChannelError::Invalid);
    const auto value = authority_side(authority,
        kind == Waiter::Kind::Send ? cap::Right::Send : cap::Right::Receive);
    KASSERT(value);
    auto* node = thread.current_wait().prepare_channel(*this);
    if (node == nullptr) return libk::unexpected(ChannelError::Busy);
    Waiter& waiter = *node;
    waiter.kind = kind;
    waiter.side = *value;
    waiter.cspace = &cspace;
    waiter.authority = handle;
    waiter.channel_ref = libk::move(reference).value();
    waiter.completion.set_policy(diag::concurrency::OperationPolicy{
        .kind = kind == Waiter::Kind::Send
            ? diag::concurrency::WaitKind::ChannelSend
            : diag::concurrency::WaitKind::ChannelReceive,
        .expectation = diag::concurrency::Expectation::ExternalUnbounded,
        .driver = diag::concurrency::NodeRef::external(reinterpret_cast<u64>(this), 1),
    });
    bool accepted{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        accepted = opened_ && !closing_;
        if (accepted) {
            ++waiter_count_;
            wait_queue(kind, *value).push_back(waiter);
        } else {
            waiter.state = Waiter::State::Done;
            waiter.references = 0;
        }
    }
    if (!accepted) {
        waiter.channel_ref.reset();
        return libk::unexpected(ChannelError::Closed);
    }
    return libk::expected(&waiter);
}

auto Channel::begin_wait(
    Waiter& waiter, cap::Resolved<Channel>& authority,
    kernel::Thread& thread, kernel::CpuRegistry& cpus) noexcept
    -> libk::Expected<ChannelWaitResult, ChannelError> {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        ++waiter.references; // Completion, before any producer can publish.
    }
    if (!thread.begin_wait(waiter.completion, cpus)) {
        { kernel::sync::IrqLockGuard guard{lock_}; --waiter.references; }
        finish_waiter(waiter);
        return libk::unexpected(ChannelError::Busy);
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        ++waiter.references; // Authority and all its dispatched callbacks.
    }
    const bool attached = static_cast<bool>(authority.attach(waiter.grant_attachment));
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!attached) {
            waiter.result = kernel::operation::Result{MYOS_STATUS_INVALID_CAP, 0};
            waiter.state = Waiter::State::Done;
            waiter.authority_detaching = true;
            --waiter.references;
        } else if (waiter.state == Waiter::State::Attaching) {
            waiter.state = Waiter::State::Awaiting;
        }
        waiter.admitted = true;
    }
    (void)arm_waiter(waiter);
    drop_waiter(waiter); // Admission is the last access on this path.
    return libk::expected(ChannelWaitResult{kernel::operation::State::Waiting, 0});
}

auto Channel::close(ChannelSide value) noexcept -> bool {
    if (value != ChannelSide::A && value != ChannelSide::B) {
        return false;
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Side& current = side(value);
        if (current.closed) {
            return false;
        }
        current.closed = true;
        for (u64& sequence : current.sequence) {
            if (sequence != libk::numeric_limits<u64>::max()) {
                ++sequence;
            }
        }
        Side& other = side(peer(value));
        for (u64& sequence : other.sequence) {
            if (sequence != libk::numeric_limits<u64>::max()) {
                ++sequence;
            }
        }
    }
    notify_ready();
    return true;
}

auto Channel::close(
    cap::Resolved<Channel>& authority) noexcept
    -> libk::Expected<void, ChannelError> {
    auto side_value = authority_side(authority, cap::Right::Close);
    if (!side_value) {
        return libk::unexpected(ChannelError::Denied);
    }
    static_cast<void>(close(*side_value));
    return libk::expected();
}

auto Channel::bind(
    cap::Resolved<Channel>& authority,
    cap::Resolved<Notification>& notification,
    ChannelCondition condition) noexcept
    -> libk::Expected<usize, ChannelError> {
    const cap::Right right = condition == ChannelCondition::Writable
        ? cap::Right::Send : cap::Right::Receive;
    auto side_value = authority_side(authority, right);
    if (!side_value) {
        return libk::unexpected(ChannelError::Denied);
    }
    const auto notification_authority = notification.authority();
    const auto* const notification_data =
        libk::get_if<cap::NotificationAuthority>(
            &notification_authority.data);
    if (notification_data == nullptr
        || !notification.rights().contains(cap::Right::Signal)
        || notification_data->badge == 0) {
        return libk::unexpected(ChannelError::Denied);
    }

    Relation* relation{};
    usize index{};
    u64 generation{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!opened_ || closing_) {
            return libk::unexpected(ChannelError::ResourceExhausted);
        }
        for (auto& candidate : relations_) {
            if (candidate.state == Relation::State::Idle) {
                relation = &candidate;
                break;
            }
        }
        if (relation == nullptr) return libk::unexpected(ChannelError::ResourceExhausted);
        index = relation->index;
        if (relation->generation == kRelationGenerationMax) {
            return libk::unexpected(ChannelError::GenerationExhausted);
        }
        ++relation->generation;
        generation = relation->generation;
        relation->side = *side_value;
        relation->condition = condition;
        relation->observed = sequence_locked(*side_value, condition);
        relation->armed = true;
        relation->state = Relation::State::Attaching;
    }

    auto hold_ref = notification.reference();
    if (!hold_ref) {
        abort_relation(*relation);
        return libk::unexpected(ChannelError::Busy);
    }
    auto hold = libk::move(hold_ref).value().into_hold<Notification>();
    if (!hold) {
        abort_relation(*relation);
        return libk::unexpected(ChannelError::Busy);
    }
    relation->notification = libk::move(hold).value();
    auto bound = relation->notification->bind(
        relation->source, notification_data->badge);
    if (!bound) {
        abort_relation(*relation);
        return libk::unexpected(ChannelError::Busy);
    }
    if (!authority.attach(relation->channel_link.attachment)) {
        abort_relation(*relation);
        return libk::unexpected(ChannelError::Busy);
    }
    if (!notification.attach(relation->notification_link.attachment)) {
        abort_relation(*relation);
        return libk::unexpected(ChannelError::Busy);
    }
    bool committed{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (relation->state == Relation::State::Attaching && !closing_) {
            relation->state = Relation::State::Attached;
            committed = true;
        }
    }
    if (!committed) {
        abort_relation(*relation);
        return libk::unexpected(ChannelError::Busy);
    }
    notify_ready();
    return libk::expected((generation << kRelationIndexBits) | index);
}

auto Channel::arm(
    cap::Resolved<Channel>& authority,
    usize relation_handle,
    u64 observed) noexcept -> libk::Expected<u64, ChannelError> {
    const usize index = relation_handle & kRelationIndexMask;
    const u64 generation = relation_handle >> kRelationIndexBits;
    NotificationSource* signal{};
    u64 sequence{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (index >= relations_.size()) {
            return libk::unexpected(ChannelError::InvalidRelation);
        }
        Relation& relation = relation_at(index);
        const cap::Right required = relation.condition
            == ChannelCondition::Writable
            ? cap::Right::Send : cap::Right::Receive;
        const auto side_value = authority_side(authority, required);
        if (!side_value) {
            return libk::unexpected(ChannelError::Denied);
        }
        const ChannelSide requested_side = *side_value;
        if (relation.state != Relation::State::Attached
            || relation.generation != generation
            || relation.side != requested_side) {
            return libk::unexpected(ChannelError::InvalidRelation);
        }
        sequence = sequence_locked(
            relation.side, relation.condition);
        relation.observed = observed;
        if (ready_locked(relation.side, relation.condition)
            || sequence != observed) {
            relation.armed = false;
            signal = &relation.source;
        } else {
            relation.armed = true;
        }
    }
    if (signal != nullptr) {
        static_cast<void>(signal->signal());
    }
    return libk::expected(sequence);
}

auto Channel::mint(
    cap::Resolved<Channel>& authority,
    cap::CSpace& destination,
    u64 badge,
    cap::Rights rights) noexcept
    -> libk::Expected<cap::CapHandle, ChannelError> {
    const auto side_value = authority_side(authority, cap::Right::Delegate);
    if (!side_value || badge == 0) {
        return libk::unexpected(ChannelError::Denied);
    }
    const auto effective = authority.authority();
    const auto* const data = libk::get_if<cap::ChannelAuthority>(
        &effective.data);
    if (data == nullptr || !data->unbound()) {
        return libk::unexpected(ChannelError::Denied);
    }
    auto reserved = destination.reserve_derivation();
    if (!reserved) {
        return libk::unexpected(cap_error(reserved.error()));
    }
    auto target = authority.reference();
    if (!target) {
        return libk::unexpected(ChannelError::Busy);
    }
    const cap::ChannelAuthority child_data{
        .side = *side_value,
        .badge = badge,
        .fixed = ~u64{},
    };
    const cap::GrantCeiling ceiling{rights, child_data};
    const cap::ChannelBadgeDerivation proof{*this, *side_value, badge};
    auto transaction = libk::move(reserved).value();
    auto child = authority.derive_channel_badge(
        libk::move(transaction.grant_),
        libk::move(target).value(),
        ceiling,
        proof);
    if (!child) {
        return libk::unexpected(ChannelError::Denied);
    }
    auto installed = destination.insert(
        libk::move(transaction.slot_),
        libk::move(child).value(),
        cap::CapView{rights, child_data});
    if (!installed) {
        return libk::unexpected(cap_error(installed.error()));
    }
    return libk::expected(installed.value());
}

void Channel::retire(object::ObjectCleanup&& cleanup) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(!cleanup_);
        cleanup_ = libk::move(cleanup);
        closing_ = true;
    }
    static_cast<void>(close(ChannelSide::A));
    static_cast<void>(close(ChannelSide::B));
    clear_queues();
    for (Relation& relation : relations_) {
        if (relation.state != Relation::State::Idle) {
            detach_relation(relation);
        }
    }
    for (SideLink& link : side_links_) {
        detach_side(link);
    }
    try_finish_retire();
}

auto Channel::side_state(ChannelSide value) const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return side(value).closed;
}

void Channel::finish_waiter(Waiter& waiter) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(!waiter.completion.attached() && waiter.references == 1);
        KASSERT(!waiter.grant_attachment.attached() && !waiter.grant_attachment.busy());
        waiter.state = Waiter::State::Done;
        waiter.references = 0;
        wait_queue(waiter.kind, waiter.side).erase(waiter);
        KASSERT(waiter_count_ != 0);
        --waiter_count_;
    }
    auto channel_ref = libk::move(waiter.channel_ref);
    notify_ready();
    try_finish_retire();
}

void Channel::detach_waiter_authority(Waiter& waiter) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!waiter.admitted || waiter.authority_detaching) return;
        waiter.authority_detaching = true;
    }
    if (waiter.grant_attachment.detach()) drop_waiter(waiter);
    // Otherwise the last dispatched GrantWork drops the authority reference.
}

void Channel::drop_waiter(Waiter& waiter) noexcept {
    bool publish{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(waiter.references > 1);
        --waiter.references;
        publish = waiter.references == 1
            && (waiter.state == Waiter::State::Ready || waiter.state == Waiter::State::Done);
        if (publish) KASSERT(waiter.admitted && waiter.authority_detaching);
    }
    // The sole remaining reference belongs to Completion. No callback can
    // access resident storage after it becomes consumable by the continuation.
    if (publish) waiter.completion.signal();
}

void Channel::invalidate_waiter(
    void* context,
    cap::GrantWork&& work,
    cap::GrantInvalidation reason) noexcept {
    KASSERT(context != nullptr && reason == cap::GrantInvalidation::Revoke);
    auto& waiter = *static_cast<Waiter*>(context);
    waiter.owner->waiter_invalidated(waiter, libk::move(work));
}

void Channel::release_waiter(void* context) noexcept {
    KASSERT(context != nullptr);
    auto& waiter = *static_cast<Waiter*>(context);
    waiter.owner->drop_waiter(waiter);
}

void Channel::waiter_invalidated(
    Waiter& waiter, cap::GrantWork&& work) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        ++waiter.references;
        waiter.result = kernel::operation::Result{MYOS_STATUS_DENIED, 0};
        waiter.state = Waiter::State::Done;
    }
    detach_waiter_authority(waiter);
    work.reset();
    drop_waiter(waiter);
}

auto Channel::resume_waiter(
    Waiter& waiter,
    arch::TrapContext& trap) noexcept -> bool {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (waiter.state == Waiter::State::Done) {
            trap.set_result(0, static_cast<usize>(
                static_cast<isize>(waiter.result.status)));
            trap.set_result(1, waiter.result.value);
            return true;
        }
        KASSERT(waiter.state == Waiter::State::Ready);
    }

    ChannelError error{ChannelError::Invalid};
    bool success{};
    u64 value{};
    if (waiter.cspace == nullptr || !waiter.authority) {
        error = ChannelError::Invalid;
    } else if (waiter.kind == Waiter::Kind::Send) {
        auto authority = waiter.cspace->resolve<Channel>(
            waiter.authority, cap::Rights::of(cap::Right::Send));
        if (!authority) {
            error = ChannelError::Invalid;
        } else {
            auto sent = send_impl(
                authority.value(), *waiter.cspace, waiter.send, &waiter);
            if (sent) {
                value = sent.value();
                success = true;
            } else {
                error = sent.error();
            }
        }
    } else {
        ChannelRecv received{};
        received.receive_limit = waiter.receive_limit;
        auto authority = waiter.cspace->resolve<Channel>(
            waiter.authority, cap::Rights::of(cap::Right::Receive));
        if (!authority) {
            error = ChannelError::Invalid;
        } else {
            auto admitted = waiter.buffer != nullptr
                ? waiter.buffer->access()
                : libk::Expected<Buffer::Access, BufferError>{
                      libk::unexpected(BufferError::Invalid)};
            if (!admitted) {
                error = ChannelError::TransferFailed;
            } else {
                auto taken = receive_impl(
                    authority.value(), *waiter.cspace, received, &waiter);
                if (!taken) {
                    error = taken.error();
                } else {
                    myos_channel_message wire{};
                    wire.version = MYOS_CHANNEL_VERSION;
                    wire.receive_limit = static_cast<u32>(
                        received.receive_limit);
                    wire.transaction = received.transaction;
                    wire.tag = received.tag;
                    wire.word_count = static_cast<u32>(received.word_count);
                    wire.cap_count = static_cast<u32>(received.cap_count);
                    wire.received_count = static_cast<u32>(
                        received.cap_count);
                    wire.sender_badge = received.sender_badge;
                    wire.sequence = received.sequence;
                    for (usize index = 0;
                         index < received.word_count;
                         ++index) {
                        wire.words[index] = received.words[index];
                    }
                    for (usize index = 0;
                         index < received.cap_count;
                         ++index) {
                        wire.received[index] = received.caps[index].raw();
                    }
                    if (!admitted.value().write(0, libk::Span<const byte>{
                            reinterpret_cast<const byte*>(&wire),
                            sizeof(wire)})) {
                        error = ChannelError::TransferFailed;
                    } else {
                        value = received.sequence;
                        success = true;
                    }
                }
            }
        }
    }

    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (waiter.state == Waiter::State::Done) {
            trap.set_result(0, static_cast<usize>(
                static_cast<isize>(waiter.result.status)));
            trap.set_result(1, waiter.result.value);
            return true;
        }
        waiter.result = success
            ? kernel::operation::Result{MYOS_STATUS_OK, value}
            : kernel::operation::Result{wait_status(error), 0};
        waiter.state = Waiter::State::Done;
        trap.set_result(0, static_cast<usize>(
            static_cast<isize>(waiter.result.status)));
        trap.set_result(1, waiter.result.value);
    }
    return true;
}

void Channel::invalidate(
    void* context,
    cap::GrantWork&& work,
    cap::GrantInvalidation reason) noexcept {
    KASSERT(context != nullptr && reason == cap::GrantInvalidation::Revoke);
    auto& link = *static_cast<AuthLink*>(context);
    link.relation->owner->invalidated(link, libk::move(work));
}

void Channel::released(void* context) noexcept {
    KASSERT(context != nullptr);
    auto& link = *static_cast<AuthLink*>(context);
    link.relation->owner->relation_released(*link.relation);
}

void Channel::invalidated(AuthLink& link, cap::GrantWork&& work) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        link.work = libk::move(work);
    }
    detach_relation(*link.relation);
}

void Channel::relation_released(Relation& relation) noexcept {
    if (!relation.channel_link.attachment.attached()
        && !relation.channel_link.attachment.busy()) {
        relation.channel_link.attachment.reset();
    }
    if (!relation.notification_link.attachment.attached()
        && !relation.notification_link.attachment.busy()) {
        relation.notification_link.attachment.reset();
    }
    finish_relation(relation);
    try_finish_retire();
}

void Channel::invalidate_side(
    void* context,
    cap::GrantWork&& work,
    cap::GrantInvalidation reason) noexcept {
    KASSERT(context != nullptr && reason == cap::GrantInvalidation::Revoke);
    auto& link = *static_cast<SideLink*>(context);
    link.owner->side_invalidated(link, libk::move(work));
}

void Channel::release_side(void* context) noexcept {
    KASSERT(context != nullptr);
    auto& link = *static_cast<SideLink*>(context);
    link.owner->side_released(link);
}

void Channel::side_invalidated(
    SideLink& link,
    cap::GrantWork&& work) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        link.work = libk::move(work);
    }
    static_cast<void>(close(link.side));
    if (link.attachment.attached()) {
        static_cast<void>(link.attachment.detach());
    }
    link.work.reset();
    if (!link.attachment.attached() && !link.attachment.busy()) {
        link.attachment.reset();
    }
    try_finish_retire();
}

void Channel::side_released(SideLink& link) noexcept {
    if (!link.attachment.attached() && !link.attachment.busy()) {
        link.attachment.reset();
    }
    try_finish_retire();
}

void Channel::detach_side(SideLink& link) noexcept {
    if (link.attachment.attached()) {
        static_cast<void>(link.attachment.detach());
    }
    link.work.reset();
    if (!link.attachment.attached() && !link.attachment.busy()) {
        link.attachment.reset();
    }
}

void Channel::try_finish_retire() noexcept {
    object::ObjectCleanup done{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!cleanup_ || !closing_
            || waiter_count_ != 0) {
            return;
        }
        for (const SideLink& link : side_links_) {
            if (link.attachment.attached() || link.attachment.busy()
                || link.work) {
                return;
            }
        }
        for (const Relation& relation : relations_) {
            if (relation.state != Relation::State::Idle
                || relation.channel_link.attachment.attached()
                || relation.channel_link.attachment.busy()
                || relation.channel_link.work
                || relation.notification_link.attachment.attached()
                || relation.notification_link.attachment.busy()
                || relation.notification_link.work) {
                return;
            }
        }
        done = libk::move(cleanup_);
    }
    done.complete();
}

void Channel::abort_relation(Relation& relation) noexcept {
    bool do_abort{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (relation.state == Relation::State::Attaching
            || relation.state == Relation::State::Detaching) {
            relation.state = Relation::State::Detaching;
            do_abort = true;
        }
    }
    if (!do_abort) {
        return;
    }
    relation.source.reset();
    if (relation.channel_link.attachment.attached()) {
        static_cast<void>(relation.channel_link.attachment.detach());
    }
    if (relation.notification_link.attachment.attached()) {
        static_cast<void>(relation.notification_link.attachment.detach());
    }
    relation.channel_link.work.reset();
    relation.notification_link.work.reset();
    if (!relation.channel_link.attachment.attached()
        && !relation.channel_link.attachment.busy()) {
        relation.channel_link.attachment.reset();
    }
    if (!relation.notification_link.attachment.attached()
        && !relation.notification_link.attachment.busy()) {
        relation.notification_link.attachment.reset();
    }
    relation.notification.reset();
    finish_relation(relation);
    try_finish_retire();
}

void Channel::detach_relation(Relation& relation) noexcept {
    bool do_detach{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (relation.state == Relation::State::Attaching) {
            // The binder owns the in-flight installation. Mark cancellation
            // only; it will roll back all reverse edges before returning.
            relation.state = Relation::State::Detaching;
        } else if (relation.state == Relation::State::Attached) {
            relation.state = Relation::State::Detaching;
            do_detach = true;
        }
    }
    if (!do_detach) {
        return;
    }
    abort_relation(relation);
}

void Channel::finish_relation(Relation& relation) noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    // GrantAttachment::released is a deferred callback: the last work item
    // may outlive the relation transition that already returned the cell to
    // Idle.  Releasing that stale callback is an idempotent completion, not a
    // second detach transaction.
    if (relation.state != Relation::State::Detaching) {
        return;
    }
    if (relation.channel_link.attachment.attached()
        || relation.channel_link.attachment.busy()
        || relation.channel_link.work
        || relation.notification_link.attachment.attached()
        || relation.notification_link.attachment.busy()
        || relation.notification_link.work) {
        return;
    }
    relation.state = Relation::State::Idle;
    relation.armed = false;
    relation.observed = 0;
}

void Channel::discard_message(Message& message) noexcept {
    for (Escrow& escrow : message.escrows) {
        if (escrow.kind == Escrow::Kind::Move && escrow.source != nullptr) {
            if (!escrow.source->escrow_restore(
                    escrow.source_slot,
                    libk::move(escrow.grant),
                    escrow.view)) {
                auto refund = escrow.source->escrow_drop(escrow.source_slot);
                refund.complete();
            }
        } else {
            escrow.grant.reset();
        }
    }
    message.escrows.clear();
}

auto Channel::relation_at(usize index) noexcept -> Relation& {
    KASSERT(index < relations_.size());
    auto it = relations_.begin();
    while (index-- != 0) ++it;
    return *it;
}

void Channel::release_message(Message& message) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        auto& target = side(message.destination);
        KASSERT(target.occupied != 0);
        --target.occupied;
        free_messages_.push_back(message);
    }
    notify_ready();
}

void Channel::clear_queues() noexcept {
    for (usize side_index_value = 0; side_index_value < 2; ++side_index_value) {
        for (;;) {
            Message* message{};
            {
                kernel::sync::IrqLockGuard guard{lock_};
                Side& current = sides_[side_index_value];
                if (current.queue.empty()) {
                    break;
                }
                message = &current.queue.pop_front();
            }
            KASSERT(message != nullptr);
            discard_message(*message);
            release_message(*message);
        }
    }
}

auto Channel::make_escrow(
    cap::CSpace& source,
    const myos_cap_transfer& spec,
    Escrow& escrow) noexcept -> libk::Expected<void, ChannelError> {
    libk::optional<Escrow::Kind> kind{};
    switch (spec.operation) {
    case MYOS_CAP_COPY:
        kind.emplace(Escrow::Kind::Copy);
        break;
    case MYOS_CAP_MOVE:
        kind.emplace(Escrow::Kind::Move);
        break;
    case MYOS_CAP_DELEGATE:
        kind.emplace(Escrow::Kind::Delegate);
        break;
    default:
        break;
    }
    auto rights = cap::Rights::from_raw(spec.rights);
    if (!kind || !rights || spec.flags != 0) {
        return libk::unexpected(ChannelError::Invalid);
    }
    const cap::CapHandle source_handle = cap::CapHandle::from_raw(spec.source);
    auto copied = source.snapshot(source_handle);
    if (!copied) {
        return libk::unexpected(cap_error(copied.error()));
    }
    auto snapshot = libk::move(copied).value();
    cap::GrantLease lease = libk::move(snapshot.lease);
    auto effective = cap::compose(
        lease.kind(), lease.ceiling(), snapshot.view);
    if (!effective) {
        return libk::unexpected(ChannelError::Denied);
    }
    escrow.source_handle = source_handle;
    escrow.kind = *kind;
    switch (*kind) {
    case Escrow::Kind::Copy: {
        if (!effective.value().rights.contains(cap::Right::Duplicate)) {
            return libk::unexpected(ChannelError::Denied);
        }
        const cap::CapView view{*rights, effective.value().data};
        auto valid = cap::compose(lease.kind(), effective.value().ceiling(), view);
        auto grant = snapshot.graph->ref(snapshot.key);
        if (!valid || !grant) {
            return libk::unexpected(ChannelError::Denied);
        }
        escrow.grant = libk::move(grant).value();
        escrow.view = view;
        break;
    }
    case Escrow::Kind::Delegate: {
        const cap::GrantCeiling ceiling{*rights, effective.value().data};
        if (!effective.value().rights.contains(cap::Right::Delegate)
            || !cap::attenuates(lease.kind(), effective.value(), ceiling)) {
            return libk::unexpected(ChannelError::Denied);
        }
        auto charge = source.reserve_grant();
        auto target = lease.clone_target();
        auto valid = cap::compose(lease.kind(), ceiling,
            cap::CapView{*rights, effective.value().data});
        if (!charge || !target || !valid) {
            return libk::unexpected(ChannelError::ResourceExhausted);
        }
        auto child = snapshot.graph->derive(
            libk::move(charge).value(), lease,
            libk::move(target).value(), ceiling);
        if (!child) {
            return libk::unexpected(ChannelError::ResourceExhausted);
        }
        escrow.grant = libk::move(child).value();
        escrow.view = cap::CapView{*rights, effective.value().data};
        break;
    }
    case Escrow::Kind::Move: {
        if (!rights->empty()) {
            return libk::unexpected(ChannelError::Invalid);
        }
        auto moved = source.escrow_move(
            escrow.source_handle,
            escrow.grant,
            escrow.view,
            escrow.source_slot);
        if (!moved) {
            return libk::unexpected(cap_error(moved.error()));
        }
        escrow.source = &source;
        break;
    }
    }
    return libk::expected();
}

auto Channel::commit_escrows(
    Message& message,
    cap::CSpace& destination,
    libk::InplaceVector<cap::CSpace::Reservation,
        MYOS_CHANNEL_MAX_CAPS>& reservations,
    ChannelRecv& result) noexcept -> CommitResult {
    if (reservations.size() != message.escrows.size()) {
        return CommitResult::Capacity;
    }

    // Keep a lease for every in-flight Grant through the destination commit.
    // A revoke may race the receive after the message was queued; the lease
    // makes the preflight and publication one indivisible admission window.
    libk::InplaceVector<cap::GrantLease, MYOS_CHANNEL_MAX_CAPS> leases{};
    for (Escrow& escrow : message.escrows) {
        auto acquired = escrow.grant.acquire();
        if (!acquired) {
            return CommitResult::Invalid;
        }
        auto effective = cap::compose(
            acquired.value().kind(), acquired.value().ceiling(), escrow.view);
        if (!effective || !leases.try_push_back(libk::move(acquired).value())) {
            return CommitResult::Invalid;
        }
    }

    {
        kernel::sync::IrqLockGuard guard{destination.lock_};
        if (!destination.accepting_) {
            return CommitResult::Capacity;
        }
        for (usize index = 0; index < reservations.size(); ++index) {
            const auto handle = reservations[index].handle();
            auto* const slot = handle ? destination.slot(handle.index()) : nullptr;
            if (slot == nullptr || slot->generation != handle.generation()
                || slot->state != cap::CSpace::SlotState::Reserved) {
                return CommitResult::Capacity;
            }
        }
        for (usize index = 0; index < reservations.size(); ++index) {
            Escrow& escrow = message.escrows[index];
            const cap::CapHandle handle = reservations[index].handle();
            KASSERT(escrow.grant);
            auto committed = destination.commit_locked(
                reservations[index], libk::move(escrow.grant), escrow.view);
            KASSERT(committed);
            result.caps[index] = handle;
        }
    }
    for (Escrow& escrow : message.escrows) {
        if (escrow.kind == Escrow::Kind::Move && escrow.source != nullptr) {
            auto refund = escrow.source->escrow_drop(escrow.source_slot);
            refund.complete();
            escrow.source = nullptr;
        }
    }
    return CommitResult::Committed;
}

} // namespace kernel::ipc
