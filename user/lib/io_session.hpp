#pragma once

#include <libk/memory.hpp>
#include <libk/optional.hpp>
#include <user/lib/cap_attenuation.hpp>
#include <user/lib/io_queue.hpp>
#include <user/lib/mapped_memory.hpp>
#include <user/lib/service.hpp>

namespace myos::io {

inline constexpr size_t BufferSize = 4096;
inline constexpr size_t PayloadSize = QueueDepth * BufferSize;

// Each endpoint has one control reader and one outstanding control exchange.
// Data submission never uses this Channel's blocking-operation slot.
struct ControlMessage final {
    uint64_t version{QueueVersion};
    uint64_t operation{};
    uint64_t id{};
    int64_t status{};
    uint64_t value{};
    uint64_t size{};
    char data[80]{};
};
static_assert(sizeof(ControlMessage) == MYOS_CHANNEL_MAX_WORDS * sizeof(myos_word_t));

struct ControlPacket final {
    ControlMessage message{};
    cap::OwnedCap capabilities[4]{};
    size_t count{};
    myos_word_t badge{};
};

// A pending reply owns transferred capabilities until the Channel accepts
// the message. Reply backpressure cannot outlive an export's local owner.
struct ControlReply final {
    ControlMessage message{};
    cap::OwnedCap capabilities[4]{};
    myos_word_t rights[4]{};
    size_t count{};

    auto offer(cap::OwnedCap&& capability, myos_word_t granted) noexcept -> bool {
        if (!capability || count == 4) return false;
        rights[count] = granted;
        capabilities[count++] = libk::move(capability);
        return true;
    }
};

class ControlPort final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto open(myos_cap_t channel, myos_cap_t events) noexcept -> myos_status_t {
        const auto bound = channel_bind(channel, events, MYOS_CHANNEL_READABLE);
        if (bound.status != MYOS_STATUS_OK) return bound.status;
        channel_ = channel;
        sequence_ = 0;
        readable_ = bound.value;
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto send(const ControlMessage& message,
        const myos_cap_transfer* capabilities = nullptr, size_t count = 0) noexcept -> myos_status_t {
        return send_to(channel_, message, capabilities, count);
    }

    [[nodiscard]] static auto send_to(myos_cap_t channel, const ControlMessage& message,
        const myos_cap_transfer* capabilities = nullptr, size_t count = 0) noexcept -> myos_status_t {
        if (count > 4) return MYOS_STATUS_BAD_ARGS;
        auto& wire = *reinterpret_cast<myos_channel_message*>(service::IpcAddress);
        wire = {};
        wire.version = MYOS_CHANNEL_VERSION;
        wire.word_count = MYOS_CHANNEL_MAX_WORDS;
        wire.cap_count = count;
        service::copy(wire.words, &message, sizeof(message));
        for (size_t index = 0; index < count; ++index) wire.caps[index] = capabilities[index];
        return channel_try_send(channel).status;
    }

    [[nodiscard]] auto receive(ControlPacket& packet) noexcept -> myos_status_t {
        packet = {};
        auto& wire = *reinterpret_cast<myos_channel_message*>(service::IpcAddress);
        wire = {};
        wire.version = MYOS_CHANNEL_VERSION;
        wire.receive_limit = 4;
        const auto received = channel_try_recv(channel_);
        if (received.status != MYOS_STATUS_OK) return received.status;
        sequence_ = received.value;
        packet.badge = wire.sender_badge;
        // Adopt every committed capability even if the message is invalid.
        // Packet destruction then closes them on all rejection paths.
        packet.count = wire.received_count;
        for (size_t index = 0; index < packet.count; ++index)
            packet.capabilities[index] = cap::OwnedCap{{wire.received[index], 0}};
        if (wire.word_count != MYOS_CHANNEL_MAX_WORDS) return MYOS_STATUS_BAD_ARGS;
        service::copy(&packet.message, wire.words, sizeof(packet.message));
        if (packet.message.version != QueueVersion || packet.message.size > sizeof(packet.message.data))
            return MYOS_STATUS_BAD_ARGS;
        return MYOS_STATUS_OK;
    }

