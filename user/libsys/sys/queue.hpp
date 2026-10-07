#pragma once

#include <utility>


#include <libk/shared_ring.hpp>
#include <uapi/abi.h>
#include <algorithm>
#include <array>
#include <libk/memory.hpp>
#include <optional>
#include <sys/handle.hpp>
#include <sys/channel.hpp>
#include <expected>

namespace sys::io {

inline constexpr uint64_t QueueVersion = 1;
inline constexpr size_t QueueDepth = 32;
inline constexpr size_t BufferSize = 4096;
inline constexpr size_t PayloadSize = QueueDepth * BufferSize;
using Submissions = libk::SharedRing<8, QueueDepth>;
using Completions = libk::SharedRing<4, QueueDepth>;

// One aligned device read and the file bytes within it.
struct extent final {
    uint64_t offset{};
    size_t size{}, skip{}, bytes{};
};

enum class Operation : uint64_t { Read = 1, Write = 2, Flush = 3, Identify = 4 };
// Cancellation travels over the session's control Channel, so a full data
// queue never prevents it. Its reply acknowledges receipt, not buffer release.
enum class Control : uint64_t { Open = 1, Cancel, Close, Sync, Stat };

struct Request final {
    uint64_t id{};
    uint64_t operation{};
    uint64_t object{};
    uint64_t offset{};
    uint64_t buffer{};
    uint64_t buffer_offset{};
    uint64_t length{};
    uint64_t flags{};

    [[nodiscard]] auto encode() const noexcept -> Submissions::Entry {
        return {{id, operation, object, offset, buffer, buffer_offset, length, flags}};
    }
    [[nodiscard]] static auto decode(const Submissions::Entry& value) noexcept -> Request {
        return {value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7]};
    }
};

struct Completion final {
    uint64_t id{};
    int64_t status{};
    uint64_t bytes{};
    uint64_t flags{};

    [[nodiscard]] auto encode() const noexcept -> Completions::Entry {
        return {{id, static_cast<uint64_t>(status), bytes, flags}};
    }
    [[nodiscard]] static auto decode(const Completions::Entry& value) noexcept -> Completion {
        return {value[0], __builtin_bit_cast(int64_t, value[1]), value[2], value[3]};
    }
};

// The service owns the MemoryObjects and initializes both pages before
// granting access. ClientPage is writable only by the client; ServerPage only
// by the service. Each peer maps the other page read-only. Neither page holds
// payload buffers or capability selectors. Session identity comes from the
// authenticated control exchange, not an untrusted shared header.
struct alignas(4096) ClientPage final {
    Submissions::Data submissions{};
    Completions::Cursor completions{};
};
struct alignas(4096) ServerPage final {
    Completions::Data completions{};
    Submissions::Cursor submissions{};
};
static_assert(sizeof(ClientPage) == 4096 && sizeof(ServerPage) == 4096);

// Owned by one client execution. Publish once per submitted batch and signal
// the server Notification when publish() returns true. After draining a batch,
// release completions and signal again so admission blocked on credits resumes.
class ClientQueue final : private libk::noncopyable_nonmovable {
public:
    ClientQueue(ClientPage& client, const ServerPage& server) noexcept
        : submissions_(client.submissions, server.submissions),
          completions_(server.completions, client.completions) {}

    [[nodiscard]] auto submit(Request& request) noexcept -> libk::RingResult {
        if (next_id_ == UINT64_MAX) return libk::RingResult::Exhausted;
        Request snapshot = request;
        snapshot.id = next_id_ + 1;
        const auto result = submissions_.push(snapshot.encode());
        if (result == libk::RingResult::Ready) {
            request.id = ++next_id_;
        }
        return result;
    }
    [[nodiscard]] auto publish() noexcept -> bool { return submissions_.publish(); }
    [[nodiscard]] auto take(Completion& completion) noexcept -> libk::RingResult {
        Completions::Entry entry{};
        const auto result = completions_.pop(entry);
        if (result == libk::RingResult::Ready) completion = Completion::decode(entry);
        return result;
    }
    [[nodiscard]] auto release() noexcept -> bool { return completions_.release(); }

private:
    Submissions::Producer submissions_;
    Completions::Consumer completions_;
    uint64_t next_id_{};
};

