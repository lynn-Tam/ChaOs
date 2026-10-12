#pragma once

#include <util.hpp>
#include <string.h>

#include <utility>

#include <libk/shared_ring.hpp>
#include <uapi/abi.h>
#include <algorithm>
#include <array>
#include <libk/memory.hpp>
#include <optional>
#include <cap.hpp>
#include <ch.hpp>
#include <expected>

inline constexpr size_t io_queue_depth = 32;
inline constexpr size_t io_buffer_size = 4096;
inline constexpr size_t io_payload_size = io_queue_depth * io_buffer_size;
using io_submissions = libk::shared_ring<8, io_queue_depth>;
using io_completions = libk::shared_ring<4, io_queue_depth>;

// One aligned device read and the file bytes within it.
struct io_extent final {
    uint64_t offset{};
    size_t size{}, skip{}, bytes{};
};

enum class io_operation : uint64_t { Read = 1, Write = 2, Flush = 3, Identify = 4 };
// Cancellation travels over the session's control Channel, so a full data
// queue never prevents it. Its reply acknowledges receipt, not buffer release.
enum class io_control : uint64_t { Open = 1, Cancel, Close, Sync, Stat };

struct io_cmd final {
    uint64_t id{};
    uint64_t operation{};
    uint64_t object{};
    uint64_t offset{};
    uint64_t buffer{};
    uint64_t buffer_offset{};
    uint64_t length{};
    uint64_t flags{};

    [[nodiscard]] auto encode() const noexcept -> io_submissions::value_type {
        return {{id, operation, object, offset, buffer, buffer_offset, length, flags}};
    }
    [[nodiscard]] static auto decode(const io_submissions::value_type& value) noexcept -> io_cmd {
        return {value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7]};
    }
};

struct io_done final {
    uint64_t id{};
    int64_t status{};
    uint64_t bytes{};
    uint64_t flags{};

    [[nodiscard]] auto encode() const noexcept -> io_completions::value_type {
        return {{id, static_cast<uint64_t>(status), bytes, flags}};
    }
    [[nodiscard]] static auto decode(const io_completions::value_type& value) noexcept -> io_done {
        return {value[0], __builtin_bit_cast(int64_t, value[1]), value[2], value[3]};
    }
};

// The service owns the MemoryObjects and initializes both pages before
// granting access. io_tx_page is writable only by the client; io_rx_page only
// by the service. Each peer maps the other page read-only. Neither page holds
// payload buffers or capability selectors. Session identity comes from the
// authenticated control exchange, not an untrusted shared header.
struct alignas(4096) io_tx_page final {
    io_submissions::data submissions{};
    io_completions::cursor completions{};
};
struct alignas(4096) io_rx_page final {
    io_completions::data completions{};
    io_submissions::cursor submissions{};
};
static_assert(sizeof(io_tx_page) == 4096 && sizeof(io_rx_page) == 4096);

// Owned by one client execution. Publish once per submitted batch and signal
// the server Notification when publish() returns true. After draining a batch,
// release completions and signal again so admission blocked on credits resumes.
class io_tx final : private libk::noncopyable_nonmovable {
  public:
    io_tx(io_tx_page& client, const io_rx_page& server) noexcept
        : submissions_(client.submissions, server.submissions),
          completions_(server.completions, client.completions) {}

    [[nodiscard]] auto submit(io_cmd& request) noexcept -> libk::ring_status {
        if (next_id_ == UINT64_MAX)
            return libk::ring_status::exhausted;
        io_cmd snapshot = request;
        snapshot.id = next_id_ + 1;
        const auto result = submissions_.push(snapshot.encode());
        if (result == libk::ring_status::ready) {
            request.id = ++next_id_;
        }
        return result;
    }
    [[nodiscard]] auto publish() noexcept -> bool { return submissions_.publish(); }
    [[nodiscard]] auto take(io_done& completion) noexcept -> libk::ring_status {
        io_completions::value_type entry{};
        const auto result = completions_.pop(entry);
        if (result == libk::ring_status::ready)
            completion = io_done::decode(entry);
        return result;
    }
    [[nodiscard]] auto release() noexcept -> bool { return completions_.release(); }

  private:
    io_submissions::producer submissions_;
    io_completions::consumer completions_;
    uint64_t next_id_{};
};

