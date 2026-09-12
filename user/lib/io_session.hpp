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
};

class ControlPort final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto open(myos_cap_t channel, myos_cap_t events) noexcept -> myos_status_t {
        const auto bound = channel_bind(channel, events, MYOS_CHANNEL_READABLE);
        if (bound.status != MYOS_STATUS_OK) return bound.status;
        channel_ = channel;
        readable_ = bound.value;
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto send(const ControlMessage& message,
        const myos_cap_transfer* capabilities = nullptr, size_t count = 0) noexcept -> myos_status_t {
        if (count > 4) return MYOS_STATUS_BAD_ARGS;
        auto& wire = *reinterpret_cast<myos_channel_message*>(service::IpcAddress);
        wire = {};
        wire.version = MYOS_CHANNEL_VERSION;
        wire.word_count = MYOS_CHANNEL_MAX_WORDS;
        wire.cap_count = count;
        service::copy(wire.words, &message, sizeof(message));
        for (size_t index = 0; index < count; ++index) wire.caps[index] = capabilities[index];
        return channel_try_send(channel_).status;
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
        return channel_arm(channel_, readable_, sequence_).status;
    }

private:
    myos_cap_t channel_{};
    myos_word_t readable_{};
    uint64_t sequence_{};
};

// A session owns three service-created MemoryObjects. The service maps its
// client's producer page read-only after initialization; the client receives
// only Map rights, with read-only authority over the server page and payload.
// Task ResourcePool/VSpace own final reclamation, as for MappedMemory.
class ServerMemory final {
public:
    [[nodiscard]] auto create(myos_cap_t pool, myos_cap_t vspace, uintptr_t address) noexcept
        -> myos_status_t {
        constexpr size_t sizes[] = {4096, 4096, PayloadSize};
        for (size_t index = 0; index < 3; ++index) {
            auto memory = MappedMemory::create(pool, vspace, address + index * 4096, sizes[index]);
            if (!memory) return memory.error();
            mappings_[index] = libk::move(memory).value();
        }
        libk::construct_at(reinterpret_cast<ClientPage*>(mappings_[0].address));
        libk::construct_at(reinterpret_cast<ServerPage*>(mappings_[1].address));
        const auto protected_page = vm_protect(mappings_[0].region.selector(),
            mappings_[0].address, 4096, MYOS_VM_READ);
        return protected_page.status == MYOS_STATUS_PENDING ? MYOS_STATUS_OK : protected_page.status;
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
                .rights = MYOS_RIGHT_MAP | MYOS_RIGHT_DUPLICATE,
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
        for (size_t index = 0; index < 3; ++index) {
            auto memory = MappedMemory::map(vspace, libk::move(packet.capabilities[index]),
                address + index * 4096, sizes[index],
                index == 0 ? MYOS_VM_READ | MYOS_VM_WRITE : MYOS_VM_READ);
            if (!memory) return memory.error();
            mappings_[index] = libk::move(memory).value();
        }
        event_ = libk::move(packet.capabilities[3]);
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

    [[nodiscard]] auto exchange(ControlMessage& message) noexcept -> myos_status_t {
        if (!queue_ || next_id_ == UINT64_MAX) return MYOS_STATUS_BAD_ARGS;
        message.id = ++next_id_;
        auto status = control_.send(message);
        if (status != MYOS_STATUS_OK) return status;
        ControlPacket packet;
        status = receive(message, packet);
        if (status != MYOS_STATUS_OK) return status;
        if (packet.count != 0) return MYOS_STATUS_PEER_FAULT;
        message = packet.message;
        status = memory_.signal(); // the peer may be waiting for reply credit
        return status == MYOS_STATUS_OK ? message.status : status;
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
    [[nodiscard]] auto open(myos_cap_t channel, myos_cap_t events, myos_cap_t pool,
        myos_cap_t vspace, myos_cap_t cspace, uintptr_t address, uint64_t value = 0) noexcept
        -> myos_status_t {
        events_ = events;
        pool_ = pool;
        vspace_ = vspace;
        cspace_ = cspace;
        address_ = address;
        value_ = value;
        return control_.open(channel, events);
    }

    template<class Handler>
    [[nodiscard]] auto poll(Handler&& application) noexcept -> myos_status_t {
        if (reply_) return MYOS_STATUS_OK;
        ControlPacket packet;
        const auto status = control_.receive(packet);
        if (status == MYOS_STATUS_WOULD_BLOCK || status == MYOS_STATUS_BUSY) return MYOS_STATUS_OK;
        if (status != MYOS_STATUS_OK) return status;
        const auto& message = packet.message;
        reply_.emplace(ControlMessage{.operation = message.operation, .id = message.id});
        if (closing_) reply_->status = MYOS_STATUS_CLOSED;
        else if (message.operation == static_cast<uint64_t>(Control::Open)) {
            if (queue_ || packet.count != 1 || message.value != QueueDepth || message.size != 0)
                reply_->status = MYOS_STATUS_BAD_ARGS;
            else {
                auto created = memory_.create(pool_, vspace_, address_);
                if (created != MYOS_STATUS_OK) return created;
                created = memory_.export_pages(cspace_, exports_);
                if (created != MYOS_STATUS_OK) return created;
                peer_event_ = libk::move(packet.capabilities[0]);
                queue_.emplace(memory_.client(), memory_.server());
                reply_->value = value_;
            }
        } else if (packet.count != 0 || !queue_) reply_->status = MYOS_STATUS_BAD_ARGS;
        else if (message.operation == static_cast<uint64_t>(Control::Cancel)) {
            Ticket ticket{};
            reply_->status = message.size != 0 ? MYOS_STATUS_BAD_ARGS
                : queue_->cancel(message.value, ticket) ? MYOS_STATUS_OK : MYOS_STATUS_NOT_FOUND;
        } else if (message.operation == static_cast<uint64_t>(Control::Close)) {
            if (message.size != 0) reply_->status = MYOS_STATUS_BAD_ARGS;
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
    [[nodiscard]] auto arm() noexcept -> myos_status_t { return control_.arm(); }

    [[nodiscard]] auto flush(bool close_ready) noexcept -> myos_status_t {
        if (queue_) {
            // A visible completion also returns its submission credit, so a
            // client can immediately refill a completed batch.
            (void)queue_->release();
            const bool completed = queue_->publish();
            if (completed) {
                const auto status = notification_signal(peer_event_.selector()).status;
                if (status != MYOS_STATUS_OK) return status;
            }
        }
        if (!reply_ || (closing_ && (!close_ready || queue_->active() != 0))) return MYOS_STATUS_OK;
        myos_cap_transfer transfers[4]{};
        size_t count{};
        if (exports_[0]) {
            for (size_t index = 0; index < 3; ++index)
                transfers[index] = {exports_[index].selector(), MYOS_RIGHT_MAP, MYOS_CAP_COPY, 0};
            transfers[3] = {events_, MYOS_RIGHT_SIGNAL, MYOS_CAP_COPY, 0};
            count = 4;
        }
        const auto status = control_.send(*reply_, transfers, count);
        if (status == MYOS_STATUS_OK) {
            reply_.reset();
            for (auto& exported : exports_) exported = {};
        }
        return status == MYOS_STATUS_WOULD_BLOCK || status == MYOS_STATUS_BUSY ? MYOS_STATUS_OK : status;
    }

private:
    ControlPort control_;
    ServerMemory memory_;
    libk::optional<ServerQueue> queue_;
    cap::OwnedCap peer_event_;
    cap::OwnedCap exports_[3]{};
    libk::optional<ControlMessage> reply_;
    myos_cap_t events_{}, pool_{}, vspace_{}, cspace_{};
    uintptr_t address_{};
    uint64_t value_{};
    bool closing_{};
};

} // namespace myos::io