// Reserves payload slots and snapshots for one execution's transfers. The
// connection owns the mapped views; callers retain their domain lifetimes.
class requests final : private libk::noncopyable_nonmovable {
public:
    requests(ClientQueue& queue, const uint8_t* payload, uint8_t* writable) noexcept
        : queue_(queue), payload_(payload), writable_(writable) {}
    [[nodiscard]] auto submit(Request& request, void* data, bool exact = false) noexcept -> status_t {
        if (error_) return error_;
        if (request.length > BufferSize
            || request.offset > UINT64_MAX - request.length) return STATUS_BAD_ARGS;
        const auto op = static_cast<Operation>(request.operation);
        if (op == Operation::Write && request.length && !data) return STATUS_BAD_ARGS;
        if (op == Operation::Write && !writable_) return STATUS_DENIED;
        if (op == Operation::Identify && next()) return STATUS_BUSY;
        for (size_t slot = 0; slot < entries_.size(); ++slot) if (!entries_[slot].request.id) {
            request.buffer_offset = op == Operation::Flush || op == Operation::Identify
                ? 0 : slot * BufferSize;
            if (op == Operation::Write && request.length)
                std::copy_n(static_cast<const uint8_t*>(data), request.length, writable_ + request.buffer_offset);
            const auto status = queue_.submit(request);
            if (status == libk::RingResult::Full) return STATUS_BUSY;
            if (status != libk::RingResult::Ready) return fail(STATUS_PEER_FAULT);
            entries_[slot] = {.request = request, .data = data, .exact = exact};
            return STATUS_OK;
        }
        return STATUS_BUSY;
    }
    void poll() noexcept {
        if (error_) return;
        for (;;) {
            Completion result{};
            const auto status = queue_.take(result);
            if (status == libk::RingResult::Empty) return;
            auto* entry = find(result.id);
            if (status != libk::RingResult::Ready || !entry || entry->ready || result.flags
                || result.bytes > entry->request.length
                || (entry->exact && result.status == STATUS_OK && result.bytes != entry->request.length)) {
                (void)fail(STATUS_PEER_FAULT);
                return;
            }
            const auto op = static_cast<Operation>(entry->request.operation);
            if (entry->data && result.bytes && result.status == STATUS_OK && (op == Operation::Read || op == Operation::Identify))
                std::copy_n(payload_ + entry->request.buffer_offset, result.bytes, static_cast<uint8_t*>(entry->data));
            entry->result = result;
            entry->ready = true;
        }
    }
    // id zero selects any ready result. A result is retired exactly once.
    [[nodiscard]] auto take(Completion& result, uint64_t id = 0) noexcept -> status_t {
        poll();
        for (auto& entry : entries_) if (entry.request.id && entry.ready && (!id || entry.request.id == id)) {
            result = entry.result;
            entry = {};
            return STATUS_OK;
        }
        return STATUS_WOULD_BLOCK;
    }
    [[nodiscard]] auto next(uint64_t after = 0) const noexcept -> const Request* {
        const Request* first{};
        for (const auto& entry : entries_)
            if (entry.request.id > after && (!first || entry.request.id < first->id)) first = &entry.request;
        return first;
    }
    [[nodiscard]] auto error() const noexcept -> status_t { return error_; }
    [[nodiscard]] auto contains(uint64_t id) const noexcept -> bool {
        if (id) for (const auto& entry : entries_) if (entry.request.id == id) return true;
        return false;
    }
    [[nodiscard]] auto fail(status_t status) noexcept -> status_t {
        error_ = status;
        for (auto& entry : entries_) if (entry.request.id && !entry.ready) {
            entry.result = {entry.request.id, status, 0, 0};
            entry.ready = true;
        }
        return status;
    }
private:
    struct entry {
        Request request{};
        void* data{};
        Completion result{};
        bool ready{}, exact{};
    };
    auto find(uint64_t id) noexcept -> entry* {
        if (id) for (auto& entry : entries_) if (entry.request.id == id) return &entry;
        return nullptr;
    }
    ClientQueue& queue_;
    const uint8_t* payload_;
    uint8_t* writable_;
    std::array<entry, QueueDepth> entries_{};
    status_t error_{};
};

