#pragma once
#include "value.hpp"
#include <rpc.hpp>
#include <expected>
#include <functional>
#include <memory>
#include <string>

// Maximum control body for version 2; bulk data belongs in BufferSlice.
inline constexpr size_t ubus_body_limit = 1u << 20;

// Transport capabilities are separate from value indices. A message owns all
// received/explicitly offered slots; attach borrows a sender's slot until send.
struct ubus_msg {
    ubus_data data;
    std::array<cap_owner, CHANNEL_MAX_CAPS> caps;
    std::array<CapXfer, CHANNEL_MAX_CAPS> transfers{};
    size_t count{};
    status_t status{};
    word_t tag{}, badge{}, deadline{};
    auto attach(cap_t cap, word_t rights) -> std::expected<size_t, status_t>;
    auto offer(cap_owner&& cap, word_t rights) -> std::expected<size_t, status_t>;
    auto give(cap_owner&& cap) -> std::expected<size_t, status_t>;
    void commit();
    void forward(); // Move owned received capabilities on the next successful send.
    auto clone() const -> std::expected<ubus_msg, status_t>;
    // Select a value and its referenced capabilities, borrowing source slots.
    auto slice(ubus_value) const -> std::expected<ubus_msg, status_t>;
    auto capability(ubus_value value) const -> cap_t;
    auto take(ubus_value value) -> cap_owner;
};

struct ubus_field {
    std::string name;
    ubus_type type{};
    bool optional{};
    uint64_t max_bytes{UINT64_MAX};
    // Named Record fields, or one unnamed value schema for Array/Record maps.
    // Empty means any contained value. Vector can constrain its numeric atom.
    std::vector<ubus_field> fields{};
    std::optional<ubus_atom> atom{};
};
using ubus_schema = std::vector<ubus_field>;
struct ubus_method;

struct ubus_request : ubus_msg {
    cap_owner reply;
    auto facet() const -> word_t { return badge ? badge - 1 : 0; }
    std::shared_ptr<const ubus_method> method;
    auto respond(ubus_msg response = {}, bool* committed = nullptr) -> status_t;
};
using ubus_handler = std::function<status_t(ubus_request&, ubus_msg&)>;
struct ubus_method {
    std::string name;
    ubus_schema input, output;
    ubus_handler handler;
    word_t required{};
    bool notify{};
};

// Wire storage outlives a blocked native send. Its body capability is simply
// another common carrier, never a second serialization format. RPC bodies
// may be reused for the same endpoint after its reply; decoding copies them.
struct ubus_wire {
    std::array<word_t, CHANNEL_MAX_WORDS> words{};
    std::array<CapXfer, CHANNEL_MAX_CAPS> caps{};
    cap_owner body;
    size_t size{}, count{}, capacity{};
    auto encode(const ubus_msg&, uint16_t kind, size_t maxcaps) -> status_t;
};

auto ubus_send(cap_t endpoint, ubus_msg&, bool block = true) -> status_t;
auto ubus_notify(cap_t endpoint, ubus_msg&, bool block = true) -> status_t;
auto ubus_receive(cap_t endpoint, ubus_request&, bool block = false) -> status_t;
auto ubus_receive_event(cap_t endpoint, ubus_msg&, bool block = false) -> status_t;

class ubus_rx {
    ch_rx input_;
public:
    explicit operator bool() const { return bool(input_); }
    auto open(cap_t channel, cap_t events = 0) -> status_t { return input_.open(channel, events); }
    auto close() -> status_t { return input_.close(); }
    auto arm() -> status_t { return input_.arm(); }
    auto receive(ubus_request&) -> status_t;
    auto receive(ubus_msg&) -> status_t;
};

class ubus_call {
    rpc_op rpc_;
    ubus_wire wire_;
    ubus_msg message_;
    cap_t peer_{};
public:
    auto active() const noexcept -> bool { return rpc_.active(); }
    auto sent() const noexcept -> bool { return rpc_.sent(); }
    auto deadline() const noexcept -> word_t { return rpc_.deadline(); }
    auto begin(cap_t endpoint, ubus_msg message, bool drain = false) -> status_t;
    auto poll(ubus_msg&) -> status_t;
    auto wait(ubus_msg&) -> status_t;
    auto cancel() -> status_t;
};

class ubus_conn;