// Reserves payload slots and snapshots for one execution's transfers. The
// connection owns the mapped views; callers retain their domain lifetimes.
class io_requests final : private libk::noncopyable_nonmovable {
  public:
    io_requests(io_tx& queue, const uint8_t* payload, uint8_t* writable) noexcept
        : queue_(queue), payload_(payload), writable_(writable) {}
    [[nodiscard]] auto submit(io_cmd& request, void* data, bool exact = false) noexcept
        -> status_t {
        if (error_)
            return error_;
        if (request.length > io_buffer_size || request.offset > UINT64_MAX - request.length)
            return STATUS_BAD_ARGS;
        const auto op = static_cast<io_operation>(request.operation);
        if (op == io_operation::Write && request.length && !data)
            return STATUS_BAD_ARGS;
        if (op == io_operation::Write && !writable_)
            return STATUS_DENIED;
        if (op == io_operation::Identify && next())
            return STATUS_BUSY;
        for (size_t slot = 0; slot < entries_.size(); ++slot)
            if (!entries_[slot].request.id) {
                request.buffer_offset = op == io_operation::Flush || op == io_operation::Identify
                                            ? 0
                                            : slot * io_buffer_size;
                if (op == io_operation::Write && request.length)
                    std::copy_n(static_cast<const uint8_t*>(data), request.length,
                                writable_ + request.buffer_offset);
                const auto status = queue_.submit(request);
                if (status == libk::ring_status::full)
                    return STATUS_BUSY;
                if (status != libk::ring_status::ready)
                    return fail(STATUS_PEER_FAULT);
                entries_[slot] = {.request = request, .data = data, .exact = exact};
                return STATUS_OK;
            }
        return STATUS_BUSY;
    }
    void poll() noexcept {
        if (error_)
            return;
        for (;;) {
            io_done result{};
            const auto status = queue_.take(result);
            if (status == libk::ring_status::empty)
                return;
            auto* entry = find(result.id);
            if (status != libk::ring_status::ready || !entry || entry->ready || result.flags ||
                result.bytes > entry->request.length ||
                (entry->exact && result.status == STATUS_OK &&
                 result.bytes != entry->request.length)) {
                (void) fail(STATUS_PEER_FAULT);
                return;
            }
            const auto op = static_cast<io_operation>(entry->request.operation);
            if (entry->data && result.bytes && result.status == STATUS_OK &&
                (op == io_operation::Read || op == io_operation::Identify))
                std::copy_n(payload_ + entry->request.buffer_offset, result.bytes,
                            static_cast<uint8_t*>(entry->data));
            entry->result = result;
            entry->ready = true;
        }
    }
    // id zero selects any ready result. A result is retired exactly once.
    [[nodiscard]] auto take(io_done& result, uint64_t id = 0) noexcept -> status_t {
        poll();
        for (auto& entry : entries_)
            if (entry.request.id && entry.ready && (!id || entry.request.id == id)) {
                result = entry.result;
                entry = {};
                return STATUS_OK;
            }
        return STATUS_WOULD_BLOCK;
    }
    [[nodiscard]] auto next(uint64_t after = 0) const noexcept -> const io_cmd* {
        const io_cmd* first{};
        for (const auto& entry : entries_)
            if (entry.request.id > after && (!first || entry.request.id < first->id))
                first = &entry.request;
        return first;
    }
    [[nodiscard]] auto error() const noexcept -> status_t { return error_; }
    [[nodiscard]] auto contains(uint64_t id) const noexcept -> bool {
        if (id)
            for (const auto& entry : entries_)
                if (entry.request.id == id)
                    return true;
        return false;
    }
    [[nodiscard]] auto fail(status_t status) noexcept -> status_t {
        error_ = status;
        for (auto& entry : entries_)
            if (entry.request.id && !entry.ready) {
                entry.result = {entry.request.id, status, 0, 0};
                entry.ready = true;
            }
        return status;
    }

  private:
    struct entry {
        io_cmd request{};
        void* data{};
        io_done result{};
        bool ready{}, exact{};
    };
    auto find(uint64_t id) noexcept -> entry* {
        if (id)
            for (auto& entry : entries_)
                if (entry.request.id == id)
                    return &entry;
        return nullptr;
    }
    io_tx& queue_;
    const uint8_t* payload_;
    uint8_t* writable_;
    std::array<entry, io_queue_depth> entries_{};
    status_t error_{};
};

enum class io_admission : uint8_t { Ready, Empty, Backpressure, Closed, InvalidPeer };

struct io_ticket final {
    size_t slot{};
    uint64_t id{};
};

// One service execution owns admission, cancellation and completion. Device
// callbacks carry a io_ticket, never a borrowed pointer to a reusable cell. The
// request snapshot is the authority for validation/dispatch; rings only carry
// transport state. This class does not grant buffer or file authority.
class io_rx final : private libk::noncopyable_nonmovable {
  public:
    io_rx(const io_tx_page& client, io_rx_page& server) noexcept
        : submissions_(client.submissions, server.submissions),
          completions_(server.completions, client.completions) {}