enum class Admission : uint8_t { Ready, Empty, Backpressure, Closed, InvalidPeer };

struct Ticket final {
    size_t slot{};
    uint64_t id{};
};

// One service execution owns admission, cancellation and completion. Device
// callbacks carry a Ticket, never a borrowed pointer to a reusable cell. The
// request snapshot is the authority for validation/dispatch; rings only carry
// transport state. This class does not grant buffer or file authority.
class ServerQueue final : private libk::noncopyable_nonmovable {
public:
    ServerQueue(const ClientPage& client, ServerPage& server) noexcept
        : submissions_(client.submissions, server.submissions),
          completions_(server.completions, client.completions) {}

    [[nodiscard]] auto admit(Ticket& ticket) noexcept -> Admission {
        if (stopping_) return Admission::Closed;
        if (!completions_.refresh()) return invalid_peer();
        const auto retained = completions_.produced() - completions_.acknowledged();
        // Every admitted operation reserves its terminal CQ cell until that
        // completion is consumed. Pending storage is therefore also bounded.
        if (retained + active_ == QueueDepth) return Admission::Backpressure;
        Submissions::Entry wire{};
        const auto result = submissions_.pop(wire);
        if (result == libk::RingResult::Empty) return Admission::Empty;
        if (result != libk::RingResult::Ready) return invalid_peer();
        const Request request = Request::decode(wire);
        // IDs increase across the entire session, including completed work.
        // This makes delayed cancellation/completion unable to hit a reuse.
        if (request.id <= last_id_) return invalid_peer();
        for (size_t slot = 0; slot < QueueDepth; ++slot) {
            if (pending_[slot].active) continue;
            pending_[slot] = Pending{request, true, false};
            ++active_;
            last_id_ = request.id;
            ticket = {slot, request.id};
            return Admission::Ready;
        }
        // Only local bookkeeping could violate the reserved-cell invariant.
        __builtin_trap();
    }

    [[nodiscard]] auto request(Ticket ticket) const noexcept -> const Request* {
        return valid(ticket) ? &pending_[ticket.slot].request : nullptr;
    }
    [[nodiscard]] auto cancel(uint64_t id, Ticket& ticket) noexcept -> status_t {
        for (size_t slot = 0; slot < QueueDepth; ++slot) {
            auto& pending = pending_[slot];
            if (pending.active && pending.request.id == id) {
                if (pending.committed) return STATUS_BUSY;
                pending.cancelled = true;
                ticket = {slot, id};
                return STATUS_OK;
            }
        }
        return STATUS_NOT_FOUND;
    }
    // Called at the backend's irreversible boundary, before another control
    // request can run. A committed operation reports its real completion.
    [[nodiscard]] auto commit(Ticket ticket) noexcept -> bool {
        if (!valid(ticket) || pending_[ticket.slot].cancelled) return false;
        pending_[ticket.slot].committed = true;
        return true;
    }
    [[nodiscard]] auto cancelled(Ticket ticket) const noexcept -> bool {
        return valid(ticket) && pending_[ticket.slot].cancelled;
    }

