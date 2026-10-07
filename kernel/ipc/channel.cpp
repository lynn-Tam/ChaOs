#include <expected>
#include <optional>
#include <ipc/notification.hpp>
#include <cap/graph.hpp>
#include <object/ref.hpp>
#include <ipc/channel.hpp>

#include <libk/assert.hpp>
#include <base/types.hpp>
#include <cpu/local.hpp>
#include <cpu/registry.hpp>
#include <ipc/buffer.hpp>
#include <limits>
#include <libk/checked_arithmetic.hpp>
#include <libk/scope_guard.hpp>
#include <utility>
#include <sync.hpp>
#include <task/thread.hpp>
#include <sched/dispatcher.hpp>
#include <uapi/abi.h>

namespace ipc {

// Relation handles reserve the low byte for the fixed relation slot.  Keep
// generation exhaustion expressed in the actual encoded width; allowing a
// full-width u64 generation would silently truncate when the handle is
// encoded in usize.
constexpr usize RelBits = 8;
constexpr usize RelMask =
    (usize{1} << RelBits) - usize{1};
constexpr u64 MaxRelGen = static_cast<u64>(
    std::numeric_limits<usize>::max() >> RelBits);

[[nodiscard]] static auto cap_error(cap::CSpaceError error) noexcept
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

Channel::GrantLink::GrantLink(Relation& owner, bool is_channel) noexcept
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
    : source(ipc::NotificationSource::Closed::bind<&Relation::closed>(*this)),
      channel_link(*this, true),
      notification_link(*this, false) {}

Channel::Relation::~Relation() noexcept {
    libk_assert(state == State::Idle);
    libk_assert(!source.attached());
    libk_assert(!channel_link.attachment.attached()
        && !notification_link.attachment.attached());
    libk_assert(!channel_link.attachment.busy()
        && !notification_link.attachment.busy());
    libk_assert(!channel_link.work && !notification_link.work);
    libk_assert(!notification);
}

Channel::Wait::Wait(Channel& channel) noexcept
    : owner(&channel),
      grant_attachment(this, Channel::waiter_ops_),
      completion(Completion::bind<Wait, &Wait::release, &Wait::cancel>(*this)) {}

Channel::Wait::~Wait() noexcept {
    if (hook.is_linked()) owner->finish_waiter(*this);
    channel_ref.reset();
    libk_assert(!hook.is_linked());
    libk_assert(!grant_attachment.attached() && !grant_attachment.busy());
    libk_assert(!completion.attached());
}

void Channel::Wait::release() noexcept {
    // The caller retains the fair turn through its result publication.
}

auto Channel::Wait::cancel() noexcept -> bool {
    {
        sync::Lock guard{owner->lock_};
        if (state != State::Awaiting && state != State::Armed) return false;
        result = WaitResult{STATUS_CANCELED, 0};
        state = State::Done;
        ++references;
    }
    owner->detach_waiter_grant(*this);
    owner->drop_waiter(*this);
    // Publication, including any cap callback already in flight, owns
    // completion. Stop retains the continuation until that publication drains.
    return false;
}

void Channel::Relation::closed() noexcept {
    notification_closed();
}

void Channel::Relation::notification_closed() noexcept {
    if (owner != nullptr) {
        owner->detach_relation(*this);
    }
}

auto Channel::storage_bytes(ChannelConfig config) noexcept -> std::optional<usize> {
    const auto cells = libk::checked_multiply(config.queue_capacity, usize{2});
    if (!cells) return std::nullopt;
    const auto pages = libk::checked_add(mm::Slab<Message, false>::quota_for(*cells).pages,
        mm::Slab<Relation, false>::quota_for(config.relation_capacity).pages);
    return pages ? libk::checked_multiply(*pages, mm::page_size) : std::nullopt;
}

Channel::Channel(mm::Pmm& pmm, ChannelConfig config) noexcept
    : config_(config),
      messages_(pmm, mm::Slab<Message, false>::quota_for(config.queue_capacity * 2)),
      side_links_{
          SideLink{*this, ChannelSide::A},
          SideLink{*this, ChannelSide::B}},
      relation_pool_(pmm, mm::Slab<Relation, false>::quota_for(config.relation_capacity)) {}

Channel::~Channel() noexcept {
    libk_assert(!cleanup_ && waiter_count_ == 0);
    clear_queues();
    while (!free_messages_.empty()) {
        auto& message = free_messages_.pop_front();
        messages_.destroy(message);
    }
    libk_assert(messages_.live_count() == 0);
    while (!relations_.empty()) {
        auto& relation = relations_.pop_front();
        libk_assert(relation.state == Relation::State::Idle);
        relation_pool_.destroy(relation);
    }
    for (SideLink& link : side_links_) {
        libk_assert(!link.attachment.attached() && !link.attachment.busy());
        libk_assert(!link.work);
    }
    libk_assert(!opened_ || closing_);
}

void Channel::bind_sponsor(
    resource::Sponsorship& sponsor) noexcept {
    libk_assert(!payer_);
    auto source = sponsor.payer().clone();
    libk_assert(source);
    payer_ = std::move(*source);
}

auto Channel::open() noexcept -> std::expected<void, ChannelError> {
    if (config_.queue_capacity == 0
        || config_.queue_capacity > CHANNEL_MAX_QUEUE
        || config_.max_words == 0
        || config_.max_words > CHANNEL_MAX_WORDS
        || config_.max_caps > CHANNEL_MAX_CAPS
        || config_.relation_capacity > CHANNEL_MAX_RELATIONS) {
        return std::unexpected(ChannelError::Invalid);
    }
    {
        sync::Lock guard{lock_};
        if (opened_) {
            return std::unexpected(ChannelError::Busy);
        }
    }

    // Creation pays for every cell and readiness slot. Operational paths only
    // move intrusive links and never grow backing storage.
    const usize required = config_.queue_capacity * 2;
    while (free_messages_.size() < required) {
        auto made = messages_.create(payer_);
        if (!made) return std::unexpected(ChannelError::ResourceExhausted);
        free_messages_.push_back(*made.value());
    }
    while (relations_.size() < config_.relation_capacity) {
        auto made = relation_pool_.create(payer_);
        if (!made) return std::unexpected(ChannelError::ResourceExhausted);
        auto& relation = *made.value();
        relation.owner = this;
        relation.index = relations_.size();
        relations_.push_back(relation);
    }

    sync::Lock guard{lock_};
    if (opened_) {
        return std::unexpected(ChannelError::Busy);
    }
    opened_ = true;
    return {};
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
    auto channel = object.value().as<Channel>();
    if (!channel || &channel.value().get() != this) {
        return false;
    }
    SideLink& link = side_links_[side_index(value)];
    const auto attached = target.value().attach(link.attachment);
    return static_cast<bool>(attached);
}

auto Channel::side(ChannelSide value) noexcept -> Side& {
    libk_assert(value == ChannelSide::A || value == ChannelSide::B);
    return sides_[side_index(value)];
}

auto Channel::side(ChannelSide value) const noexcept -> const Side& {
    libk_assert(value == ChannelSide::A || value == ChannelSide::B);
    return sides_[side_index(value)];
}

auto Channel::side_index(ChannelSide value) const noexcept -> usize {
    return value == ChannelSide::B ? 1 : 0;
}

auto Channel::peer(ChannelSide value) const noexcept -> ChannelSide {
    return value == ChannelSide::A ? ChannelSide::B : ChannelSide::A;
}

auto Channel::side_for(
    const cap::Resolved<Channel>& cap,
    cap::Right right) const noexcept -> std::optional<ChannelSide> {
    if (&cap.object() != this || !cap.rights().contains(right)) {
        return std::nullopt;
    }
    const auto effective = cap.view();
    const auto* const data = std::get_if<cap::ChanLimit>(
        &effective.data);
    if (data == nullptr
        || (data->side != ChannelSide::A && data->side != ChannelSide::B)) {
        return std::nullopt;
    }
    return std::optional<ChannelSide>{data->side};
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
        sync::Lock guard{lock_};
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
            sync::Lock guard{lock_};
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
        detach_waiter_grant(*ready[index]);
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
        // transition. The resume path re-resolves the cap and returns
        // CLOSED/PEER_CLOSED instead of leaving a blocked sender stranded.
        return current.closed || other.closed
            || (other.occupied < config_.queue_capacity);
    }
    return !current.queue.empty() || current.closed || other.closed;
}