// The complete method table is immutable once a connector is issued. Replacing
// a table means replacing the object; old capabilities retain their old object.
class ubus_object {
    ch_pair channel_;
    cap_owner budget_, scope_;
    ev_watch closing_;
    ubus_rx input_;
    std::vector<std::shared_ptr<const ubus_method>> methods_;
    std::string interface_;
    uint64_t version_, features_;
    std::function<status_t(word_t, std::string_view)> access_;
    bool sealed_{};
    auto describe(word_t facet) const -> ubus_data;
    auto bind(ubus_request&) -> status_t;
public:
    // The same side-effect-free policy governs Describe and invocation. Empty
    // method names check the lifetime of a bound authority, including Bind.
    explicit ubus_object(std::string interface = {}, uint64_t version = 1, uint64_t features = 0,
                         std::function<status_t(word_t, std::string_view)> access = {})
        : interface_(std::move(interface)), version_(version), features_(features), access_(std::move(access)) {}
    auto add(std::string name, ubus_schema input, ubus_schema output, ubus_handler handler,
             word_t required = 0, bool notify = false) -> status_t;
    ~ubus_object();
    auto close() -> status_t;
    auto open(size_t depth = 16, cap_t quota = 0, cap_owner scope = {}) -> status_t;
    auto scope() const -> cap_t { return scope_.selector(); }
    auto connector(word_t facet = 0) -> std::expected<cap_owner, status_t>;
    // A returned object releases mint authority: its last remote handle closes
    // the sending side, so the owner can drain and reclaim the object.
    auto handle(word_t facet = 0, word_t rights = RIGHT_SEND | RIGHT_DUPLICATE) -> std::expected<cap_owner, status_t>;
    auto poll() -> status_t;
    auto arm() -> status_t { return input_.arm(); }
    auto endpoint() const -> cap_t { return channel_.ends[0].selector(); }
    auto issuer() const -> cap_t { return channel_.ends[1].selector(); }
};

// Bind negotiates a description and retains the same grant as its connector.
// No second endpoint identity outlives or bypasses that grant's revocation.
class ubus_conn {
    cap_owner endpoint_;
    ev_watch peer_;
    ubus_data description_;
    ubus_call call_;
    struct binding { std::string interface; uint64_t minimum, maximum, features; cap_owner endpoint; };
    std::optional<binding> binding_;
    auto bound(ubus_msg&) -> status_t;
    auto install(cap_owner, ubus_value, std::string_view, uint64_t, uint64_t, uint64_t) -> status_t;
public:
    auto active() const -> bool { return call_.active(); }
    auto state() const -> status_t;
    auto close() -> status_t;
    auto release() -> std::expected<cap_owner, status_t>;
    auto adopt(cap_owner endpoint, std::string_view interface = {}, uint64_t minimum = 0,
               uint64_t maximum = UINT64_MAX, uint64_t features = 0) -> status_t;
    auto begin_bind(cap_t connector, std::string_view interface = {}, uint64_t minimum = 0,
                    uint64_t maximum = UINT64_MAX, uint64_t features = 0, uint64_t deadline = 0) -> status_t;
    auto poll_bind() -> status_t;
    auto bind(cap_t connector, std::string_view interface = {}, uint64_t minimum = 0,
              uint64_t maximum = UINT64_MAX, uint64_t features = 0) -> status_t;
    auto description() const -> ubus_value { return description_.view(); }
    auto endpoint() const -> cap_t { return endpoint_.selector(); }
    auto token(std::string_view name) const -> std::optional<word_t>;
    auto invoke(std::string_view name, ubus_msg request, ubus_msg& response) -> status_t;
    auto begin(std::string_view name, ubus_msg request) -> status_t;
    auto notify(std::string_view name, ubus_msg request, bool block = true) -> status_t;
    auto poll(ubus_msg& response) -> status_t { return call_.poll(response); }
    auto cancel() -> status_t { return call_.cancel(); }
};