    // Caller first ends all backend and buffer access. A cancellation marker
    // alone is never sufficient. A stale or duplicate Ticket changes nothing.
    [[nodiscard]] auto finish(Ticket ticket, int64_t status, uint64_t bytes) noexcept -> bool {
        if (!valid(ticket)) return false;
        const Completion completion{ticket.id, status, bytes, 0};
        if (completions_.push(completion.encode()) != libk::RingResult::Ready) {
            stopping_ = true;
            return false;
        }
        pending_[ticket.slot] = {};
        --active_;
        return true;
    }
    [[nodiscard]] auto publish() noexcept -> bool { return completions_.publish(); }
    [[nodiscard]] auto release() noexcept -> bool { return submissions_.release(); }
    void stop() noexcept { stopping_ = true; }
    [[nodiscard]] auto active() const noexcept -> size_t { return active_; }

    // A failed/dead peer cannot consume completions. Its session owner first
    // drains backend access (including IOSpace close when required), then drops
    // each exact request through this path. It is not a successful completion.
    [[nodiscard]] auto abandon(Ticket ticket) noexcept -> bool {
        if (!stopping_ || !valid(ticket)) return false;
        pending_[ticket.slot] = {};
        --active_;
        return true;
    }

private:
    struct Pending final {
        Request request{};
        bool active{};
        bool cancelled{};
        bool committed{};
    };
    [[nodiscard]] auto valid(Ticket ticket) const noexcept -> bool {
        return ticket.slot < QueueDepth && pending_[ticket.slot].active
            && pending_[ticket.slot].request.id == ticket.id;
    }
    [[nodiscard]] auto invalid_peer() noexcept -> Admission {
        stopping_ = true;
        return Admission::InvalidPeer;
    }

    Submissions::Consumer submissions_;
    Completions::Producer completions_;
    Pending pending_[QueueDepth]{};
    uint64_t last_id_{};
    size_t active_{};
    bool stopping_{};
};

} // namespace sys::io

namespace sys::io {

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
static_assert(sizeof(ControlMessage) == CHANNEL_MAX_WORDS * sizeof(word_t));

struct ControlPacket final {
    ControlMessage message{};
    cap::OwnedCap capabilities[4]{};
    size_t count{};
    word_t badge{};
};

// A pending reply owns transferred capabilities until the Channel accepts
// the message. Reply backpressure cannot outlive an export's local owner.
struct ControlReply final {
    ControlMessage message{};
    cap::OwnedCap capabilities[4]{};
    word_t rights[4]{};
    size_t count{};

    auto offer(cap::OwnedCap&& capability, word_t granted) noexcept -> bool {
        if (!capability || count == 4) return false;
        rights[count] = granted;
        capabilities[count++] = std::move(capability);
        return true;
    }
};

class ControlPort final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto open(cap_t channel, cap_t events) noexcept -> status_t {
        const auto bound = channel_bind(channel, events, CHANNEL_READABLE);
        if (bound.status != STATUS_OK) return bound.status;
        channel_ = channel;
        sequence_ = 0;
        readable_ = bound.value;
        return STATUS_OK;
    }

    [[nodiscard]] auto send(const ControlMessage& message,
        const CapXfer* capabilities = nullptr, size_t count = 0) noexcept -> status_t {
        return send_to(channel_, message, capabilities, count);
    }

    [[nodiscard]] static auto send_to(cap_t channel, const ControlMessage& message,
        const CapXfer* capabilities = nullptr, size_t count = 0) noexcept -> status_t {
        if (count > 4) return STATUS_BAD_ARGS;
        auto& wire = *reinterpret_cast<ChanMsg*>(service::IpcAddress);
        wire = {};
        wire.version = CHANNEL_VERSION;
        wire.word_count = CHANNEL_MAX_WORDS;
        wire.cap_count = count;
        service::copy(wire.words, &message, sizeof(message));
        for (size_t index = 0; index < count; ++index) wire.caps[index] = capabilities[index];
        return channel_try_send(channel).status;
    }