    // Call only after draining control/data work. A sequence mismatch or
    // terminal Channel state retains a hint in the caller's Notification.
    [[nodiscard]] auto arm() noexcept -> myos_status_t {
        const auto result = channel_arm(channel_, readable_, sequence_);
        if (result.status == MYOS_STATUS_OK) sequence_ = result.value;
        return result.status;
    }

private:
    myos_cap_t channel_{};
    myos_word_t readable_{};
    uint64_t sequence_{};
};

// A service slot owns its three resident MemoryObjects. The service retains
// writable initialization authority; each client sees only its producer page
// writable. Revoking that session's export grants precedes buffer reuse.
class ServerMemory final {
public:
    [[nodiscard]] auto create(myos_cap_t pool, myos_cap_t vspace, uintptr_t address) noexcept
        -> myos_status_t {
        if (mappings_[0].memory) return MYOS_STATUS_OK;
        constexpr size_t sizes[] = {4096, 4096, PayloadSize};
        for (size_t index = 0; index < 3; ++index) {
            auto memory = MappedMemory::create(pool, vspace, address + index * 4096, sizes[index]);
            if (!memory) return memory.error();
            mappings_[index] = libk::move(memory).value();
            // Storage roots must not fault for queue/payload storage while
            // completing a downstream request under memory pressure.
            for (size_t offset = 0; offset < sizes[index]; offset += 4096)
                *reinterpret_cast<volatile uint8_t*>(mappings_[index].address + offset) = 0;
        }
        return MYOS_STATUS_OK;
    }

    void initialize() noexcept {
        libk::construct_at(reinterpret_cast<ClientPage*>(mappings_[0].address));
        libk::construct_at(reinterpret_cast<ServerPage*>(mappings_[1].address));
        // A new client must not observe its predecessor's payload bytes.
        auto* bytes = payload();
        for (size_t index = 0; index < PayloadSize; ++index) bytes[index] = 0;
    }