// Bounded deferred calls with application-owned payloads. The queue knows no
// operation names or data layout; completion is the ordinary public Reply.
struct ubus_ticket { size_t slot{}; uint64_t id{}; };
enum class ubus_admission { Ready, Empty, Closed };
template<class T, size_t Capacity> class ubus_queue {
    struct entry { uint64_t id{}; T data{}; ubus_request call; bool admitted{}, committed{}, cancelled{}; };
    std::array<entry, Capacity> entries_;
    uint64_t last_{};
    size_t active_{};
    bool stopping_{};
    auto valid(ubus_ticket ticket) const -> bool {
        return ticket.slot < Capacity && ticket.id && entries_[ticket.slot].id == ticket.id;
    }
public:
    auto accept(uint64_t id, T data, ubus_request& call) -> status_t {
        if (stopping_) return STATUS_CLOSED;
        if (id <= last_) return STATUS_STALE;
        for (auto& entry : entries_) if (!entry.id) {
            entry.id = id; entry.data = std::move(data); entry.call = std::move(call);
            last_ = id; ++active_; return STATUS_PENDING;
        }
        return STATUS_BUSY;
    }
    auto admit(ubus_ticket& ticket) -> ubus_admission {
        if (stopping_) return ubus_admission::Closed;
        entry* first{};
        for (auto& entry : entries_) if (entry.id && !entry.admitted && (!first || entry.id < first->id)) first = &entry;
        if (!first) return ubus_admission::Empty;
        first->admitted = true; ticket = {size_t(first - entries_.data()), first->id}; return ubus_admission::Ready;
    }
    auto request(ubus_ticket ticket) const -> const T* { return valid(ticket) ? &entries_[ticket.slot].data : nullptr; }
    auto call(ubus_ticket ticket) -> ubus_request* { return valid(ticket) ? &entries_[ticket.slot].call : nullptr; }
    auto cancel(uint64_t id) -> status_t {
        for (auto& entry : entries_) if (id && entry.id == id) {
            if (entry.committed) return STATUS_BUSY;
            entry.cancelled = true; return STATUS_OK;
        }
        return STATUS_NOT_FOUND;
    }
    auto commit(ubus_ticket ticket) -> bool {
        if (!valid(ticket) || entries_[ticket.slot].cancelled) return false;
        const auto peer = sys::source_info(entries_[ticket.slot].call.reply.selector(), SOURCE_WRITE);
        if (peer.status != STATUS_OK || (peer.value & SOURCE_TERMINAL)) return false;
        entries_[ticket.slot].committed = true; return true;
    }
    auto cancelled(ubus_ticket ticket) const -> bool { return valid(ticket) && entries_[ticket.slot].cancelled; }
    auto finish(ubus_ticket ticket, ubus_msg result) -> bool {
        if (!valid(ticket)) return false;
        (void)entries_[ticket.slot].call.respond(std::move(result));
        entries_[ticket.slot] = {}; --active_; return true;
    }
    auto abandon(ubus_ticket ticket) -> bool {
        if (!stopping_ || !valid(ticket)) return false;
        entries_[ticket.slot] = {}; --active_; return true;
    }
    void stop() {
        stopping_ = true;
        for (size_t i = 0; i < Capacity; ++i) {
            auto& entry = entries_[i];
            if (!entry.id) continue;
            if (!entry.admitted) { ubus_msg result; result.status = STATUS_CANCELED; (void)finish({i, entry.id}, std::move(result)); }
            else if (!entry.committed) entry.cancelled = true;
        }
    }
    auto active() const -> size_t { return active_; }
    auto sequence() const -> uint64_t { return last_; }
};

// A bounded set of outgoing calls. Applications supply only their local
// metadata and message construction; transport, ordering and deadlines stay here.
template<class T, size_t Capacity> class ubus_batch {
    struct entry { uint64_t id{}; T data{}; ubus_call call; status_t failure{}; };
    std::array<std::unique_ptr<entry>, Capacity> entries_;
public:
    template<class Build> auto submit(uint64_t id, T data, ubus_conn& peer,
        std::string_view method, Build&& build, bool drain = true) -> status_t {
        if (!id || contains(id)) return STATUS_BAD_ARGS;
        const auto token = peer.token(method); if (!token) return STATUS_INVALID_OP;
        for (const auto& item : entries_)
            if (item && item->id && !item->call.sent()) return STATUS_BUSY;
        for (size_t i = 0; i < Capacity; ++i) if (!entries_[i] || !entries_[i]->id) {
            auto& slot = entries_[i]; if (!slot) slot = std::make_unique<entry>();
            ubus_msg message;
            auto status = build(i, data, message); if (status != STATUS_OK) return status;
            message.tag = *token;
            status = slot->call.begin(peer.endpoint(), std::move(message), drain);
            if (status != STATUS_OK) return status;
            slot->id = id; slot->failure = 0; slot->data = std::move(data); return STATUS_OK;
        }
        return STATUS_BUSY;
    }
    auto completion(T& data, ubus_msg& response, uint64_t id = 0) -> status_t {
        for (auto& item : entries_) if (item && item->id && (!id || item->id == id)) {
            const auto status = item->failure ? item->failure : item->call.poll(response);
            if (status == STATUS_WOULD_BLOCK) continue;
            if (status != STATUS_OK) response.status = status;
            data = std::move(item->data); item->id = 0; return STATUS_OK;
        }
        return STATUS_WOULD_BLOCK;
    }
    auto next(uint64_t after = 0) const -> const T* {
        const entry* first{};
        for (const auto& item : entries_) if (item && item->id > after && (!first || item->id < first->id)) first = item.get();
        return first ? &first->data : nullptr;
    }
    auto contains(uint64_t id) const -> bool {
        return std::ranges::any_of(entries_, [&](const auto& item) { return item && item->id == id && id; });
    }
    auto deadline() const -> uint64_t {
        uint64_t due{};
        for (const auto& item : entries_) if (item && item->id) {
            const auto deadline = item->call.deadline();
            if (deadline != UINT64_MAX && deadline && (!due || deadline < due)) due = deadline;
        }
        return due;
    }
    void abort(status_t status = STATUS_CANCELED) {
        for (auto& item : entries_) if (item && item->id) { (void)item->call.cancel(); item->failure = status; }
    }
};