    [[nodiscard]] auto receive(ControlPacket& packet) noexcept -> status_t {
        packet = {};
        auto& wire = *reinterpret_cast<ChanMsg*>(service::IpcAddress);
        wire = {};
        wire.version = CHANNEL_VERSION;
        wire.receive_limit = 4;
        const auto received = channel_try_recv(channel_);
        if (received.status != STATUS_OK) return received.status;
        sequence_ = received.value;
        packet.badge = wire.sender_badge;
        // Adopt every committed capability even if the message is invalid.
        // Packet destruction then closes them on all rejection paths.
        packet.count = wire.received_count;
        for (size_t index = 0; index < packet.count; ++index)
            packet.capabilities[index] = cap::OwnedCap{{wire.received[index], 0}};
        if (wire.word_count != CHANNEL_MAX_WORDS) return STATUS_BAD_ARGS;
        service::copy(&packet.message, wire.words, sizeof(packet.message));
        if (packet.message.version != QueueVersion || packet.message.size > sizeof(packet.message.data))
            return STATUS_BAD_ARGS;
        return STATUS_OK;
    }

    // Call only after draining control/data work. A sequence mismatch or
    // terminal Channel state retains a hint in the caller's Notification.
    [[nodiscard]] auto arm() noexcept -> status_t {
        const auto result = channel_arm(channel_, readable_, sequence_);
        if (result.status == STATUS_OK) sequence_ = result.value;
        return result.status;
    }

private:
    cap_t channel_{};
    word_t readable_{};
    uint64_t sequence_{};
};

// A service slot owns its three resident MemoryObjects. The service retains
// writable initialization authority; each client sees only its producer page
// writable. Revoking that session's export grants precedes buffer reuse.
class ClientMemory final {
public:
    [[nodiscard]] auto map(cap_t vspace, uintptr_t address, ControlPacket& packet,
        bool writable_payload = false) noexcept
        -> status_t {
        if (packet.count != 4) return STATUS_BAD_ARGS;
        constexpr size_t sizes[] = {4096, 4096, PayloadSize};
        MappedMemory mappings[3];
        for (size_t index = 0; index < 3; ++index) {
            auto memory = MappedMemory::map(vspace, std::move(packet.capabilities[index]),
                address + index * 4096, sizes[index],
                index == 0 || (index == 2 && writable_payload)
                    ? VM_READ | VM_WRITE : VM_READ);
            if (!memory) return memory.error();
            mappings[index] = std::move(memory).value();
        }
        for (size_t index = 0; index < 3; ++index) mappings_[index] = std::move(mappings[index]);
        event_ = std::move(packet.capabilities[3]);
        writable_payload_ = writable_payload;
        return STATUS_OK;
    }

    [[nodiscard]] auto close() noexcept -> status_t {
        for (auto& mapping : mappings_) {
            const auto status = mapping.close();
            if (status != STATUS_OK) return status;
        }
        event_ = {};
        writable_payload_ = false;
        return STATUS_OK;
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
    [[nodiscard]] auto writable_payload() noexcept -> uint8_t* {
        return writable_payload_ ? reinterpret_cast<uint8_t*>(mappings_[2].address) : nullptr;
    }
    [[nodiscard]] auto signal() const noexcept -> status_t {
        return notification_signal(event_.selector()).status;
    }

private:
    MappedMemory mappings_[3]{};
    cap::OwnedCap event_{};
    bool writable_payload_{};
};

// Synchronous control is reserved for startup and explicit application calls.
// Mapped payload ownership and buffered transfers share this connection. Raw
// forwarding consumers own their snapshots and use queue() instead.
class ClientSession final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto adopt(cap::OwnedCap&& channel, cap_t events,
        cap_t vspace, uintptr_t address, uint64_t& value,
        bool writable_payload = false) noexcept -> status_t {
        if (queue_ || channel_ || !channel) return STATUS_BAD_ARGS;
        channel_ = std::move(channel);
        const auto status = open(channel_.selector(), events, vspace, address,
            value, writable_payload);
        if (status != STATUS_OK) {
            (void)object_destroy(channel_.selector());
            channel_ = {};
        }
        return status;
    }
    [[nodiscard]] auto open(cap_t channel, cap_t events,
        cap_t vspace, uintptr_t address, uint64_t& value,
        bool writable_payload = false) noexcept -> status_t {
        if (queue_) return STATUS_BAD_ARGS;
        events_ = events;
        auto status = control_.open(channel, events);
        if (status != STATUS_OK) return status;
        const CapXfer event{events, RIGHT_SIGNAL, CAP_COPY, 0};
        ControlMessage request{.operation = static_cast<uint64_t>(Control::Open),
            .id = ++next_id_, .value = QueueDepth};
        status = control_.send(request, &event, 1);
        if (status != STATUS_OK) return status;
        ControlPacket packet;
        status = receive(request, packet);
        if (status != STATUS_OK) return status;
        if (packet.message.status != STATUS_OK) return packet.message.status;
        status = memory_.map(vspace, address, packet, writable_payload);
        if (status != STATUS_OK) return status;
        queue_.emplace(memory_.client(), memory_.server());
        requests_.emplace(*queue_, memory_.payload(), memory_.writable_payload());
        value = packet.message.value;
        return memory_.signal();
    }

