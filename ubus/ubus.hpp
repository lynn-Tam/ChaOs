#pragma once
#include "value.hpp"
#include <rpc.hpp>
#include <expected>
#include <functional>
#include <memory>
#include <string>

// Maximum control body for version 1; bulk data belongs in BufferSlice.
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
    auto attach(cap_t cap, word_t rights) -> std::optional<size_t>;
    auto offer(cap_owner cap, word_t rights) -> std::optional<size_t>;
    auto give(cap_owner cap) -> std::optional<size_t>;
    void commit();
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
    auto facet() const -> word_t { return badge >= 2 && !(badge & 1) ? (badge - 2) / 2 : 0; }
    std::shared_ptr<const ubus_method> method;
    auto respond(ubus_msg response = {}) -> status_t;
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
// another common carrier, never a second serialization format.
struct ubus_wire {
    std::array<word_t, CHANNEL_MAX_WORDS> words{};
    std::array<CapXfer, CHANNEL_MAX_CAPS> caps{};
    cap_owner body;
    size_t size{}, count{};
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
    ubus_rx input_;
    std::vector<std::shared_ptr<const ubus_method>> methods_;
    std::string interface_;
    uint64_t version_, features_;
    bool sealed_{};
    auto describe(word_t facet) const -> ubus_data;
    auto bind(ubus_request&) -> status_t;
public:
    explicit ubus_object(std::string interface = {}, uint64_t version = 1, uint64_t features = 0)
        : interface_(std::move(interface)), version_(version), features_(features) {}
    auto add(std::string name, ubus_schema input, ubus_schema output, ubus_handler handler,
             word_t required = 0, bool notify = false) -> status_t;
    auto open(size_t depth = 16) -> status_t;
    auto connector(word_t facet = 0) -> std::expected<cap_owner, status_t>;
    // A returned object releases mint authority: its last remote handle closes
    // the sending side, so the owner can drain and reclaim the object.
    auto handle(word_t facet = 0, word_t rights = RIGHT_SEND | RIGHT_DUPLICATE) -> std::expected<cap_owner, status_t>;
    auto publish(ubus_conn& directory, std::string_view name, word_t facet = 0) -> std::expected<uint64_t, status_t>;
    auto poll() -> status_t;
    auto arm() -> status_t { return input_.arm(); }
    auto endpoint() const -> cap_t { return channel_.ends[0].selector(); }
};

// Connections cache the authorized description returned by Bind. Every call,
// including SDK convenience functions, uses the same values and native engine.
class ubus_conn {
    cap_owner endpoint_;
    ev_watch peer_;
    ubus_data description_;
    ubus_call call_;
    auto install(cap_owner, ubus_value, std::string_view, uint64_t, uint64_t, uint64_t) -> status_t;
public:
    auto active() const -> bool { return call_.active(); }
    auto state() const -> status_t;
    auto close() -> status_t;
    auto release() -> std::expected<cap_owner, status_t>;
    auto adopt(cap_owner endpoint, std::string_view interface = {}, uint64_t minimum = 0,
               uint64_t maximum = UINT64_MAX, uint64_t features = 0) -> status_t;
    auto connect(ubus_conn& directory, std::string_view name, std::string_view interface = {},
                 uint64_t minimum = 0, uint64_t maximum = UINT64_MAX) -> status_t;
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

// Bootstrap injects capabilities and scalar resources, never service contracts.
inline constexpr rt_key ubus_boot{"bus.boot", ObjChannel};
inline constexpr rt_key ubus_owner{"bus.owner", ObjScope};
inline constexpr rt_key ubus_directory{"bus", ObjChannel};
enum ubus_right : uint64_t {
    ubus_list = 1, ubus_resolve = 2, ubus_publish = 4, ubus_manage = 8,
    ubus_view = 16, ubus_wait = 32, ubus_all = 63
};
inline constexpr rt_key ubus_publisher{"bus.publish", ObjChannel};
inline constexpr rt_key ubus_ready{"ready", ObjNotification};