    [[nodiscard]] auto admit(io_ticket& ticket) noexcept -> io_admission {
        if (stopping_)
            return io_admission::Closed;
        if (!completions_.refresh())
            return invalid_peer();
        const auto retained = completions_.produced() - completions_.acknowledged();
        // Every admitted operation reserves its terminal CQ cell until that
        // completion is consumed. Pending storage is therefore also bounded.
        if (retained + active_ == io_queue_depth)
            return io_admission::Backpressure;
        io_submissions::value_type wire{};
        const auto result = submissions_.pop(wire);
        if (result == libk::ring_status::empty)
            return io_admission::Empty;
        if (result != libk::ring_status::ready)
            return invalid_peer();
        const io_cmd request = io_cmd::decode(wire);
        // IDs increase across the entire session, including completed work.
        // This makes delayed cancellation/completion unable to hit a reuse.
        if (request.id <= last_id_)
            return invalid_peer();
        for (size_t slot = 0; slot < io_queue_depth; ++slot) {
            if (pending_[slot].active)
                continue;
            pending_[slot] = Pending{request, true, false};
            ++active_;
            last_id_ = request.id;
            ticket = {slot, request.id};
            return io_admission::Ready;
        }
        // Only local bookkeeping could violate the reserved-cell invariant.
        __builtin_trap();
    }

    [[nodiscard]] auto request(io_ticket ticket) const noexcept -> const io_cmd* {
        return valid(ticket) ? &pending_[ticket.slot].request : nullptr;
    }
    [[nodiscard]] auto cancel(uint64_t id, io_ticket& ticket) noexcept -> status_t {
        for (size_t slot = 0; slot < io_queue_depth; ++slot) {
            auto& pending = pending_[slot];
            if (pending.active && pending.request.id == id) {
                if (pending.committed)
                    return STATUS_BUSY;
                pending.cancelled = true;
                ticket = {slot, id};
                return STATUS_OK;
            }
        }
        return STATUS_NOT_FOUND;
    }
    // Called at the backend's irreversible boundary, before another control
    // request can run. A committed operation reports its real completion.
    [[nodiscard]] auto commit(io_ticket ticket) noexcept -> bool {
        if (!valid(ticket) || pending_[ticket.slot].cancelled)
            return false;
        pending_[ticket.slot].committed = true;
        return true;
    }
    [[nodiscard]] auto cancelled(io_ticket ticket) const noexcept -> bool {
        return valid(ticket) && pending_[ticket.slot].cancelled;
    }