    // A directory capability grants only admission. Replies and data belong
    // to the newly created private channel, so clients never share a reader.
    [[nodiscard]] auto connect(cap_t directory, cap_t pool, cap_t cspace, cap_t events,
        cap_t vspace, uintptr_t address, uint64_t& value,
        bool writable_payload = false) noexcept -> status_t {
        if (queue_ || channel_) return STATUS_BUSY;
        const auto pair = channel_create(pool, 1, CHANNEL_MAX_WORDS, 4, 2);
        if (pair.status != STATUS_OK) return pair.status;
        channel_ = cap::OwnedCap{{pair.value, 0}};
        cap::OwnedCap server{{pair.value2, 0}};
        constexpr auto rights = RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE | RIGHT_DUPLICATE;
        const auto client_endpoint = channel_mint(channel_.selector(), cspace, 1, rights | RIGHT_DESTROY);
        const auto server_endpoint = channel_mint(server.selector(), cspace, 1, rights);
        cap::OwnedCap client_fixed, server_fixed;
        if (client_endpoint.status == STATUS_OK) client_fixed = cap::OwnedCap{{client_endpoint.value, 0}};
        if (server_endpoint.status == STATUS_OK) server_fixed = cap::OwnedCap{{server_endpoint.value, 0}};
        if (!client_fixed || !server_fixed) {
            (void)object_destroy(channel_.selector());
            channel_ = {};
            return !client_fixed ? client_endpoint.status : server_endpoint.status;
        }
        channel_ = std::move(client_fixed);
        server = std::move(server_fixed);
        events_ = events;
        auto status = control_.open(channel_.selector(), events);
        const ControlMessage request{.operation = static_cast<uint64_t>(Control::Open),
            .id = ++next_id_, .value = QueueDepth};
        const CapXfer transfers[]{
            {server.selector(), RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE, CAP_COPY, 0},
            {events, RIGHT_SIGNAL, CAP_COPY, 0}};
        if (status == STATUS_OK) status = ControlPort::send_to(directory, request, transfers, 2);
        ControlPacket packet;
        if (status == STATUS_OK) status = receive(request, packet);
        if (status == STATUS_OK) status = packet.message.status;
        if (status == STATUS_OK) status = memory_.map(vspace, address, packet, writable_payload);
        if (status != STATUS_OK) {
            (void)object_destroy(channel_.selector());
            channel_ = {};
            const auto closed = memory_.close();
            if (closed != STATUS_OK) cap::SyscallBackend::ownership_fault(closed);
            return status;
        }
        queue_.emplace(memory_.client(), memory_.server());
        requests_.emplace(*queue_, memory_.payload(), memory_.writable_payload());
        value = packet.message.value;
        return memory_.signal();
    }