    [[nodiscard]] auto export_pages(myos_cap_t cspace, cap::OwnedCap (&exports)[3]) noexcept
        -> myos_status_t {
        // The tail is padding, outside both ring objects. This is private
        // writable descriptor scratch; peers can only read this page.
        constexpr size_t scratch = 4096 - MYOS_CAP_ATTENUATION_SIZE;
        static_assert(offsetof(ServerPage, submissions) + sizeof(Submissions::Cursor) <= scratch);
        auto& wire = *reinterpret_cast<uint8_t (*)[MYOS_CAP_ATTENUATION_SIZE]>(
            mappings_[1].address + scratch);
        for (size_t index = 0; index < 3; ++index) {
            const myos_cap_attenuation view{
                .version = MYOS_CAP_ATTENUATION_VERSION_CURRENT,
                .kind = MYOS_OBJECT_KIND_MEMORY,
                .size = MYOS_CAP_ATTENUATION_SIZE,
                .rights = MYOS_RIGHT_MAP | MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_REVOKE,
                .words = {0, mappings_[index].size / 4096,
                    index == 0 ? MYOS_VM_READ | MYOS_VM_WRITE : MYOS_VM_READ, MYOS_VM_NORMAL}};
            deploy::attenuation::encode_wire(view, wire);
            const auto exported = cap_typed_delegate(mappings_[index].memory.selector(), cspace,
                mappings_[1].memory.selector(), scratch);
            if (exported.status != MYOS_STATUS_OK) return exported.status;
            exports[index] = cap::OwnedCap{{exported.value, 0}};
        }
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto client() const noexcept -> const ClientPage& {
        return *reinterpret_cast<const ClientPage*>(mappings_[0].address);
    }
    [[nodiscard]] auto server() noexcept -> ServerPage& {
        return *reinterpret_cast<ServerPage*>(mappings_[1].address);
    }
    [[nodiscard]] auto payload() noexcept -> uint8_t* {
        return reinterpret_cast<uint8_t*>(mappings_[2].address);
    }

private:
    MappedMemory mappings_[3]{};
};

class ClientMemory final {
public:
    [[nodiscard]] auto map(myos_cap_t vspace, uintptr_t address, ControlPacket& packet) noexcept
        -> myos_status_t {
        if (packet.count != 4) return MYOS_STATUS_BAD_ARGS;
        constexpr size_t sizes[] = {4096, 4096, PayloadSize};
        MappedMemory mappings[3];
        for (size_t index = 0; index < 3; ++index) {
            auto memory = MappedMemory::map(vspace, libk::move(packet.capabilities[index]),
                address + index * 4096, sizes[index],
                index == 0 ? MYOS_VM_READ | MYOS_VM_WRITE : MYOS_VM_READ);
            if (!memory) return memory.error();
            mappings[index] = libk::move(memory).value();
        }
        for (size_t index = 0; index < 3; ++index) mappings_[index] = libk::move(mappings[index]);
        event_ = libk::move(packet.capabilities[3]);
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto close() noexcept -> myos_status_t {
        for (auto& mapping : mappings_) {
            const auto status = mapping.close();
            if (status != MYOS_STATUS_OK) return status;
        }
        event_ = {};
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto client() noexcept -> ClientPage& {
        return *reinterpret_cast<ClientPage*>(mappings_[0].address);
    }
    [[nodiscard]] auto server() const noexcept -> const ServerPage& {
        return *reinterpret_cast<const ServerPage*>(mappings_[1].address);
    }
    [[nodiscard]] auto payload() const noexcept -> const uint8_t* {
        return reinterpret_cast<const uint8_t*>(mappings_[2].address);
    }
    [[nodiscard]] auto signal() const noexcept -> myos_status_t {
        return notification_signal(event_.selector()).status;
    }

private:
    MappedMemory mappings_[3]{};
    cap::OwnedCap event_{};
};

// Synchronous control is reserved for startup and explicit application calls.
// Forwarding loops use queue() directly and flush once per batch; this wrapper
// deliberately does not own a second table of outstanding data requests.
class ClientSession final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto open(myos_cap_t channel, myos_cap_t events,
        myos_cap_t vspace, uintptr_t address, uint64_t& value) noexcept -> myos_status_t {
        if (queue_) return MYOS_STATUS_BAD_ARGS;
        events_ = events;
        auto status = control_.open(channel, events);
        if (status != MYOS_STATUS_OK) return status;
        const myos_cap_transfer event{events, MYOS_RIGHT_SIGNAL, MYOS_CAP_COPY, 0};
        ControlMessage request{.operation = static_cast<uint64_t>(Control::Open),
            .id = ++next_id_, .value = QueueDepth};
        status = control_.send(request, &event, 1);
        if (status != MYOS_STATUS_OK) return status;
        ControlPacket packet;
        status = receive(request, packet);
        if (status != MYOS_STATUS_OK) return status;
        if (packet.message.status != MYOS_STATUS_OK) return packet.message.status;
        status = memory_.map(vspace, address, packet);
        if (status != MYOS_STATUS_OK) return status;
        queue_.emplace(memory_.client(), memory_.server());
        value = packet.message.value;
        return memory_.signal();
    }

    // A directory capability grants only admission. Replies and data belong
    // to the newly created private channel, so clients never share a reader.
    [[nodiscard]] auto connect(myos_cap_t directory, myos_cap_t pool, myos_cap_t cspace, myos_cap_t events,
        myos_cap_t vspace, uintptr_t address, uint64_t& value) noexcept -> myos_status_t {
        if (queue_ || channel_) return MYOS_STATUS_BUSY;
        const auto pair = channel_create(pool, 1, MYOS_CHANNEL_MAX_WORDS, 4, 2);
        if (pair.status != MYOS_STATUS_OK) return pair.status;
        channel_ = cap::OwnedCap{{pair.value, 0}};
        cap::OwnedCap server{{pair.value2, 0}};
        constexpr auto rights = MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_CLOSE | MYOS_RIGHT_DUPLICATE;
        const auto client_endpoint = channel_mint(channel_.selector(), cspace, 1, rights | MYOS_RIGHT_DESTROY);
        const auto server_endpoint = channel_mint(server.selector(), cspace, 1, rights);
        cap::OwnedCap client_fixed, server_fixed;
        if (client_endpoint.status == MYOS_STATUS_OK) client_fixed = cap::OwnedCap{{client_endpoint.value, 0}};
        if (server_endpoint.status == MYOS_STATUS_OK) server_fixed = cap::OwnedCap{{server_endpoint.value, 0}};
        if (!client_fixed || !server_fixed) {
            (void)object_destroy(channel_.selector());
            channel_ = {};
            return !client_fixed ? client_endpoint.status : server_endpoint.status;
        }
        channel_ = libk::move(client_fixed);
        server = libk::move(server_fixed);
        events_ = events;
        auto status = control_.open(channel_.selector(), events);
        const ControlMessage request{.operation = static_cast<uint64_t>(Control::Open),
            .id = ++next_id_, .value = QueueDepth};
        const myos_cap_transfer transfers[]{
            {server.selector(), MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_CLOSE, MYOS_CAP_COPY, 0},
            {events, MYOS_RIGHT_SIGNAL, MYOS_CAP_COPY, 0}};
        if (status == MYOS_STATUS_OK) status = ControlPort::send_to(directory, request, transfers, 2);
        ControlPacket packet;
        if (status == MYOS_STATUS_OK) status = receive(request, packet);
        if (status == MYOS_STATUS_OK) status = packet.message.status;
        if (status == MYOS_STATUS_OK) status = memory_.map(vspace, address, packet);
        if (status != MYOS_STATUS_OK) {
            (void)object_destroy(channel_.selector());
            channel_ = {};
            const auto closed = memory_.close();
            if (closed != MYOS_STATUS_OK) cap::SyscallBackend::ownership_fault(closed);
            return status;
        }
        queue_.emplace(memory_.client(), memory_.server());
        value = packet.message.value;
        return memory_.signal();
    }

    [[nodiscard]] auto close() noexcept -> myos_status_t {
        if (!queue_ || !channel_) return MYOS_STATUS_BAD_ARGS;
        ControlMessage request{.operation = static_cast<uint64_t>(Control::Close)};
        auto status = exchange(request);
        if (status != MYOS_STATUS_OK) return status;
        queue_.reset();
        status = memory_.close();
        if (status != MYOS_STATUS_OK) return status;
        status = object_destroy(channel_.selector()).status;
        if (status == MYOS_STATUS_OK) channel_ = {};
        return status;
    }

    [[nodiscard]] auto exchange(ControlMessage& message, ControlPacket& packet) noexcept
        -> myos_status_t {
        if (!queue_ || next_id_ == UINT64_MAX) return MYOS_STATUS_BAD_ARGS;
        message.id = ++next_id_;
        auto status = control_.send(message);
        if (status != MYOS_STATUS_OK) return status;
        status = receive(message, packet);
        if (status != MYOS_STATUS_OK) return status;
        message = packet.message;
        status = memory_.signal();
        return status == MYOS_STATUS_OK ? message.status : status;
    }

    [[nodiscard]] auto exchange(ControlMessage& message) noexcept -> myos_status_t {
        ControlPacket packet;
        const auto status = exchange(message, packet);
        return packet.count == 0 ? status : MYOS_STATUS_PEER_FAULT;
    }

    [[nodiscard]] auto queue() noexcept -> ClientQueue& { return *queue_; }
    [[nodiscard]] auto payload() const noexcept -> const uint8_t* { return memory_.payload(); }
    [[nodiscard]] auto flush() noexcept -> myos_status_t {
        const bool submitted = queue_->publish();
        const bool consumed = queue_->release();
        return submitted || consumed ? memory_.signal() : MYOS_STATUS_OK;
    }
    [[nodiscard]] auto arm() noexcept -> myos_status_t { return control_.arm(); }

private:
    [[nodiscard]] auto receive(const ControlMessage& request, ControlPacket& packet) noexcept
        -> myos_status_t {
        for (;;) {
            auto status = control_.receive(packet);
            if (status == MYOS_STATUS_OK)
                return packet.message.id == request.id && packet.message.operation == request.operation
                    ? MYOS_STATUS_OK : MYOS_STATUS_PEER_FAULT;
            if (status != MYOS_STATUS_WOULD_BLOCK && status != MYOS_STATUS_BUSY) return status;
            status = control_.arm();
            if (status != MYOS_STATUS_OK) return status;
            status = notification_wait(events_).status;
            if (status != MYOS_STATUS_OK) return status;
        }
    }

    cap::OwnedCap channel_;
    ControlPort control_;
    ClientMemory memory_;
    libk::optional<ClientQueue> queue_;
    myos_cap_t events_{};
    uint64_t next_id_{};
};

// One lifetime per endpoint. Backend owners stop admission on closing(), end
// all backend access, then pass close_ready to flush(). The common transport
// owns page authority, control reply credit and terminal queue publication.
class ServerSession final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto prepare(myos_cap_t pool, myos_cap_t vspace,
        myos_cap_t cspace, uintptr_t address) noexcept -> myos_status_t {
        cspace_ = cspace;
        return memory_.create(pool, vspace, address);
    }
    [[nodiscard]] auto bind(myos_cap_t channel, myos_cap_t events, uint64_t value = 0) noexcept -> myos_status_t {
        events_ = events;
        value_ = value;
        return control_.open(channel, events);
    }
    [[nodiscard]] auto open(myos_cap_t channel, myos_cap_t events, myos_cap_t pool,
        myos_cap_t vspace, myos_cap_t cspace, uintptr_t address, uint64_t value = 0) noexcept
        -> myos_status_t {
        const auto status = prepare(pool, vspace, cspace, address);
        return status == MYOS_STATUS_OK ? bind(channel, events, value) : status;
    }