auto Channel::arm_waiter(Waiter& waiter) noexcept -> bool {
    bool ready{};
    {
        sync::Lock guard{lock_};
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
        detach_waiter_grant(waiter);
    }
    return ready;
}

auto Channel::send(
    cap::Resolved<Channel>& cap,
    cap::CSpace& source,
    const ChannelSend& request,
    Waiter* reservation) noexcept
    -> std::expected<u64, ChannelError> {
    auto side_value = side_for(cap, cap::Right::Send);
    if (!side_value) {
        return std::unexpected(ChannelError::Denied);
    }
    const auto effective = cap.view();
    const auto* const auth = std::get_if<cap::ChanLimit>(
        &effective.data);
    if (auth == nullptr || !auth->exact()) {
        return std::unexpected(ChannelError::Denied);
    }
    if (request.word_count > config_.max_words
        || request.cap_count > config_.max_caps) {
        return std::unexpected(ChannelError::Invalid);
    }

    Message* message{};
    {
        sync::Lock guard{lock_};
        if (!opened_ || closing_) {
            return std::unexpected(ChannelError::Closed);
        }
        if (!owns_turn_locked(Waiter::Kind::Send, *side_value, reservation)) {
            return std::unexpected(ChannelError::WouldBlock);
        }
        const Side& current = side(*side_value);
        Side& target = side(peer(*side_value));
        if (current.closed) {
            return std::unexpected(ChannelError::Closed);
        }
        if (target.closed) {
            return std::unexpected(ChannelError::PeerClosed);
        }
        if (target.occupied >= config_.queue_capacity) {
            return std::unexpected(ChannelError::WouldBlock);
        }
        if (target.sequence[static_cast<usize>(ChannelCondition::Readable)]
                == std::numeric_limits<u64>::max()
            || current.sequence[static_cast<usize>(ChannelCondition::Writable)]
                == std::numeric_limits<u64>::max()) {
            return std::unexpected(ChannelError::GenerationExhausted);
        }
        libk_assert(!free_messages_.empty());
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
            return std::unexpected(ChannelError::ResourceExhausted);
        }
        auto& escrow = message->escrows.back();
        auto made = make_escrow(source, request.caps[index], escrow);
        if (!made) {
            discard_message(*message);
            release_message(*message);
            return std::unexpected(made.error());
        }
    }

    u64 sequence{};
    ChannelError failure{ChannelError::Closed};
    bool enqueued{};
    {
        sync::Lock guard{lock_};
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
                            == std::numeric_limits<u64>::max()
                        || writable_sequence
                            == std::numeric_limits<u64>::max()) {
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
        return std::unexpected(failure);
    }
    notify_ready();
    return (sequence);
}