    [[nodiscard]] auto close() noexcept -> status_t {
        if (!queue_ || !channel_) return STATUS_BAD_ARGS;
        ControlMessage request{.operation = static_cast<uint64_t>(Control::Close)};
        const auto status = requests_->error() ? requests_->error() : exchange(request);
        requests_.reset();
        queue_.reset();
        const auto unmapped = memory_.close();
        if (unmapped != STATUS_OK) cap::SyscallBackend::ownership_fault(unmapped);
        const auto destroyed = object_destroy(channel_.selector()).status;
        if (destroyed != STATUS_OK) cap::SyscallBackend::ownership_fault(destroyed);
        channel_ = {};
        return status;
    }

    [[nodiscard]] auto exchange(ControlMessage& message, ControlPacket& packet) noexcept
        -> status_t {
        if (!queue_ || next_id_ == UINT64_MAX) return STATUS_BAD_ARGS;
        message.id = ++next_id_;
        auto status = control_.send(message);
        if (status != STATUS_OK) return status;
        status = receive(message, packet);
        if (status != STATUS_OK) return status;
        message = packet.message;
        status = memory_.signal();
        return status == STATUS_OK ? message.status : status;
    }

    [[nodiscard]] auto exchange(ControlMessage& message) noexcept -> status_t {
        ControlPacket packet;
        const auto status = exchange(message, packet);
        return packet.count == 0 ? status : STATUS_PEER_FAULT;
    }

    [[nodiscard]] auto queue() noexcept -> ClientQueue& { return *queue_; }
    [[nodiscard]] auto requests() noexcept -> io::requests& { return *requests_; }
    [[nodiscard]] auto submit(Request& request, void* data = nullptr, bool exact = false) noexcept -> status_t {
        const auto status = requests_->submit(request, data, exact);
        if (status != STATUS_OK) return status;
        const auto published = flush();
        if (published != STATUS_OK) (void)requests_->fail(published);
        return STATUS_OK;
    }
    [[nodiscard]] auto completion(Completion& result, uint64_t id = 0) noexcept -> status_t {
        requests_->poll();
        if (!requests_->error()) {
            const auto status = flush();
            if (status != STATUS_OK) (void)requests_->fail(status);
        }
        return requests_->take(result, id);
    }
    [[nodiscard]] auto wait(Completion& result, uint64_t id = 0, uint64_t deadline = 0) noexcept -> status_t {
        if (id && !requests_->contains(id)) return STATUS_NOT_FOUND;
        for (;;) {
            const auto status = completion(result, id);
            if (status != STATUS_WOULD_BLOCK || !requests_->next()) return status;
            auto waiting = arm();
            if (waiting == STATUS_OK) waiting = notification_wait(events_, deadline).status;
            if (waiting != STATUS_OK) (void)requests_->fail(waiting);
        }
    }
    [[nodiscard]] auto writable_payload() noexcept -> uint8_t* { return memory_.writable_payload(); }
    [[nodiscard]] auto payload() const noexcept -> const uint8_t* { return memory_.payload(); }
    [[nodiscard]] auto flush() noexcept -> status_t {
        const bool submitted = queue_->publish();
        const bool consumed = queue_->release();
        return submitted || consumed ? memory_.signal() : STATUS_OK;
    }
    [[nodiscard]] auto arm() noexcept -> status_t { return control_.arm(); }

private:
    [[nodiscard]] auto receive(const ControlMessage& request, ControlPacket& packet) noexcept
        -> status_t {
        for (;;) {
            auto status = control_.receive(packet);
            if (status == STATUS_OK)
                return packet.message.id == request.id && packet.message.operation == request.operation
                    ? STATUS_OK : STATUS_PEER_FAULT;
            if (status != STATUS_WOULD_BLOCK && status != STATUS_BUSY) return status;
            status = control_.arm();
            if (status != STATUS_OK) return status;
            status = notification_wait(events_).status;
            if (status != STATUS_OK) return status;
        }
    }

    cap::OwnedCap channel_;
    ControlPort control_;
    ClientMemory memory_;
    std::optional<ClientQueue> queue_;
    std::optional<io::requests> requests_;
    cap_t events_{};
    uint64_t next_id_{};
};