    // Caller first ends all backend and buffer access. A cancellation marker
    // alone is never sufficient. A stale or duplicate io_ticket changes nothing.
    [[nodiscard]] auto finish(io_ticket ticket, int64_t status, uint64_t bytes) noexcept -> bool {
        if (!valid(ticket))
            return false;
        const io_done completion{ticket.id, status, bytes, 0};
        if (completions_.push(completion.encode()) != libk::ring_status::ready) {
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
    [[nodiscard]] auto abandon(io_ticket ticket) noexcept -> bool {
        if (!stopping_ || !valid(ticket))
            return false;
        pending_[ticket.slot] = {};
        --active_;
        return true;
    }

  private:
    struct Pending final {
        io_cmd request{};
        bool active{};
        bool cancelled{};
        bool committed{};
    };
    [[nodiscard]] auto valid(io_ticket ticket) const noexcept -> bool {
        return ticket.slot < io_queue_depth && pending_[ticket.slot].active &&
               pending_[ticket.slot].request.id == ticket.id;
    }
    [[nodiscard]] auto invalid_peer() noexcept -> io_admission {
        stopping_ = true;
        return io_admission::InvalidPeer;
    }

    io_submissions::consumer submissions_;
    io_completions::producer completions_;
    Pending pending_[io_queue_depth]{};
    uint64_t last_id_{};
    size_t active_{};
    bool stopping_{};
};

// Both participants own the same three mappings; only their granted permissions
// and request obligations differ. Events belong to sessions, not mapped memory.
class io_mem final {
    static constexpr std::array<size_t, 3> sizes{4096, 4096, io_payload_size};
    std::array<vm_area, 3> pages_{};
    template <class F> auto acquire(F&& make) -> status_t {
        if (pages_[0].memory)
            return STATUS_BUSY;
        std::array<vm_area, 3> pages;
        for (size_t i = 0; i < pages.size(); ++i) {
            auto page = make(i, sizes[i]);
            if (!page)
                return page.error();
            pages[i] = std::move(*page);
        }
        pages_ = std::move(pages);
        return STATUS_OK;
    }

  public:
    auto create(cap_t pool, cap_t vm, uintptr_t address) -> status_t {
        if (pages_[0].memory)
            return STATUS_OK;
        return acquire([&](size_t i, size_t size) {
            auto page = vm_area::create(pool, vm, address ? address + i * 4096 : 0, size);
            if (page)
                for (size_t n = 0; n < size; n += 4096)
                    *reinterpret_cast<volatile uint8_t*>(page->address + n) = 0;
            return page;
        });
    }
    auto map(cap_t vm, uintptr_t address, std::span<cap_owner, 3> caps, bool writable_payload)
        -> status_t {
        return acquire([&](size_t i, size_t size) {
            return vm_area::map(vm, std::move(caps[i]), address ? address + i * 4096 : 0, size,
                                i == 0 || (i == 2 && writable_payload) ? VM_READ | VM_WRITE
                                                                       : VM_READ);
        });
    }
    auto close() -> status_t {
        for (auto& page : pages_) {
            const auto s = page.close();
            if (s != STATUS_OK)
                return s;
        }
        return STATUS_OK;
    }
    void initialize() noexcept {
        libk::construct_at(reinterpret_cast<io_tx_page*>(pages_[0].address));
        libk::construct_at(reinterpret_cast<io_rx_page*>(pages_[1].address));
        // A new client must not observe its predecessor's payload bytes.
        auto* bytes = payload();
        for (size_t index = 0; index < io_payload_size; ++index)
            bytes[index] = 0;
    }

    [[nodiscard]] auto export_pages(cap_t cspace, cap_owner (&exports)[3],
                                    bool writable_payload = false) noexcept -> status_t {
        // The tail is padding, outside both ring objects. This is private
        // writable descriptor scratch; peers can only read this page.
        constexpr size_t scratch = 4096 - CAP_ATTENUATION_SIZE;
        static_assert(offsetof(io_rx_page, submissions) + sizeof(io_submissions::cursor) <=
                      scratch);
        auto& wire =
            *reinterpret_cast<uint8_t (*)[CAP_ATTENUATION_SIZE]>(pages_[1].address + scratch);
        for (size_t index = 0; index < 3; ++index) {
            const CapView view{.version = CAP_ATTENUATION_VERSION_CURRENT,
                               .kind = ObjMem,
                               .size = CAP_ATTENUATION_SIZE,
                               .rights = RIGHT_MAP | RIGHT_DUPLICATE | RIGHT_REVOKE,
                               .words = {0, pages_[index].size / 4096,
                                         index == 0 || (index == 2 && writable_payload)
                                             ? VM_READ | VM_WRITE
                                             : VM_READ}};
            cap_encode(view, wire);
            const auto exported = sys::cap_typed_delegate(pages_[index].memory.selector(), cspace,
                                                          pages_[1].memory.selector(), scratch);
            if (exported.status != STATUS_OK)
                return exported.status;
            exports[index] = cap_owner{exported.value};
        }
        return STATUS_OK;
    }

    auto client() noexcept -> io_tx_page& {
        return *reinterpret_cast<io_tx_page*>(pages_[0].address);
    }
    auto client() const noexcept -> const io_tx_page& {
        return *reinterpret_cast<const io_tx_page*>(pages_[0].address);
    }
    auto server() noexcept -> io_rx_page& {
        return *reinterpret_cast<io_rx_page*>(pages_[1].address);
    }
    auto server() const noexcept -> const io_rx_page& {
        return *reinterpret_cast<const io_rx_page*>(pages_[1].address);
    }
    auto payload() noexcept -> uint8_t* { return reinterpret_cast<uint8_t*>(pages_[2].address); }
    auto payload() const noexcept -> const uint8_t* {
        return reinterpret_cast<const uint8_t*>(pages_[2].address);
    }
};

// Synchronous control is reserved for startup and explicit application calls.
// Mapped payload ownership and buffered transfers share this connection. Raw
// forwarding consumers own their snapshots and use queue() instead.
class io_client final : private libk::noncopyable_nonmovable {
  public:
    [[nodiscard]] auto adopt(cap_owner&& channel, cap_t events, cap_t vspace, uintptr_t address,
                             uint64_t& value, bool writable_payload = false) noexcept -> status_t {
        if (queue_ || channel_ || !channel)
            return STATUS_BAD_ARGS;
        channel_ = std::move(channel);
        const auto status =
            open(channel_.selector(), events, vspace, address, value, writable_payload);
        if (status != STATUS_OK) {
            if (const auto rc = control_.close(); rc != STATUS_OK)
                sys::exit(rc);
            (void) sys::object_destroy(channel_.selector());
            channel_ = {};
        }
        return status;
    }
    [[nodiscard]] auto open(cap_t channel, cap_t events, cap_t vspace, uintptr_t address,
                            uint64_t& value, bool writable_payload = false) noexcept -> status_t {
        if (queue_)
            return STATUS_BAD_ARGS;
        events_ = events;
        auto status = control_.open(channel, events);
        if (status != STATUS_OK)
            return status;
        const CapXfer event{events, RIGHT_SIGNAL, CAP_COPY, 0};
        ch_msg request{.operation = static_cast<uint64_t>(io_control::Open),
                       .id = ++next_id_,
                       .value = io_queue_depth};
        status = control_.send(request, {&event, 1}, false).status;
        if (status != STATUS_OK)
            return status;
        ch_packet packet;
        status = receive(request, packet);
        if (status != STATUS_OK)
            return status;
        if (packet.message.status != STATUS_OK)
            return packet.message.status;
        status = map_pages(vspace, address, packet, writable_payload);
        if (status != STATUS_OK)
            return status;
        queue_.emplace(memory_.client(), std::as_const(memory_).server());
        requests_.emplace(*queue_, std::as_const(memory_).payload(), this->writable_payload());
        value = packet.message.value;
        return sys::notification_signal(peer_event_.selector()).status;
    }

    // A directory capability grants only admission. Replies and data belong
    // to the newly created private channel, so clients never share a reader.
    [[nodiscard]] auto connect(cap_t directory, cap_t pool, cap_t cspace, cap_t events,
                               cap_t vspace, uintptr_t address, uint64_t& value,
                               bool writable_payload = false) noexcept -> status_t {
        if (queue_ || channel_)
            return STATUS_BUSY;
        const auto pair = sys::channel_create(pool, 1, CHANNEL_MAX_WORDS, 4, 2);
        if (pair.status != STATUS_OK)
            return pair.status;
        channel_ = cap_owner{pair.value};
        cap_owner server{pair.value2};
        constexpr auto rights = RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE | RIGHT_DUPLICATE;
        const auto client_endpoint =
            sys::channel_mint(channel_.selector(), cspace, 1, rights | RIGHT_DESTROY);
        const auto server_endpoint = sys::channel_mint(server.selector(), cspace, 1, rights);
        cap_owner client_fixed, server_fixed;
        if (client_endpoint.status == STATUS_OK)
            client_fixed = cap_owner{client_endpoint.value};
        if (server_endpoint.status == STATUS_OK)
            server_fixed = cap_owner{server_endpoint.value};
        if (!client_fixed || !server_fixed) {
            if (const auto rc = control_.close(); rc != STATUS_OK)
                sys::exit(rc);
            (void) sys::object_destroy(channel_.selector());
            channel_ = {};
            return !client_fixed ? client_endpoint.status : server_endpoint.status;
        }
        channel_ = std::move(client_fixed);
        server = std::move(server_fixed);
        events_ = events;
        auto status = control_.open(channel_.selector(), events);
        const ch_msg request{.operation = static_cast<uint64_t>(io_control::Open),
                             .id = ++next_id_,
                             .value = io_queue_depth};
        const CapXfer transfers[]{
            {server.selector(), RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE, CAP_COPY, 0},
            {events, RIGHT_SIGNAL, CAP_COPY, 0}};
        if (status == STATUS_OK)
            status = ch_port::send_to(directory, request, transfers, 2);
        ch_packet packet;
        if (status == STATUS_OK)
            status = receive(request, packet);
        if (status == STATUS_OK)
            status = packet.message.status;
        if (status == STATUS_OK)
            status = map_pages(vspace, address, packet, writable_payload);
        if (status != STATUS_OK) {
            if (const auto rc = control_.close(); rc != STATUS_OK)
                sys::exit(rc);
            (void) sys::object_destroy(channel_.selector());
            channel_ = {};
            const auto closed = memory_.close();
            if (closed != STATUS_OK)
                sys::exit(closed);
            return status;
        }
        queue_.emplace(memory_.client(), std::as_const(memory_).server());
        requests_.emplace(*queue_, std::as_const(memory_).payload(), this->writable_payload());
        value = packet.message.value;
        return sys::notification_signal(peer_event_.selector()).status;
    }

    [[nodiscard]] auto close() noexcept -> status_t {
        if (!queue_ || !channel_)
            return STATUS_BAD_ARGS;
        ch_msg request{.operation = static_cast<uint64_t>(io_control::Close)};
        const auto status = requests_->error() ? requests_->error() : exchange(request);
        requests_.reset();
        queue_.reset();
        const auto unmapped = memory_.close();
        if (unmapped != STATUS_OK)
            sys::exit(unmapped);
        if (const auto rc = control_.close(); rc != STATUS_OK)
            sys::exit(rc);
        const auto destroyed = sys::object_destroy(channel_.selector()).status;
        if (destroyed != STATUS_OK)
            sys::exit(destroyed);
        channel_ = {};
        peer_event_ = {};
        writable_payload_ = false;
        return status;
    }

    [[nodiscard]] auto exchange(ch_msg& message, ch_packet& packet) noexcept -> status_t {
        if (!queue_ || next_id_ == UINT64_MAX)
            return STATUS_BAD_ARGS;
        message.id = ++next_id_;
        auto status = control_.send(message, {}, false).status;
        if (status != STATUS_OK)
            return status;
        status = receive(message, packet);
        if (status != STATUS_OK)
            return status;
        message = packet.message;
        status = sys::notification_signal(peer_event_.selector()).status;
        return status == STATUS_OK ? message.status : status;
    }

    [[nodiscard]] auto exchange(ch_msg& message) noexcept -> status_t {
        ch_packet packet;
        const auto status = exchange(message, packet);
        return packet.count == 0 ? status : STATUS_PEER_FAULT;
    }

    [[nodiscard]] auto queue() noexcept -> io_tx& { return *queue_; }
    [[nodiscard]] auto requests() noexcept -> io_requests& { return *requests_; }
    [[nodiscard]] auto submit(io_cmd& request, void* data = nullptr, bool exact = false) noexcept
        -> status_t {
        const auto status = requests_->submit(request, data, exact);
        if (status != STATUS_OK)
            return status;
        const auto published = flush();
        if (published != STATUS_OK)
            (void) requests_->fail(published);
        return STATUS_OK;
    }
    [[nodiscard]] auto completion(io_done& result, uint64_t id = 0) noexcept -> status_t {
        requests_->poll();
        if (!requests_->error()) {
            const auto status = flush();
            if (status != STATUS_OK)
                (void) requests_->fail(status);
        }
        return requests_->take(result, id);
    }
    [[nodiscard]] auto wait(io_done& result, uint64_t id = 0, uint64_t deadline = 0) noexcept
        -> status_t {
        if (id && !requests_->contains(id))
            return STATUS_NOT_FOUND;
        for (;;) {
            const auto status = completion(result, id);
            if (status != STATUS_WOULD_BLOCK || !requests_->next())
                return status;
            auto waiting = arm();
            if (waiting == STATUS_OK)
                waiting = sys::notification_wait(events_, deadline).status;
            if (waiting != STATUS_OK)
                (void) requests_->fail(waiting);
        }
    }
    [[nodiscard]] auto writable_payload() noexcept -> uint8_t* {
        return writable_payload_ ? memory_.payload() : nullptr;
    }
    [[nodiscard]] auto payload() const noexcept -> const uint8_t* { return memory_.payload(); }
    [[nodiscard]] auto flush() noexcept -> status_t {
        const bool submitted = queue_->publish();
        const bool consumed = queue_->release();
        return submitted || consumed ? sys::notification_signal(peer_event_.selector()).status
                                     : STATUS_OK;
    }
    [[nodiscard]] auto arm() noexcept -> status_t { return control_.arm().status; }

  private:
    auto map_pages(cap_t vm, uintptr_t address, ch_packet& packet, bool writable) -> status_t {
        if (packet.count != 4)
            return STATUS_BAD_ARGS;
        const auto s =
            memory_.map(vm, address, std::span{packet.capabilities}.first<3>(), writable);
        if (s == STATUS_OK) {
            peer_event_ = std::move(packet.capabilities[3]);
            writable_payload_ = writable;
        }
        return s;
    }

    [[nodiscard]] auto receive(const ch_msg& request, ch_packet& packet) noexcept -> status_t {
        for (;;) {
            auto status = control_.receive(packet);
            if (status == STATUS_OK)
                return packet.message.id == request.id &&
                               packet.message.operation == request.operation
                           ? STATUS_OK
                           : STATUS_PEER_FAULT;
            if (status != STATUS_WOULD_BLOCK && status != STATUS_BUSY)
                return status;
            status = control_.arm().status;
            if (status != STATUS_OK)
                return status;
            status = sys::notification_wait(events_).status;
            if (status != STATUS_OK)
                return status;
        }
    }

    cap_owner channel_;
    ch_port control_;
    io_mem memory_;
    cap_owner peer_event_;
    bool writable_payload_{};
    std::optional<io_tx> queue_;
    std::optional<io_requests> requests_;
    cap_t events_{};
    uint64_t next_id_{};
};

// The caller retains object identity and output until the final callback.
struct io_read final {
    uint64_t object{}, offset{};
    uint8_t* output{};
    size_t size{}, done{};
    void* context{};
    void (*complete)(io_read&, status_t) noexcept {};
    bool (*cancelled)(const io_read&) noexcept {};
    bool active{}, submitted{};
};

// Filesystem mapping stays with its owner. This reader only schedules bounded
// physical reads and copies the authorized bytes from each aligned extent.
// Exact-id completion leaves metadata I/O and other owners' results untouched.
template <class Backend, class Mapper, size_t Credits = io_queue_depth> class io_reader final {
    static_assert(Credits && Credits <= io_queue_depth);
    struct flight {
        io_read* read{};
        uint64_t id{}, buffer{};
        io_extent extent{};
    };
    Backend& backend_;
    Mapper map_;
    std::array<flight, Credits> flights_{};

    static auto cancelled(const io_read& read) noexcept -> bool {
        return read.cancelled && read.cancelled(read);
    }
    static void finish(io_read& read, status_t status) noexcept {
        read.active = read.submitted = false;
        read.complete(read, status);
    }
    void complete(flight& slot, const io_done& result) noexcept {
        const auto flight = slot;
        slot = {};
        auto& read = *flight.read;
        read.submitted = false;
        if (cancelled(read))
            finish(read, STATUS_CANCELED);
        else if (result.status != STATUS_OK)
            finish(read, result.status);
        else if (result.bytes != flight.extent.size)
            finish(read, STATUS_PEER_FAULT);
        else {
            ::memcpy(read.output + read.done,
                     backend_.payload() + flight.buffer + flight.extent.skip, flight.extent.bytes);
            read.done += flight.extent.bytes;
            if (read.done == read.size)
                finish(read, STATUS_OK);
        }
    }

  public:
    io_reader(Backend& backend, Mapper map) noexcept : backend_(backend), map_(map) {}

    auto poll() noexcept -> std::expected<bool, status_t> {
        bool progress{};
        for (auto& slot : flights_)
            if (slot.read) {
                io_done result{};
                const auto status = backend_.completion(result, slot.id);
                if (status == STATUS_WOULD_BLOCK)
                    continue;
                if (status != STATUS_OK)
                    return std::unexpected(status);
                complete(slot, result);
                progress = true;
            }
        return progress;
    }
    auto wait() noexcept -> status_t {
        for (auto& slot : flights_)
            if (slot.read) {
                io_done result{};
                const auto status = backend_.wait(result, slot.id);
                if (status == STATUS_OK)
                    complete(slot, result);
                return status;
            }
        return STATUS_WOULD_BLOCK;
    }
    auto submit(io_read& read) noexcept -> std::expected<bool, status_t> {
        if (!read.active || read.submitted)
            return false;
        if (cancelled(read)) {
            finish(read, STATUS_CANCELED);
            return true;
        }
        if (read.done == read.size) {
            finish(read, STATUS_OK);
            return true;
        }
        for (auto& slot : flights_)
            if (!slot.read) {
                const auto io_extent = map_(read);
                if (!io_extent) {
                    finish(read, io_extent.error());
                    return true;
                }
                if (!io_extent->bytes || io_extent->bytes > read.size - read.done ||
                    io_extent->skip > io_extent->size ||
                    io_extent->bytes > io_extent->size - io_extent->skip)
                    return std::unexpected(STATUS_INTERNAL);
                io_cmd request{.operation = static_cast<uint64_t>(io_operation::Read),
                               .offset = io_extent->offset,
                               .length = io_extent->size};
                const auto status = backend_.submit(request);
                if (status == STATUS_BUSY)
                    return false;
                if (status != STATUS_OK) {
                    finish(read, status);
                    return true;
                }
                read.submitted = true;
                slot = {&read, request.id, request.buffer_offset, *io_extent};
                return true;
            }
        return false;
    }
};

// One lifetime per endpoint. Backend owners stop admission on closing(), end
// all backend access, then pass close_ready to flush(). The common transport
// owns page authority, control reply credit and final queue publication.

class io_server final : private libk::noncopyable_nonmovable {
  public:
    [[nodiscard]] auto prepare(cap_t pool, cap_t vspace, cap_t cspace, uintptr_t address,
                               bool writable_payload = false) noexcept -> status_t {
        cspace_ = cspace;
        writable_payload_ = writable_payload;
        return memory_.create(pool, vspace, address);
    }
    [[nodiscard]] auto bind(cap_t channel, cap_t events, uint64_t value = 0) noexcept -> status_t {
        events_ = events;
        value_ = value;
        return control_.open(channel, events);
    }
    [[nodiscard]] auto open(cap_t channel, cap_t events, cap_t pool, cap_t vspace, cap_t cspace,
                            uintptr_t address, uint64_t value = 0) noexcept -> status_t {
        const auto status = prepare(pool, vspace, cspace, address);
        return status == STATUS_OK ? bind(channel, events, value) : status;
    }

    void abort() noexcept {
        closing_ = true;
        if (queue_)
            queue_->stop();
        peer_event_ = {};
        reply_.reset();
    }
    [[nodiscard]] auto done() const noexcept -> bool {
        if (!closing_ || reply_ || (queue_ && queue_->active() != 0))
            return false;
        for (const auto& exported : exports_)
            if (exported)
                return false;
        return true;
    }
    void reset() noexcept {
        if (!done())
            sys::exit(STATUS_BUSY);
        if (const auto rc = control_.close(); rc != STATUS_OK)
            sys::exit(rc);
        queue_.reset();
        peer_event_ = {};
        closing_ = false;
    }

    template <class Handler> [[nodiscard]] auto poll(Handler&& application) noexcept -> status_t {
        if (reply_)
            return STATUS_OK;
        ch_packet packet;
        const auto status = control_.receive(packet);
        if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY)
            return STATUS_OK;
        if (status != STATUS_OK)
            return status;
        return accept(packet, std::forward<Handler>(application));
    }

    template <class Handler>
    [[nodiscard]] auto accept(ch_packet& packet, Handler&& application) noexcept -> status_t {
        if (reply_)
            return STATUS_BUSY;
        const auto& message = packet.message;
        reply_.emplace();
        reply_->message = ch_msg{.operation = message.operation, .id = message.id};
        if (closing_)
            reply_->message.status = STATUS_CLOSED;
        else if (message.operation == static_cast<uint64_t>(io_control::Open)) {
            if (queue_ || packet.count != 1 || message.value != io_queue_depth || message.size != 0)
                reply_->message.status = STATUS_BAD_ARGS;
            else {
                memory_.initialize();
                auto created = memory_.export_pages(cspace_, exports_, writable_payload_);
                if (created != STATUS_OK)
                    return created;
                for (auto& exported : exports_) {
                    const auto copied = sys::cap_duplicate(exported.selector(), cspace_,
                                                           RIGHT_MAP | RIGHT_DUPLICATE);
                    if (copied.status != STATUS_OK)
                        return copied.status;
                    if (!reply_->offer(cap_owner{copied.value}, RIGHT_MAP))
                        return STATUS_INTERNAL;
                }
                const auto signal =
                    sys::cap_duplicate(events_, cspace_, RIGHT_SIGNAL | RIGHT_DUPLICATE);
                if (signal.status != STATUS_OK)
                    return signal.status;
                if (!reply_->offer(cap_owner{signal.value}, RIGHT_SIGNAL))
                    return STATUS_INTERNAL;
                peer_event_ = std::move(packet.capabilities[0]);
                queue_.emplace(std::as_const(memory_).client(), memory_.server());
                reply_->message.value = value_;
            }
        } else if (packet.count != 0 || !queue_)
            reply_->message.status = STATUS_BAD_ARGS;
        else if (message.operation == static_cast<uint64_t>(io_control::Cancel)) {
            io_ticket ticket{};
            reply_->message.status =
                message.size != 0 ? STATUS_BAD_ARGS : queue_->cancel(message.value, ticket);
        } else if (message.operation == static_cast<uint64_t>(io_control::Close)) {
            if (message.size != 0)
                reply_->message.status = STATUS_BAD_ARGS;
            else {
                queue_->stop();
                closing_ = true;
            }
        } else
            application(message, *reply_);
        return STATUS_OK;
    }

    [[nodiscard]] auto queue() noexcept -> io_rx* { return queue_ ? &*queue_ : nullptr; }
    [[nodiscard]] auto payload() noexcept -> uint8_t* { return memory_.payload(); }
    [[nodiscard]] auto closing() const noexcept -> bool { return closing_; }
    [[nodiscard]] auto failed() const noexcept -> bool { return closing_ && !peer_event_; }
    [[nodiscard]] auto arm() noexcept -> status_t { return control_.arm().status; }

    [[nodiscard]] auto flush(bool close_ready) noexcept -> status_t {
        if (queue_) {
            // A visible completion also returns its submission credit, so a
            // client can immediately refill a completed batch.
            (void) queue_->release();
            const bool completed = queue_->publish();
            if (completed && peer_event_) {
                const auto status = sys::notification_signal(peer_event_.selector()).status;
                // The peer may revoke its notification while a borrowed I/O
                // completion is still in flight. Finish the local session.
                if (status == STATUS_BUSY || status == STATUS_CLOSED ||
                    status == STATUS_INVALID_CAP)
                    abort();
                else if (status != STATUS_OK)
                    return status;
            }
        }
        if (closing_ && close_ready && (!queue_ || queue_->active() == 0)) {
            // This is the observable end of peer access, before Close's reply.
            // The slot's own mappings use their root grants and remain resident.
            for (auto& exported : exports_)
                if (exported) {
                    const auto status = sys::cap_revoke(exported.selector(), true).status;
                    if (status != STATUS_OK)
                        return status;
                    exported = {};
                }
        }
        if (!reply_ || (closing_ && (!close_ready || queue_->active() != 0)))
            return STATUS_OK;
        const auto status = control_.send(*reply_);
        if (status == STATUS_OK)
            reply_.reset();
        return status == STATUS_WOULD_BLOCK || status == STATUS_BUSY ? STATUS_OK : status;
    }

  private:
    ch_port control_;
    io_mem memory_;
    std::optional<io_rx> queue_;
    cap_owner peer_event_;
    cap_owner exports_[3];
    std::optional<ch_reply> reply_;
    cap_t events_{}, cspace_{};
    uint64_t value_{};
    bool closing_{};
    bool writable_payload_{};
};