auto Channel::receive(
    cap::Resolved<Channel>& cap,
    cap::CSpace& destination,
    ChannelRecv& result,
    Waiter* reservation) noexcept
    -> std::expected<void, ChannelError> {
    auto side_value = side_for(cap, cap::Right::Receive);
    if (!side_value) {
        return std::unexpected(ChannelError::Denied);
    }

    for (usize attempt = 0; attempt != 2; ++attempt) {
        usize cap_count{};
        u64 expected_sequence{};
        {
            sync::Lock guard{lock_};
            if (!opened_ || closing_) {
                return std::unexpected(ChannelError::Closed);
            }
            if (!owns_turn_locked(Waiter::Kind::Receive, *side_value, reservation)) {
                return std::unexpected(ChannelError::WouldBlock);
            }
            Side& current = side(*side_value);
            if (current.queue.empty()) {
                if (current.closed) {
                    return std::unexpected(ChannelError::Closed);
                }
                return side(peer(*side_value)).closed
                    ? std::expected<void, ChannelError>{
                          std::unexpected(ChannelError::PeerClosed)}
                    : std::expected<void, ChannelError>{
                          std::unexpected(ChannelError::WouldBlock)};
            }
            const Message& message = current.queue.front();
            cap_count = message.escrows.size();
            if (cap_count > result.receive_limit) {
                return std::unexpected(ChannelError::ResourceExhausted);
            }
            expected_sequence = message.sequence;
        }

        libk::InplaceVector<cap::CSpace::Reservation,
            CHANNEL_MAX_CAPS> reservations{};
        for (usize index = 0; index < cap_count; ++index) {
            auto reserved = destination.reserve();
            if (!reserved) {
                return std::unexpected(cap_error(reserved.error()));
            }
            libk_assert(reservations.try_push_back(std::move(reserved).value()));
        }

        ChannelRecv received{};
        Message* consumed{};
        CommitResult commit = CommitResult::Capacity;
        {
            sync::Lock guard{lock_};
            Side& current = side(*side_value);
            if (!owns_turn_locked(Waiter::Kind::Receive, *side_value, reservation)) {
                return std::unexpected(ChannelError::WouldBlock);
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
                return std::unexpected(ChannelError::ResourceExhausted);
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
        libk_assert(consumed != nullptr);
        if (commit == CommitResult::Invalid) {
            discard_message(*consumed);
            release_message(*consumed);
            notify_ready();
            return std::unexpected(ChannelError::TransferFailed);
        }
        result = received;
        discard_message(*consumed);
        release_message(*consumed);
        notify_ready();
        return {};
    }
    return std::unexpected(ChannelError::Busy);
}

auto Channel::wait(
    cap::Resolved<Channel>&& cap, Wait& waiter, Wait::Kind kind,
    Thread& thread, CpuRegistry& cpus) noexcept
    -> std::expected<void, ChannelError> {
    auto reference = cap.reference();
    if (!reference) return std::unexpected(ChannelError::InvalidCap);
    const auto value = side_for(cap,
        kind == Wait::Kind::Send ? cap::Right::Send : cap::Right::Receive);
    if (!value) return std::unexpected(ChannelError::Denied);
    waiter.kind = kind;
    waiter.side = *value;
    waiter.channel_ref = std::move(reference).value();
    {
        sync::Lock guard{lock_};
        if (!opened_ || closing_) return std::unexpected(ChannelError::Closed);
        ++waiter_count_;
        wait_queue(kind, *value).push_back(waiter);
    }
    {
        sync::Lock guard{lock_};
        ++waiter.references; // Completion, before any producer can publish.
    }
    if (!thread.begin_wait(waiter.completion, cpus)) {
        { sync::Lock guard{lock_}; --waiter.references; }
        finish_waiter(waiter);
        return std::unexpected(ChannelError::Busy);
    }
    {
        sync::Lock guard{lock_};
        ++waiter.references; // View and all its dispatched callbacks.
    }
    const bool attached = static_cast<bool>(cap.attach(waiter.grant_attachment));
    {
        sync::Lock guard{lock_};
        if (!attached) {
            waiter.result = WaitResult{STATUS_INVALID_CAP, 0};
            waiter.state = Waiter::State::Done;
            waiter.grant_detaching = true;
            --waiter.references;
        } else if (waiter.state == Waiter::State::Attaching) {
            waiter.state = Waiter::State::Awaiting;
        }
        waiter.admitted = true;
    }
    (void)arm_waiter(waiter);
    drop_waiter(waiter); // Admission is the last access on this path.
    // The grant callback needs operations=0; the accepted target is retained
    // separately by waiter, so release the admission lease before blocking.
    cap.reset();
    thread.block();
    if (thread.stop_requested() || waiter.result.status == STATUS_CANCELED)
        return std::unexpected(ChannelError::Canceled);
    if (waiter.result.status == STATUS_DENIED)
        return std::unexpected(ChannelError::Denied);
    if (waiter.result.status == STATUS_INVALID_CAP)
        return std::unexpected(ChannelError::InvalidCap);
    return {};
}

auto Channel::close(ChannelSide value) noexcept -> bool {
    if (value != ChannelSide::A && value != ChannelSide::B) {
        return false;
    }
    {
        sync::Lock guard{lock_};
        Side& current = side(value);
        if (current.closed) {
            return false;
        }
        current.closed = true;
        for (u64& sequence : current.sequence) {
            if (sequence != std::numeric_limits<u64>::max()) {
                ++sequence;
            }
        }
        Side& other = side(peer(value));
        for (u64& sequence : other.sequence) {
            if (sequence != std::numeric_limits<u64>::max()) {
                ++sequence;
            }
        }
    }
    notify_ready();
    return true;
}

auto Channel::close(
    cap::Resolved<Channel>& cap) noexcept
    -> std::expected<void, ChannelError> {
    auto side_value = side_for(cap, cap::Right::Close);
    if (!side_value) {
        return std::unexpected(ChannelError::Denied);
    }
    static_cast<void>(close(*side_value));
    return {};
}

auto Channel::bind(
    cap::Resolved<Channel>& cap,
    cap::Resolved<Notification>& notification,
    ChannelCondition condition) noexcept
    -> std::expected<usize, ChannelError> {
    const cap::Right right = condition == ChannelCondition::Writable
        ? cap::Right::Send : cap::Right::Receive;
    auto side_value = side_for(cap, right);
    if (!side_value) {
        return std::unexpected(ChannelError::Denied);
    }
    const auto notif_view = notification.view();
    const auto* const notification_data =
        std::get_if<cap::Badge>(
            &notif_view.data);
    if (notification_data == nullptr
        || !notification.rights().contains(cap::Right::Signal)
        || notification_data->badge == 0) {
        return std::unexpected(ChannelError::Denied);
    }

    Relation* relation{};
    usize index{};
    u64 generation{};
    {
        sync::Lock guard{lock_};
        if (!opened_ || closing_) {
            return std::unexpected(ChannelError::ResourceExhausted);
        }
        for (auto& candidate : relations_) {
            if (candidate.state == Relation::State::Idle) {
                relation = &candidate;
                break;
            }
        }
        if (relation == nullptr) return std::unexpected(ChannelError::ResourceExhausted);
        index = relation->index;
        if (relation->generation == MaxRelGen) {
            return std::unexpected(ChannelError::GenerationExhausted);
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
        return std::unexpected(ChannelError::Busy);
    }
    auto hold = std::move(hold_ref).value().as<Notification>();
    if (!hold) {
        abort_relation(*relation);
        return std::unexpected(ChannelError::Busy);
    }
    relation->notification = std::move(hold).value();
    auto bound = relation->notification->bind(
        relation->source, notification_data->badge);
    if (!bound) {
        abort_relation(*relation);
        return std::unexpected(ChannelError::Busy);
    }
    if (!cap.attach(relation->channel_link.attachment)) {
        abort_relation(*relation);
        return std::unexpected(ChannelError::Busy);
    }
    if (!notification.attach(relation->notification_link.attachment)) {
        abort_relation(*relation);
        return std::unexpected(ChannelError::Busy);
    }
    bool committed{};
    {
        sync::Lock guard{lock_};
        if (relation->state == Relation::State::Attaching && !closing_) {
            relation->state = Relation::State::Attached;
            committed = true;
        }
    }
    if (!committed) {
        abort_relation(*relation);
        return std::unexpected(ChannelError::Busy);
    }
    notify_ready();
    return ((generation << RelBits) | index);
}

auto Channel::arm(
    cap::Resolved<Channel>& cap,
    usize relation_handle,
    u64 observed) noexcept -> std::expected<u64, ChannelError> {
    const usize index = relation_handle & RelMask;
    const u64 generation = relation_handle >> RelBits;
    NotificationSource* signal{};
    u64 sequence{};
    {
        sync::Lock guard{lock_};
        if (index >= relations_.size()) {
            return std::unexpected(ChannelError::InvalidRelation);
        }
        Relation& relation = relation_at(index);
        const cap::Right required = relation.condition
            == ChannelCondition::Writable
            ? cap::Right::Send : cap::Right::Receive;
        const auto side_value = side_for(cap, required);
        if (!side_value) {
            return std::unexpected(ChannelError::Denied);
        }
        const ChannelSide requested_side = *side_value;
        if (relation.state != Relation::State::Attached
            || relation.generation != generation
            || relation.side != requested_side) {
            return std::unexpected(ChannelError::InvalidRelation);
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
    return (sequence);
}

auto Channel::mint(
    cap::Resolved<Channel>& cap,
    cap::CSpace& destination,
    u64 badge,
    cap::Rights rights) noexcept
    -> std::expected<cap::Handle, ChannelError> {
    const auto side_value = side_for(cap, cap::Right::Delegate);
    if (!side_value || badge == 0) {
        return std::unexpected(ChannelError::Denied);
    }
    const auto effective = cap.view();
    const auto* const data = std::get_if<cap::ChanLimit>(
        &effective.data);
    if (data == nullptr || !data->unbound() || !effective.rights.contains(rights)) {
        return std::unexpected(ChannelError::Denied);
    }
    auto reserved = destination.reserve_derivation();
    if (!reserved) {
        return std::unexpected(cap_error(reserved.error()));
    }
    const cap::ChanLimit child_data{
        .side = *side_value,
        .badge = badge,
        .fixed = ~u64{},
    };
    const cap::View ceiling{rights, child_data};
    auto transaction = std::move(reserved).value();
    auto child = cap.lease().mint(std::move(transaction.grant_), ceiling);
    if (!child) {
        return std::unexpected(ChannelError::Denied);
    }
    auto installed = destination.insert(
        std::move(transaction.slot_),
        std::move(child).value(),
        cap::View{rights, child_data});
    if (!installed) {
        return std::unexpected(cap_error(installed.error()));
    }
    return (installed.value());
}

void Channel::retire(object::cleanup&& cleanup) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(!cleanup_);
        cleanup_ = std::move(cleanup);
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
    sync::Lock guard{lock_};
    return side(value).closed;
}

void Channel::finish_waiter(Waiter& waiter) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(!waiter.completion.attached() && waiter.references == 1);
        libk_assert(!waiter.grant_attachment.attached() && !waiter.grant_attachment.busy());
        waiter.state = Waiter::State::Done;
        waiter.references = 0;
        wait_queue(waiter.kind, waiter.side).erase(waiter);
        libk_assert(waiter_count_ != 0);
        --waiter_count_;
    }
    auto channel_ref = std::move(waiter.channel_ref);
    notify_ready();
    try_finish_retire();
}

void Channel::detach_waiter_grant(Waiter& waiter) noexcept {
    {
        sync::Lock guard{lock_};
        if (!waiter.admitted || waiter.grant_detaching) return;
        waiter.grant_detaching = true;
    }
    if (waiter.grant_attachment.detach()) drop_waiter(waiter);
    // Otherwise the last dispatched GrantWork drops the cap reference.
}

void Channel::drop_waiter(Waiter& waiter) noexcept {
    bool publish{};
    {
        sync::Lock guard{lock_};
        libk_assert(waiter.references > 1);
        --waiter.references;
        publish = waiter.references == 1
            && (waiter.state == Waiter::State::Ready || waiter.state == Waiter::State::Done);
        if (publish) libk_assert(waiter.admitted && waiter.grant_detaching);
    }
    // The sole remaining reference belongs to Completion. No callback can
    // access resident storage after it becomes consumable by the continuation.
    if (publish) waiter.completion.signal();
}

void Channel::invalidate_waiter(
    void* context,
    cap::GrantWork&& work,
    cap::GrantInvalidation reason) noexcept {
    libk_assert(context != nullptr && reason == cap::GrantInvalidation::Revoke);
    auto& waiter = *static_cast<Waiter*>(context);
    waiter.owner->waiter_invalidated(waiter, std::move(work));
}

void Channel::release_waiter(void* context) noexcept {
    libk_assert(context != nullptr);
    auto& waiter = *static_cast<Waiter*>(context);
    waiter.owner->drop_waiter(waiter);
}

void Channel::waiter_invalidated(
    Waiter& waiter, cap::GrantWork&& work) noexcept {
    {
        sync::Lock guard{lock_};
        ++waiter.references;
        waiter.result = WaitResult{STATUS_DENIED, 0};
        waiter.state = Waiter::State::Done;
    }
    detach_waiter_grant(waiter);
    work.reset();
    drop_waiter(waiter);
}

void Channel::invalidate(
    void* context,
    cap::GrantWork&& work,
    cap::GrantInvalidation reason) noexcept {
    libk_assert(context != nullptr && reason == cap::GrantInvalidation::Revoke);
    auto& link = *static_cast<GrantLink*>(context);
    link.relation->owner->invalidated(link, std::move(work));
}

void Channel::released(void* context) noexcept {
    libk_assert(context != nullptr);
    auto& link = *static_cast<GrantLink*>(context);
    link.relation->owner->relation_released(*link.relation);
}

void Channel::invalidated(GrantLink& link, cap::GrantWork&& work) noexcept {
    {
        sync::Lock guard{lock_};
        link.work = std::move(work);
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
    libk_assert(context != nullptr && reason == cap::GrantInvalidation::Revoke);
    auto& link = *static_cast<SideLink*>(context);
    link.owner->side_invalidated(link, std::move(work));
}

void Channel::release_side(void* context) noexcept {
    libk_assert(context != nullptr);
    auto& link = *static_cast<SideLink*>(context);
    link.owner->side_released(link);
}

void Channel::side_invalidated(
    SideLink& link,
    cap::GrantWork&& work) noexcept {
    {
        sync::Lock guard{lock_};
        link.work = std::move(work);
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
    object::cleanup done{};
    {
        sync::Lock guard{lock_};
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
        done = std::move(cleanup_);
    }
    done.complete();
}

void Channel::abort_relation(Relation& relation) noexcept {
    bool do_abort{};
    {
        sync::Lock guard{lock_};
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
        sync::Lock guard{lock_};
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
    sync::Lock guard{lock_};
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
                    std::move(escrow.grant),
                    escrow.view)) {
                auto refund = escrow.source->escrow_drop(escrow.source_slot);
                refund.reset();
            }
        } else {
            escrow.grant.reset();
        }
    }
    message.escrows.clear();
}

auto Channel::relation_at(usize index) noexcept -> Relation& {
    libk_assert(index < relations_.size());
    auto it = relations_.begin();
    while (index-- != 0) ++it;
    return *it;
}

void Channel::release_message(Message& message) noexcept {
    {
        sync::Lock guard{lock_};
        auto& target = side(message.destination);
        libk_assert(target.occupied != 0);
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
                sync::Lock guard{lock_};
                Side& current = sides_[side_index_value];
                if (current.queue.empty()) {
                    break;
                }
                message = &current.queue.pop_front();
            }
            libk_assert(message != nullptr);
            discard_message(*message);
            release_message(*message);
        }
    }
}