    void abort() noexcept {
        closing_ = true;
        if (queue_) queue_->stop();
        peer_event_ = {};
        reply_.reset();
    }
    [[nodiscard]] auto done() const noexcept -> bool {
        if (!closing_ || reply_ || (queue_ && queue_->active() != 0)) return false;
        for (const auto& exported : exports_) if (exported) return false;
        return true;
    }
    void reset() noexcept {
        if (!done()) cap::SyscallBackend::ownership_fault(MYOS_STATUS_BUSY);
        queue_.reset();
        peer_event_ = {};
        closing_ = false;
    }

    template<class Handler>
    [[nodiscard]] auto poll(Handler&& application) noexcept -> myos_status_t {
        if (reply_) return MYOS_STATUS_OK;
        ControlPacket packet;
        const auto status = control_.receive(packet);
        if (status == MYOS_STATUS_WOULD_BLOCK || status == MYOS_STATUS_BUSY) return MYOS_STATUS_OK;
        if (status != MYOS_STATUS_OK) return status;
        return accept(packet, libk::forward<Handler>(application));
    }

    template<class Handler>
    [[nodiscard]] auto accept(ControlPacket& packet, Handler&& application) noexcept -> myos_status_t {
        if (reply_) return MYOS_STATUS_BUSY;
        const auto& message = packet.message;
        reply_.emplace();
        reply_->message = ControlMessage{.operation = message.operation, .id = message.id};
        if (closing_) reply_->message.status = MYOS_STATUS_CLOSED;
        else if (message.operation == static_cast<uint64_t>(Control::Open)) {
            if (queue_ || packet.count != 1 || message.value != QueueDepth || message.size != 0)
                reply_->message.status = MYOS_STATUS_BAD_ARGS;
            else {
                memory_.initialize();
                auto created = memory_.export_pages(cspace_, exports_);
                if (created != MYOS_STATUS_OK) return created;
                for (auto& exported : exports_) {
                    const auto copied = cap_duplicate(exported.selector(), cspace_, MYOS_RIGHT_MAP | MYOS_RIGHT_DUPLICATE);
                    if (copied.status != MYOS_STATUS_OK) return copied.status;
                    if (!reply_->offer(cap::OwnedCap{{copied.value, 0}}, MYOS_RIGHT_MAP)) return MYOS_STATUS_INTERNAL;
                }
                const auto signal = cap_duplicate(events_, cspace_, MYOS_RIGHT_SIGNAL | MYOS_RIGHT_DUPLICATE);
                if (signal.status != MYOS_STATUS_OK) return signal.status;
                if (!reply_->offer(cap::OwnedCap{{signal.value, 0}}, MYOS_RIGHT_SIGNAL)) return MYOS_STATUS_INTERNAL;
                peer_event_ = libk::move(packet.capabilities[0]);
                queue_.emplace(memory_.client(), memory_.server());
                reply_->message.value = value_;
            }
        } else if (packet.count != 0 || !queue_) reply_->message.status = MYOS_STATUS_BAD_ARGS;
        else if (message.operation == static_cast<uint64_t>(Control::Cancel)) {
            Ticket ticket{};
            reply_->message.status = message.size != 0 ? MYOS_STATUS_BAD_ARGS
                : queue_->cancel(message.value, ticket) ? MYOS_STATUS_OK : MYOS_STATUS_NOT_FOUND;
        } else if (message.operation == static_cast<uint64_t>(Control::Close)) {
            if (message.size != 0) reply_->message.status = MYOS_STATUS_BAD_ARGS;
            else {
                queue_->stop();
                closing_ = true;
            }
        } else application(message, *reply_);
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto queue() noexcept -> ServerQueue* { return queue_ ? &*queue_ : nullptr; }
    [[nodiscard]] auto payload() noexcept -> uint8_t* { return memory_.payload(); }
    [[nodiscard]] auto closing() const noexcept -> bool { return closing_; }
    [[nodiscard]] auto failed() const noexcept -> bool { return closing_ && !peer_event_; }
    [[nodiscard]] auto arm() noexcept -> myos_status_t { return control_.arm(); }

    [[nodiscard]] auto flush(bool close_ready) noexcept -> myos_status_t {
        if (queue_) {
            // A visible completion also returns its submission credit, so a
            // client can immediately refill a completed batch.
            (void)queue_->release();
            const bool completed = queue_->publish();
            if (completed && peer_event_) {
                const auto status = notification_signal(peer_event_.selector()).status;
                if (status != MYOS_STATUS_OK) return status;
            }
        }
        if (closing_ && close_ready && (!queue_ || queue_->active() == 0)) {
            // This is the observable end of peer access, before Close's reply.
            // The slot's own mappings use their root grants and remain resident.
            for (auto& exported : exports_) if (exported) {
                const auto status = cap_revoke(exported.selector(), true).status;
                if (status != MYOS_STATUS_OK) return status;
                exported = {};
            }
        }
        if (!reply_ || (closing_ && (!close_ready || queue_->active() != 0))) return MYOS_STATUS_OK;
        myos_cap_transfer transfers[4]{};
        for (size_t index = 0; index < reply_->count; ++index)
            transfers[index] = {reply_->capabilities[index].selector(), reply_->rights[index], MYOS_CAP_COPY, 0};
        const auto status = control_.send(reply_->message, transfers, reply_->count);
        if (status == MYOS_STATUS_OK) reply_.reset();
        return status == MYOS_STATUS_WOULD_BLOCK || status == MYOS_STATUS_BUSY ? MYOS_STATUS_OK : status;
    }

private:
    ControlPort control_;
    ServerMemory memory_;
    libk::optional<ServerQueue> queue_;
    cap::OwnedCap peer_event_;
    cap::OwnedCap exports_[3];
    libk::optional<ControlReply> reply_;
    myos_cap_t events_{}, cspace_{};
    uint64_t value_{};
    bool closing_{};
};

} // namespace myos::io