// The caller retains object identity and output until the final callback.
struct read final {
    uint64_t object{}, offset{};
    uint8_t* output{};
    size_t size{}, done{};
    void* context{};
    void (*complete)(read&, status_t) noexcept{};
    bool (*cancelled)(const read&) noexcept{};
    bool active{}, submitted{};
};

// Filesystem mapping stays with its owner. This reader only schedules bounded
// physical reads and copies the authorized bytes from each aligned extent.
// Exact-id completion leaves metadata I/O and other owners' results untouched.
template<class Backend, class Mapper, size_t Credits = QueueDepth>
class reader final {
    static_assert(Credits && Credits <= QueueDepth);
    struct flight {
        io::read* read{};
        uint64_t id{}, buffer{};
        io::extent extent{};
    };
    Backend& backend_;
    Mapper map_;
    std::array<flight, Credits> flights_{};

    static auto cancelled(const io::read& read) noexcept -> bool {
        return read.cancelled && read.cancelled(read);
    }
    static void finish(io::read& read, status_t status) noexcept {
        read.active = read.submitted = false;
        read.complete(read, status);
    }
    void complete(flight& slot, const Completion& result) noexcept {
        const auto flight = slot;
        slot = {};
        auto& read = *flight.read;
        read.submitted = false;
        if (cancelled(read)) finish(read, STATUS_CANCELED);
        else if (result.status != STATUS_OK) finish(read, result.status);
        else if (result.bytes != flight.extent.size) finish(read, STATUS_PEER_FAULT);
        else {
            service::copy(read.output + read.done,
                backend_.payload() + flight.buffer + flight.extent.skip, flight.extent.bytes);
            read.done += flight.extent.bytes;
            if (read.done == read.size) finish(read, STATUS_OK);
        }
    }

public:
    reader(Backend& backend, Mapper map) noexcept : backend_(backend), map_(map) {}

    auto poll() noexcept -> std::expected<bool, status_t> {
        bool progress{};
        for (auto& slot : flights_) if (slot.read) {
            Completion result{};
            const auto status = backend_.completion(result, slot.id);
            if (status == STATUS_WOULD_BLOCK) continue;
            if (status != STATUS_OK) return std::unexpected(status);
            complete(slot, result);
            progress = true;
        }
        return progress;
    }
    auto wait() noexcept -> status_t {
        for (auto& slot : flights_) if (slot.read) {
            Completion result{};
            const auto status = backend_.wait(result, slot.id);
            if (status == STATUS_OK) complete(slot, result);
            return status;
        }
        return STATUS_WOULD_BLOCK;
    }
    auto submit(io::read& read) noexcept -> std::expected<bool, status_t> {
        if (!read.active || read.submitted) return false;
        if (cancelled(read)) { finish(read, STATUS_CANCELED); return true; }
        if (read.done == read.size) { finish(read, STATUS_OK); return true; }
        for (auto& slot : flights_) if (!slot.read) {
            const auto extent = map_(read);
            if (!extent) { finish(read, extent.error()); return true; }
            if (!extent->bytes || extent->bytes > read.size - read.done
                || extent->skip > extent->size || extent->bytes > extent->size - extent->skip)
                return std::unexpected(STATUS_INTERNAL);
            Request request{.operation = static_cast<uint64_t>(Operation::Read),
                .offset = extent->offset, .length = extent->size};
            const auto status = backend_.submit(request);
            if (status == STATUS_BUSY) return false;
            if (status != STATUS_OK) { finish(read, status); return true; }
            read.submitted = true;
            slot = {&read, request.id, request.buffer_offset, *extent};
            return true;
        }
        return false;
    }
};

// One lifetime per endpoint. Backend owners stop admission on closing(), end
// all backend access, then pass close_ready to flush(). The common transport
// owns page authority, control reply credit and terminal queue publication.
} // namespace sys::io