auto Channel::make_escrow(
    cap::CSpace& source,
    const CapXfer& spec,
    Escrow& escrow) noexcept -> std::expected<void, ChannelError> {
    std::optional<Escrow::Kind> kind{};
    switch (spec.operation) {
    case CAP_COPY:
        kind.emplace(Escrow::Kind::Copy);
        break;
    case CAP_MOVE:
        kind.emplace(Escrow::Kind::Move);
        break;
    case CAP_DELEGATE:
        kind.emplace(Escrow::Kind::Delegate);
        break;
    default:
        break;
    }
    auto rights = cap::Rights::parse(spec.rights, RIGHT_MASK);
    if (!kind || !rights || spec.flags != 0) {
        return std::unexpected(ChannelError::Invalid);
    }
    const cap::Handle source_handle = cap::Handle::from_raw(spec.source);
    auto copied = source.snapshot(source_handle);
    if (!copied) {
        return std::unexpected(cap_error(copied.error()));
    }
    auto snapshot = std::move(copied).value();
    cap::GrantLease lease = std::move(snapshot.lease);
    escrow.source_handle = source_handle;
    escrow.kind = *kind;
    switch (*kind) {
    case Escrow::Kind::Copy: {
        if (!snapshot.view.rights.contains(cap::Right::Duplicate)) {
            return std::unexpected(ChannelError::Denied);
        }
        const cap::View view{*rights, snapshot.view.data};
        auto valid = cap::compose(lease.kind(), snapshot.view, view);
        auto grant = lease.graph().ref(lease.key());
        if (!valid || !grant) {
            return std::unexpected(ChannelError::Denied);
        }
        escrow.grant = std::move(grant).value();
        escrow.view = view;
        break;
    }
    case Escrow::Kind::Delegate: {
        const cap::View ceiling{*rights, snapshot.view.data};
        if (!snapshot.view.rights.contains(cap::Right::Delegate)
            || !cap::attenuates(lease.kind(), snapshot.view, ceiling)) {
            return std::unexpected(ChannelError::Denied);
        }
        auto charge = source.reserve_grant();
        auto target = lease.clone_target();
        auto valid = cap::compose(lease.kind(), ceiling,
            cap::View{*rights, snapshot.view.data});
        if (!charge || !target || !valid) {
            return std::unexpected(ChannelError::ResourceExhausted);
        }
        auto child = lease.graph().derive(
            std::move(charge).value(), lease,
            std::move(target).value(), ceiling);
        if (!child) {
            return std::unexpected(ChannelError::ResourceExhausted);
        }
        escrow.grant = std::move(child).value();
        escrow.view = cap::View{*rights, snapshot.view.data};
        break;
    }
    case Escrow::Kind::Move: {
        if (!rights->empty()) {
            return std::unexpected(ChannelError::Invalid);
        }
        auto moved = source.escrow_move(
            escrow.source_handle,
            escrow.grant,
            escrow.view,
            escrow.source_slot);
        if (!moved) {
            return std::unexpected(cap_error(moved.error()));
        }
        escrow.source = &source;
        break;
    }
    }
    return {};
}

