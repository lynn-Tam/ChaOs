#pragma once
#include <proc.hpp>
#include <ubus/ubus.hpp>
#include <deque>
#include <functional>
#include <map>

inline constexpr word_t mgr_observe = 1, mgr_control = 2;
auto mgr_method(ubus_object&, std::string name, ubus_handler) -> status_t;
auto mgr_inspect(cap_t scope, cap_t cspace, ubus_value, ubus_msg&) -> status_t;
auto mgr_revoke(cap_t cspace, ubus_value, ubus_msg&) -> status_t;
auto mgr_row(proc_task*, uint64_t id, uint64_t gen, std::string_view name, std::string_view state, bool control) -> ubus_data;

class mgr_wait {
    struct entry {
        cap_owner scope;
        uint64_t object{}, selector{}, selected{}, epoch{}, grant{};
        std::array<ev_watch, 3> watches;
        ubus_request request;
        auto facts(ubus_msg&) const -> status_t;
    };
    std::vector<std::unique_ptr<entry>> entries_;
    uint64_t deadline_{};
public:
    auto accept(cap_t scope, cap_t cspace, ubus_request&, ubus_msg&) -> status_t;
    void poll();
    auto deadline() const -> uint64_t { return deadline_; }
};

// One deployment owns its environment, immutable recipe and current execution.
// Scope is always owned, never an optional external lifetime patch.
struct mgr_child {
    uint64_t id{}, gen{}, version{1}, environment{}, quota{32};
    std::string name, image;
    std::vector<std::string> args, env;
    uint32_t cpu{};
    proc_quota resources;
    proc_auth auth{};
    cap_owner scope;
    ev_watch retired;
    ubus_data routes, exports{ubus_type::Array}, recipe, origin;
    ubus_msg setup;
    vm_area source;
    std::function<std::expected<proc_image, status_t>()> loading;
    struct snapshot { uint64_t version{}; bool replayable{}, active{}; ubus_msg recipe; };
    std::map<std::string, snapshot, std::less<>> snapshots;
    std::deque<ubus_msg> inbox;
    bool received{}, desired{}, restart{}, reusable{true};
    std::unique_ptr<proc_task> task;
    std::string_view state{"idle"};
    status_t result{};
};

class mgr {
    struct publication {
        uint64_t id{}, owner{}, gen{}, version{};
        std::string name, interface;
        cap_owner connector;
        ev_watch terminal;
    };
    struct pending {
        enum kind { Get, Receive, Send, Changed, Configure, Spawn, Action } operation{};
        uint64_t source{}, environment{}, version{}, provider{}, provider_version{}, gen{}, epoch{};
        std::string name;
        ubus_request request;
        ubus_msg staged;
        ev_watch terminal;
        std::unique_ptr<ubus_conn> relay;
        bool introduced{};
    };
    struct loan {
        uint64_t source{}, gen{};
        cap_owner account;
        ev_watch drained;
    };
    std::map<uint64_t, loan> loans_;
    uint64_t next_loan_{};
    bool durable_{};
    std::map<std::string, uint64_t, std::less<>> revisions_;
    std::deque<std::unique_ptr<mgr_child>> children_;
    std::map<uint64_t, publication> publications_;
    std::vector<std::unique_ptr<pending>> pending_;
    ubus_object object_{"ubus.environment"};
    mgr_wait observers_;
    uint64_t next_{}, next_environment_{3}, next_publication_{}, identity_{}, epoch_{1}, deadline_{};
    size_t max_pending_{128};
    std::function<std::expected<proc_image, status_t>(mgr_child&)> load_;
    auto environment(word_t) -> mgr_child*;
    auto request(std::string_view, ubus_request&, ubus_msg&) -> status_t;
    auto resolve(mgr_child*, std::string_view, ubus_msg&, pending*) -> status_t;
    auto deliver(mgr_child&, ubus_msg&) -> status_t;
    auto defer(std::unique_ptr<pending>, ubus_request&) -> status_t;
    auto cycle(uint64_t from, uint64_t target) const -> bool;
    void withdraw(mgr_child&);
public:
    explicit mgr(std::function<std::expected<proc_image, status_t>(mgr_child&)> load) : load_(std::move(load)) {}
    auto add(std::string name, std::string image, proc_quota = {}) -> mgr_child&;
    auto configure(ubus_msg recipe, bool spawn = false, bool validate = false) -> std::expected<mgr_child*, status_t>;
    auto recover(std::function<std::expected<ubus_msg, status_t>(ubus_msg)> = {}) -> status_t;
    auto start(mgr_child&) -> status_t;
    auto poll() -> status_t;
    auto open() -> status_t;
    auto deadline() const -> uint64_t { return deadline_; }
    auto object() -> ubus_object& { return object_; }
    auto lookup(std::string_view name) -> std::expected<cap_owner, status_t>;
    auto children() const -> const auto& { return children_; }
    auto action(mgr_child&, std::string_view) -> status_t;
    auto erase(uint64_t id) -> status_t;
    auto find(uint64_t id) -> mgr_child*;
    auto find(std::string_view name) -> mgr_child*;
};
