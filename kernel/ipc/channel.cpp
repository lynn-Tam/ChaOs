#include <mm/slab.hpp>
#include <algorithm>
#include <array>
#include <expected>
#include <optional>
#include <cap/grant.hpp>
#include <object/ref.hpp>
#include <object/scope.hpp>
#include <ipc/channel.hpp>

#include <libk/assert.hpp>
#include <libk/types.hpp>
#include <limits>
#include <libk/checked_arithmetic.hpp>
#include <libk/scope_guard.hpp>
#include <utility>
#include <sync.hpp>
#include <uapi/abi.h>

namespace ipc {

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

const cap::AttachOps Channel::side_ops_{
    .invalidate = &Channel::invalidate_side,
    .released = &Channel::release_side,
    .retain = false,
};

Channel::SideLink::SideLink(Channel& channel, ChannelSide value) noexcept
    : owner(&channel),
      side(value),
      attachment(this, Channel::side_ops_) {}

auto Channel::storage_bytes(ChannelConfig config) noexcept -> std::optional<usize> {
    const auto cells = libk::checked_multiply(config.queue_capacity, usize{2});
    if (!cells) return std::nullopt;
    return libk::checked_multiply(mm::Slab<Message, false>::quota_for(*cells).pages, mm::page_size);
}

Channel::Channel(mm::Pmm& pmm, ChannelConfig config) noexcept
    : config_(config),
      messages_(pmm, mm::Slab<Message, false>::quota_for(config.queue_capacity * 2)),
      sides_{Side{*this}, Side{*this}},
      side_links_{
          SideLink{*this, ChannelSide::A},
          SideLink{*this, ChannelSide::B}} {}

Channel::~Channel() noexcept {
    libk_assert(!cleanup_ && !calls_);
    for (const auto& side : sides_) libk_assert(side.queue.empty() && side.free.empty());
    libk_assert(messages_.live_count() == 0);
    for (SideLink& link : side_links_) {
        libk_assert(!link.attachment.attached() && !link.attachment.busy());
        libk_assert(!link.work);
    }
    libk_assert(!opened_ || closing_);
}

void Channel::bind_allocation(
    resource::Allocation& source) noexcept {
    libk_assert(!quota_);
    quota_ = source.quota();
}

auto Channel::open() noexcept -> status_t {
    if (config_.queue_capacity == 0
        || config_.queue_capacity > CHANNEL_MAX_QUEUE
        || config_.max_words == 0
        || config_.max_words > CHANNEL_MAX_WORDS
        || config_.max_caps > CHANNEL_MAX_CAPS) {
        return STATUS_BAD_ARGS;
    }
    {
        sync::Lock guard{lock_};
        if (opened_) {
            return STATUS_BUSY;
        }
    }

    // Creation pays for every message cell. Operational paths only
    // move intrusive links and never grow backing storage.
    usize budget = 64;
    for (usize i = 0; i < 2; ++i) {
        auto& queue = sides_[i];
        while (queue.free.size() < config_.queue_capacity) {
            if (!budget) return STATUS_PENDING;
            --budget;
            auto made = messages_.create(quota_);
            if (!made) return STATUS_NO_MEMORY;
            made.value()->destination = i ? ChannelSide::B : ChannelSide::A;
            sync::Lock guard{queue.lock}; queue.free.push_back(*made.value());
        }
    }
    sync::Lock guard{lock_};
    if (opened_) {
        return STATUS_BUSY;
    }
    if (!initialization_.query().sequence) {
        opened_ = true; guard.restore();
        initialization_.accept(); initialization_.finish();
    }
    return STATUS_OK;
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

void Channel::readiness_locked(Side& direction, std::span<Source::Publication, 2> publications) noexcept {
    libk_assert(direction.lock.held());
    const u64 terminal = direction.sender_closed || direction.receiver_closed ? SOURCE_TERMINAL : 0;
    const u64 rx = ((!direction.queue.empty() || terminal) ? SOURCE_READY : 0) | terminal;
    const u64 tx = ((direction.occupied < config_.queue_capacity || terminal) ? SOURCE_READY : 0) | terminal;
    if (direction.readable != rx) { direction.readable = rx; publications[0] = direction.rx.prepare(rx, true); }
    if (direction.writable.load(std::memory_order_relaxed) != tx) { direction.writable.store(tx, std::memory_order_release); publications[1] = direction.tx.prepare(tx, true); }
}

auto Channel::send(
    cap::Resolved<Channel>& cap,
    cap::CSpace& source,
    const ChanMsg& request) noexcept
    -> std::expected<u64, ChannelError> {
    auto side_value = side_for(cap, cap::Right::Send);
    if (!side_value) {
        return std::unexpected(ChannelError::Denied);
    }
    const auto effective = cap.view();
    const auto* const auth = std::get_if<cap::ChanLimit>(
        &effective.data);
    if (cap.token()) {
        if (request.word_count > config_.max_words || request.cap_count > config_.max_caps)
            return std::unexpected(ChannelError::Invalid);
        return request.cap_count ? send_caps(cap, source, request) : reply(cap, request, {});
    }
    if (auth == nullptr || !auth->exact()) {
        return std::unexpected(ChannelError::Denied);
    }
    if (request.word_count > config_.max_words
        || request.cap_count > config_.max_caps) {
        return std::unexpected(ChannelError::Invalid);
    }

    auto& target = side(peer(*side_value));
    auto available = [&]() noexcept -> std::optional<ChannelError> {
        if (!opened_ || closing_ || target.sender_closed) return ChannelError::Closed;
        if (target.receiver_closed) return ChannelError::PeerClosed;
        if (target.occupied >= config_.queue_capacity) return ChannelError::WouldBlock;
        if (target.sequence == std::numeric_limits<u64>::max()) return ChannelError::GenerationExhausted;
        return {};
    };
    // Pure payload needs no speculative reservation or capability transaction.
    if (!request.cap_count) {
        Source::Publication publications[2];
        u64 sequence{};
        {
            sync::Lock guard{target.lock};
            if (const auto error = available()) return std::unexpected(*error);
            // GrantPin already owns the invocation and object use. Only new
            // capabilities/relations need a late Graph/Gate installation cut.
            auto& message = target.free.pop_front();
            message.called = false;
            message.transaction = request.transaction; message.tag = request.tag;
            message.sender_badge = auth->badge; message.word_count = request.word_count;
            std::copy_n(request.words, request.word_count, message.words);
            sequence = message.sequence = ++target.sequence;
            ++target.occupied; target.queue.push_back(message);
            readiness_locked(target, publications);
        }
        for (auto& publication : publications) publication.publish();
        return sequence;
    }
    return send_caps(cap, source, request);
}

auto Channel::send_caps(cap::Resolved<Channel>& cap, cap::CSpace& source, const ChanMsg& request,
    cap::Capability* reply_cap) noexcept -> std::expected<u64, ChannelError> {
    const auto destination = peer(*side_for(cap, cap::Right::Send));
    if (!cap.token() && !side(destination).writable.load(std::memory_order_acquire)) return std::unexpected(ChannelError::WouldBlock);
    libk::inplace_vector<Escrow, CHANNEL_MAX_CAPS> staging;
    libk::scope_exit discard{[&]() noexcept { discard_escrows(staging.span()); }};
    if (reply_cap) { libk_assert(staging.try_emplace_back()); staging.back().cap = std::move(*reply_cap); }
    for (usize i = 0; i < request.cap_count; ++i) {
        libk_assert(staging.try_emplace_back());
        auto made = make_escrow(source, request.caps[i], staging.back());
        if (!made) {
            if (reply_cap) staging.front().cap.disarm();
            return std::unexpected(made.error());
        }
    }
    auto sent = cap.token() ? this->reply(cap, request, staging.span()) : enqueue(cap, request, staging.span(), reply_cap != nullptr);
    if (sent) staging.clear();
    // An unaccepted CALL owns rollback, not an abandoned invocation.
    else if (reply_cap) staging.front().cap.disarm();
    return sent;
}

auto Channel::enqueue(cap::Resolved<Channel>& cap, const ChanMsg& request, std::span<Escrow> escrows, bool called) noexcept -> std::expected<u64, ChannelError> {
    auto& target = side(peer(*side_for(cap, cap::Right::Send)));
    // Preparation does not authorize a later publication through a closed Gate.
    std::array<cap::GrantPin, CHANNEL_MAX_CAPS> pins;
    std::array<const cap::GrantPin*, CHANNEL_MAX_CAPS + 1> grants;
    std::array<const object::ref<>*, CHANNEL_MAX_CAPS + 1> gates;
    grants[0] = &cap.pin(); gates[0] = &cap.pin().target();
    for (usize i = 0; i < escrows.size(); ++i) {
        auto pin = escrows[i].cap.grant.acquire();
        if (!pin) return std::unexpected(ChannelError::TransferFailed);
        pins[i] = std::move(*pin);
        grants[i + 1] = &pins[i]; gates[i + 1] = &pins[i].target();
    }
    Source::Publication publications[2];
    u64 sequence{};
    {
        sync::Lock guard{target.lock};
        if (!opened_ || closing_ || target.sender_closed) return std::unexpected(ChannelError::Closed);
        if (target.receiver_closed) return std::unexpected(ChannelError::PeerClosed);
        if (target.sequence == std::numeric_limits<u64>::max())
            return std::unexpected(ChannelError::GenerationExhausted);
        if (target.free.empty()) return std::unexpected(ChannelError::WouldBlock);
        const auto count = escrows.size() + 1;
        auto commit = [&] noexcept {
            auto& cell = target.free.pop_front();
            cell.called = called;
            cell.transaction = request.transaction; cell.tag = request.tag;
            cell.sender_badge = std::get<cap::ChanLimit>(cap.view().data).badge; cell.word_count = request.word_count;
            std::copy_n(request.words, request.word_count, cell.words);
            for (auto& escrow : escrows) libk_assert(cell.escrows.try_push_back(std::move(escrow)));
            cell.sequence = sequence = ++target.sequence;
            ++target.occupied; target.queue.push_back(cell);
        };
        const bool admitted = cap.pin().graph().admission(std::span{grants}.first(count))
            .publish(std::span{gates}.first(count), commit) == STATUS_OK;
        if (!admitted) return std::unexpected(ChannelError::InvalidCap);
        readiness_locked(target, publications);
    }
    for (auto& publication : publications) publication.publish();
    return sequence;
}

auto Channel::call(cap::Resolved<Channel>& request_cap, cap::CSpace& source,
    const ChanMsg& request, cap::Resolved<Channel>& transport) noexcept -> std::expected<u64, ChannelError> {
    const auto sender = side_for(request_cap, cap::Right::Send);
    const auto effective = request_cap.view();
    const auto* limit = std::get_if<cap::ChanLimit>(&effective.data);
    if (!sender || request_cap.token() || !limit || !limit->exact()
        || request.word_count > config_.max_words || request.cap_count >= config_.max_caps)
        return std::unexpected(ChannelError::Invalid);
    if (!side(peer(*sender)).writable.load(std::memory_order_acquire))
        return std::unexpected(ChannelError::WouldBlock);
    auto& channel = transport.object();
    const auto reply_side = channel.side_for(transport, cap::Right::Delegate);
    if (!reply_side || transport.token() || !transport.rights().contains(cap::Right::Send))
        return std::unexpected(ChannelError::Denied);
    if (&channel == this && *reply_side == *sender) return std::unexpected(ChannelError::Invalid);
    auto grant = transport.pin().clone();
    auto reference = transport.reference();
    if (!grant || !reference) return std::unexpected(ChannelError::InvalidCap);
    auto typed = std::move(*reference).as<Channel>();
    if (!typed) return std::unexpected(ChannelError::InvalidCap);
    const auto receiver = channel.peer(*reply_side);
    auto& direction = channel.side(receiver);
    Source::Publication publications[2];
    u64 token{};
    {
        sync::Lock lock{direction.lock};
        if (channel.closing_ || direction.sender_closed || direction.receiver_closed)
            return std::unexpected(ChannelError::Closed);
        if (direction.call || direction.occupied || direction.free.empty()) return std::unexpected(ChannelError::Busy);
        if (direction.generation == UINT64_MAX / 2) return std::unexpected(ChannelError::GenerationExhausted);
        token = (++direction.generation << 1) | channel.side_index(receiver);
        direction.call = &direction.free.pop_front(); direction.call_done = false;
        direction.call->error.reset(); direction.call->called = false; ++direction.occupied;
        channel.readiness_locked(direction, publications);
    }
    for (auto& publication : publications) publication.publish();
    cap::Capability response{std::move(*grant), {cap::Rights::of(cap::Right::Send), transport.view().data}};
    response.facet.reset({typed->key(), token, &typed->storage()});
    response.facet.get_deleter().drop = &Channel::abandon;
    bool sent{};
    libk::scope_exit rollback{[&]() noexcept { if (!sent) channel.complete(token, ChannelError::Canceled, true); }};
    auto queued = send_caps(request_cap, source, request, &response);
    if (!queued) return std::unexpected(queued.error());
    sent = true;
    return token;
}

void Channel::abandon(cap::Capability::Facet& facet) noexcept {
    auto object = static_cast<object::pool<Channel>*>(facet.store)->lookup(facet.target);
    if (!object) return; // Object close already owns cancellation and drain.
    auto use = object->admit();
    if (use) object->get().complete(facet.token, ChannelError::Abandoned);
}

auto Channel::complete(u64 token, ChannelError error, bool discard) noexcept -> bool {
    auto& direction = sides_[token & 1];
    Source::Publication publications[2];
    Message* released{};
    {
        sync::Lock lock{direction.lock};
        if (!direction.call || direction.generation != token >> 1 || (!discard && direction.call_done)) return false;
        auto& message = *direction.call;
        if (discard) {
            if (direction.call_done) direction.queue.erase(message);
            direction.call = nullptr; direction.call_done = false; released = &message;
        } else {
            direction.call_done = true; message.error = error;
            message.word_count = 0; message.sequence = ++direction.sequence;
            direction.queue.push_back(message);
        }
        readiness_locked(direction, publications);
    }
    for (auto& publication : publications) publication.publish();
    if (released) { discard_message(*released, true); released->error.reset(); release_message(*released); }
    return true;
}

auto Channel::cancel(cap::Resolved<Channel>& cap, u64 token, bool discard) noexcept -> bool {
    const auto receiver = side_for(cap, cap::Right::Receive);
    return receiver && cap.rights().contains(cap::Right::Close) && !cap.token()
        && side_index(*receiver) == (token & 1) && complete(token, ChannelError::Canceled, discard);
}

auto Channel::reply(cap::Resolved<Channel>& cap, const ChanMsg& request,
    std::span<Escrow> escrows) noexcept -> std::expected<u64, ChannelError> {
    std::array<cap::GrantPin, CHANNEL_MAX_CAPS> pins;
    std::array<const cap::GrantPin*, CHANNEL_MAX_CAPS + 1> grants;
    std::array<const object::ref<>*, CHANNEL_MAX_CAPS + 1> gates;
    grants[0] = &cap.pin();
    gates[0] = &cap.pin().target();
    for (usize i = 0; i < escrows.size(); ++i) {
        auto acquired = escrows[i].cap.grant.acquire();
        if (!acquired) return std::unexpected(ChannelError::TransferFailed);
        pins[i] = std::move(*acquired); grants[i + 1] = &pins[i];
        gates[i + 1] = &pins[i].target();
    }
    auto& source = cap.source();
    auto* slot = source.slot(cap.selector().index());
    if (!slot) return std::unexpected(ChannelError::InvalidCap);
    auto& direction = sides_[cap.token() & 1];
    Source::Publication publications[2];
    {
        // Same ordering as receive: direction -> CSpace slot -> Grant slots.
        sync::Lock queue{direction.lock};
        sync::Lock owner{slot->lock};
        if (slot->generation != cap.selector().generation() || slot->state != cap::CSpace::SlotState::Occupied)
            return std::unexpected(ChannelError::InvalidCap);
        auto& capability = slot->storage.capability;
        if (!capability.facet.get_deleter().drop || capability.facet.get().token != cap.token()
            || capability.grant.key() != cap.grant()) return std::unexpected(ChannelError::InvalidCap);
        if (!direction.call || direction.generation != cap.token() >> 1 || direction.call_done || closing_) {
            capability.disarm(); return std::unexpected(ChannelError::Canceled);
        }
        auto commit = [&]() noexcept {
            auto& message = *direction.call;
            message.called = false;
            message.transaction = request.transaction; message.tag = request.tag;
            message.word_count = request.word_count; message.sender_badge = 0;
            std::copy_n(request.words, request.word_count, message.words);
            for (auto& escrow : escrows) libk_assert(message.escrows.try_push_back(std::move(escrow)));
            direction.call_done = true; message.sequence = ++direction.sequence;
            direction.queue.push_back(message); capability.disarm();
            return true;
        };
        if (escrows.empty()) commit();
        else if (cap.pin().graph().admission(std::span{grants}.first(escrows.size() + 1))
            .publish(std::span{gates}.first(escrows.size() + 1), commit) != STATUS_OK)
            return std::unexpected(ChannelError::TransferFailed);
        readiness_locked(direction, publications);
    }
    for (auto& publication : publications) publication.publish();
    return cap.token();
}

auto Channel::receive(
    cap::Resolved<Channel>& cap, cap::CSpace& destination,
    ChanMsg& result) noexcept -> std::expected<void, ChannelError> {
    const auto value = side_for(cap, cap::Right::Receive);
    if (!value) return std::unexpected(ChannelError::Denied);
    auto& current = side(*value);
    auto copy = [](const Message& message, ChanMsg& received) noexcept {
        received.flags = message.called ? CHANNEL_FLAG_CALL : CHANNEL_FLAGS_NONE;
        received.transaction = message.transaction; received.tag = message.tag;
        received.sender_badge = message.sender_badge; received.word_count = message.word_count;
        std::copy_n(message.words, message.word_count, received.words);
        received.received_count = received.cap_count = message.escrows.size(); received.sequence = message.sequence;
    };
    usize count{};
    u64 sequence{};
    std::optional<ChannelError> completion;
    Source::Publication publications[2];
    {
        sync::Lock guard{current.lock};
        if (!opened_ || closing_) return std::unexpected(ChannelError::Closed);
        if (current.queue.empty()) {
            if (current.receiver_closed) return std::unexpected(ChannelError::Closed);
            return std::unexpected(current.sender_closed ? ChannelError::PeerClosed : ChannelError::WouldBlock);
        }
        auto& message = current.queue.front();
        count = message.escrows.size();
        result.received_count = count;
        if (count > result.receive_limit) return std::unexpected(ChannelError::ResourceExhausted);
        if (!count) {
            copy(message, result);
            completion = message.error; message.error.reset();
            if (current.call == &message) { current.call = nullptr; current.call_done = false; }
            current.free.push_back(current.queue.pop_front());
            --current.occupied;
            readiness_locked(current, publications);
        } else sequence = message.sequence;
    }
    if (!count) {
        for (auto& publication : publications) publication.publish();
        return completion ? std::expected<void, ChannelError>{std::unexpected(*completion)}
                          : std::expected<void, ChannelError>{};
    }
    auto reference = cap.reference();
    if (!reference) return std::unexpected(ChannelError::InvalidCap);
    const cap::GrantPin* grants[]{&cap.pin()};
    const auto admission = cap.pin().graph().admission(grants);
    const object::ref<>* gates[]{&*reference};
    libk::inplace_vector<cap::CSpace::Reservation, CHANNEL_MAX_CAPS> reservations;
    for (usize i = 0; i < count; ++i) {
        auto slot = destination.reserve();
        if (!slot) return std::unexpected(cap_error(slot.error()));
        libk_assert(reservations.try_push_back(std::move(*slot)));
    }
    Message* consumed{};
    bool transferred{};
    {
        sync::Lock guard{current.lock};
        if (!opened_ || closing_) return std::unexpected(ChannelError::Closed);
        if (current.queue.empty() || current.queue.front().sequence != sequence)
            return std::unexpected(ChannelError::WouldBlock);
        auto& message = current.queue.front();
        copy(message, result);
        auto dequeue = [&] noexcept {
            consumed = &current.queue.pop_front();
            if (current.call == consumed) { current.call = nullptr; current.call_done = false; }
        };
        const auto commit = commit_escrows(message, destination, reservations, result,
            admission, gates, Source::Action::bind(dequeue));
        if (!commit) {
            if (commit.error() != ChannelError::TransferFailed) return std::unexpected(commit.error());
            if (admission.publish(gates, dequeue) != STATUS_OK)
                return std::unexpected(ChannelError::InvalidCap);
        } else transferred = true;
        readiness_locked(current, publications);
    }
    for (auto& publication : publications) publication.publish();
    discard_message(*consumed, transferred);
    release_message(*consumed);
    if (!transferred) return std::unexpected(ChannelError::TransferFailed);
    return {};
}

auto Channel::close(ChannelSide value) noexcept -> bool {
    Flight flight{*this};
    if (value != ChannelSide::A && value != ChannelSide::B) {
        return false;
    }
    Source::Publication publications[4];
    {
        sync::Lock first{sides_[0].lock};
        sync::Lock second{sides_[1].lock};
        auto& receiver = side(value);
        auto& sender = side(peer(value));
        if (receiver.receiver_closed) return false;
        receiver.receiver_closed = sender.sender_closed = true;
        for (auto& direction : sides_) if (direction.call && !direction.call_done) {
            auto& message = *direction.call;
            direction.call_done = true; message.error = ChannelError::Abandoned;
            message.word_count = 0; message.sequence = ++direction.sequence;
            direction.queue.push_back(message);
        }
        readiness_locked(sides_[0], std::span{publications}.first<2>());
        readiness_locked(sides_[1], std::span{publications}.last<2>());
    }
    for (auto& publication : publications) publication.publish();
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
    if (!destination.allocation_) return std::unexpected(ChannelError::ResourceExhausted);
    auto slot = destination.reserve();
    auto usage = destination.reserve_grant();
    if (!slot || !usage)
        return std::unexpected(cap_error(!slot ? slot.error() : usage.error()));
    const cap::ChanLimit child_data{
        .side = *side_value,
        .badge = badge,
        .fixed = ~u64{},
    };
    const cap::View ceiling{rights, child_data};
    auto child = cap.pin().mint(std::move(*usage), ceiling);
    if (!child) {
        return std::unexpected(ChannelError::Denied);
    }
    auto installed = destination.insert(
        std::move(*slot),
        std::move(child).value(),
        cap::View{rights, child_data});
    if (!installed) {
        return std::unexpected(cap_error(installed.error()));
    }
    return (installed.value());
}

void Channel::retire(object::cleanup&& cleanup) noexcept {
    Flight flight{*this};
    {
        sync::Lock guard{lock_};
        libk_assert(!cleanup_);
        cleanup_ = std::move(cleanup);
        closing_ = true;
    }
    static_cast<void>(close(ChannelSide::A));
    static_cast<void>(close(ChannelSide::B));
    for (auto& side : sides_) { side.rx.close(); side.tx.close(); }
    for (SideLink& link : side_links_) {
        detach_side(link);
    }
    try_finish_retire();
}

auto Channel::side_state(ChannelSide value) const noexcept -> bool {
    const auto& direction = side(value);
    sync::Lock guard{direction.lock};
    return direction.receiver_closed;
}

void Channel::invalidate_side(
    void* context,
    object::LinkWork&& work) noexcept {
    libk_assert(context != nullptr);
    auto& link = *static_cast<SideLink*>(context);
    link.owner->side_invalidated(link, std::move(work));
}

void Channel::release_side(void* context, object::LinkWork&& done) noexcept {
    libk_assert(context != nullptr);
    auto& link = *static_cast<SideLink*>(context);
    link.owner->side_released(link, std::move(done));
}

void Channel::side_invalidated(
    SideLink& link,
    object::LinkWork&& work) noexcept {
    Flight flight{*this};
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

void Channel::side_released(SideLink& link, object::LinkWork&& terminal) noexcept {
    Flight flight{*this};
    {
        sync::Lock guard{lock_};
        terminal.reset();
        if (!link.attachment.attached() && !link.attachment.busy()) link.attachment.reset();
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

void Channel::finish_call() noexcept {
    { sync::Lock guard{lock_}; libk_assert(calls_); --calls_; }
    try_finish_retire();
}

void Channel::try_finish_retire() noexcept {
    bool retiring;
    { sync::Lock guard{lock_}; retiring = bool(cleanup_); }
    if (retiring) post();
}

void Channel::run_work() noexcept {
    const auto phase = initialization_.query().sequence;
    if (phase && phase != OP_DONE && initialization_.schedule()) return;
    advance();
}

void Channel::advance() noexcept {
    bool retiring;
    {
        sync::Lock guard{lock_};
        if (advancing_) { post(); return; }
        retiring = bool(cleanup_);
        if (retiring && calls_) return;
        advancing_ = true;
    }
    libk::scope_exit advanced{[&]() noexcept { sync::Lock guard{lock_}; advancing_ = false; }};
    const auto phase = initialization_.query().sequence;
    if (phase && phase != OP_DONE) {
        if (retiring) (void)initialization_.cancel();
        if (init_status_ == STATUS_PENDING) {
            init_status_ = initialization_.canceled() ? STATUS_CANCELED : open();
            if (init_status_ == STATUS_PENDING) { initialization_.post(); return; }
            if (init_status_ == STATUS_OK) {
                if (initialization_.commit()) opened_ = true;
                else init_status_ = STATUS_CANCELED;
            }
        }
        if (!initialization_.detach()) { initialization_.post(); return; }
        initialization_.finish(init_status_);
        if (retiring) post();
        return;
    }
    if (!retiring) return;
    if (!clear_queues(8)) { post(); return; }
    object::cleanup done;
    {
        sync::Lock guard{lock_};
        if (!cleanup_ || calls_) return;
        for (const auto& side : sides_) if (!side.rx.drained() || !side.tx.drained()) return;
        for (const SideLink& link : side_links_) {
            if (link.attachment.attached() || link.attachment.busy()
                || link.work) {
                return;
            }
        }
        done = std::move(cleanup_);
    }
    usize budget = 8;
    for (auto& side : sides_) while (budget && !side.free.empty()) { --budget; messages_.destroy(side.free.pop_front()); }
    if (messages_.live_count() || !messages_.drain(8)) {
        { sync::Lock guard{lock_}; cleanup_ = std::move(done); }
        post(); return;
    }
    work_->close(job_);
    initialization_.retire(std::move(done));
}

void Channel::discard_message(Message& message, bool committed) noexcept {
    discard_escrows(message.escrows.span(), committed);
    message.escrows.clear(); message.error.reset();
}
void Channel::discard_escrows(std::span<Escrow> escrows, bool committed) noexcept {
    for (auto& escrow : escrows) {
        if (auto* source = escrow.source_slot.owner_) {
            if (committed || !source->escrow_restore(escrow.source_slot, std::move(escrow.cap))) {
                auto release = source->escrow_drop(escrow.source_slot);
                release.reset();
            }
        }
        escrow.cap = {};
    }
}

void Channel::release_message(Message& message) noexcept {
    auto& target = side(message.destination);
    Source::Publication publications[2];
    {
        sync::Lock guard{target.lock};
        libk_assert(target.occupied != 0);
        --target.occupied;
        target.free.push_back(message);
        readiness_locked(target, publications);
    }
    for (auto& publication : publications) publication.publish();
}

auto Channel::clear_queues(usize budget) noexcept -> bool {
    for (usize side_index_value = 0; side_index_value < 2; ++side_index_value) {
        for (;;) {
            Message* message{};
            {
                Side& current = sides_[side_index_value];
                sync::Lock guard{current.lock};
                if (current.queue.empty()) {
                    break;
                }
                if (!budget) return false;
                --budget;
                message = &current.queue.pop_front();
                if (current.call == message) { current.call = nullptr; current.call_done = false; }
            }
            libk_assert(message != nullptr);
            discard_message(*message);
            release_message(*message);
        }
    }
    return true;
}

auto Channel::make_escrow(
    cap::CSpace& source,
    const CapXfer& spec,
    Escrow& escrow) noexcept -> std::expected<void, ChannelError> {
    enum class Kind { Copy, Delegate, Move };
    std::optional<Kind> kind{};
    switch (spec.operation) {
    case CAP_COPY:
        kind.emplace(Kind::Copy);
        break;
    case CAP_MOVE:
        kind.emplace(Kind::Move);
        break;
    case CAP_DELEGATE:
        kind.emplace(Kind::Delegate);
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
    cap::GrantPin pin = std::move(snapshot.pin);
    if (snapshot.token && *kind != Kind::Move) return std::unexpected(ChannelError::Denied);
    switch (*kind) {
    case Kind::Copy: {
        if (!snapshot.view.rights.contains(cap::Right::Duplicate)) {
            return std::unexpected(ChannelError::Denied);
        }
        const cap::View view{*rights, snapshot.view.data};
        auto valid = cap::compose(pin.kind(), snapshot.view, view);
        auto grant = pin.clone();
        if (!valid || !grant) {
            return std::unexpected(ChannelError::Denied);
        }
        escrow.cap.grant = std::move(grant).value();
        escrow.cap.view = view;
        break;
    }
    case Kind::Delegate: {
        const cap::View ceiling{*rights, snapshot.view.data};
        if (!snapshot.view.rights.contains(cap::Right::Delegate)
            || !cap::attenuates(pin.kind(), snapshot.view, ceiling)) {
            return std::unexpected(ChannelError::Denied);
        }
        auto usage = source.reserve_grant();
        auto target = pin.clone_target();
        auto valid = cap::compose(pin.kind(), ceiling,
            cap::View{*rights, snapshot.view.data});
        if (!usage || !target || !valid) {
            return std::unexpected(ChannelError::ResourceExhausted);
        }
        auto child = pin.graph().derive(
            std::move(usage).value(), pin,
            std::move(target).value(), ceiling);
        if (!child) {
            return std::unexpected(ChannelError::ResourceExhausted);
        }
        escrow.cap.grant = std::move(child).value();
        escrow.cap.view = cap::View{*rights, snapshot.view.data};
        break;
    }
    case Kind::Move: {
        if (!rights->empty()) {
            return std::unexpected(ChannelError::Invalid);
        }
        auto moved = source.escrow_move(
            source_handle,
            escrow.cap,
            escrow.source_slot);
        if (!moved) {
            return std::unexpected(cap_error(moved.error()));
        }
        break;
    }
    }
    return {};
}

auto Channel::commit_escrows(
    Message& message, cap::CSpace& destination,
    libk::inplace_vector<cap::CSpace::Reservation, CHANNEL_MAX_CAPS>& reservations,
    ChanMsg& result, cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates, Source::Action commit) noexcept
    -> std::expected<void, ChannelError> {
    libk_assert(reservations.size() == message.escrows.size());
    libk_assert(!message.escrows.empty());
    std::array<cap::Capability*, CHANNEL_MAX_CAPS> values;
    std::array<cap::CSpace::Reservation*, CHANNEL_MAX_CAPS> slots;
    for (usize i = 0; i < reservations.size(); ++i) {
        values[i] = &message.escrows[i].cap; slots[i] = &reservations[i];
        result.received[i] = reservations[i].handle().raw();
    }
    auto done = destination.install(std::span{values}.first(reservations.size()),
        std::span{slots}.first(reservations.size()), admission, gates, commit);
    if (!done) return std::unexpected(done.error() == cap::CSpaceError::GrantUnavailable
        ? ChannelError::TransferFailed : cap_error(done.error()));
    return {};
}

} // namespace ipc