auto Channel::commit_escrows(
    Message& message,
    cap::CSpace& destination,
    libk::InplaceVector<cap::CSpace::Reservation,
        CHANNEL_MAX_CAPS>& reservations,
    ChannelRecv& result) noexcept -> CommitResult {
    if (reservations.size() != message.escrows.size()) {
        return CommitResult::Capacity;
    }

    // Keep a lease for every in-flight Grant through the destination commit.
    // A revoke may race the receive after the message was queued; the lease
    // makes the preflight and publication one indivisible admission window.
    libk::InplaceVector<cap::GrantLease, CHANNEL_MAX_CAPS> leases{};
    for (Escrow& escrow : message.escrows) {
        auto acquired = escrow.grant.acquire();
        if (!acquired) {
            return CommitResult::Invalid;
        }
        auto effective = cap::compose(
            acquired.value().kind(), acquired.value().ceiling(), escrow.view);
        if (!effective || !leases.try_push_back(std::move(acquired).value())) {
            return CommitResult::Invalid;
        }
    }

    {
        sync::Lock guard{destination.lock_};
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
            const cap::Handle handle = reservations[index].handle();
            libk_assert(escrow.grant);
            auto committed = destination.commit_locked(
                reservations[index], std::move(escrow.grant), escrow.view);
            libk_assert(committed);
            result.caps[index] = handle;
        }
    }
    for (Escrow& escrow : message.escrows) {
        if (escrow.kind == Escrow::Kind::Move && escrow.source != nullptr) {
            auto refund = escrow.source->escrow_drop(escrow.source_slot);
            refund.reset();
            escrow.source = nullptr;
        }
    }
    return CommitResult::Committed;
}

} // namespace ipc
