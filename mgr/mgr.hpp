#pragma once
#include <proc.hpp>
#include <ubus/ubus.hpp>
#include <deque>
#include <functional>
#include <map>

inline constexpr word_t mgr_observe = 1, mgr_control = 2, mgr_create = 4, mgr_debug = 8;
enum class mgr_state { Idle, Loading, Running, Draining, Failed };
inline constexpr auto mgr_state_name(mgr_state state) -> std::string_view {
    constexpr std::string_view names[]{"idle", "loading", "running", "draining", "failed"};
    return names[std::to_underlying(state)];
}
// One deployment owns its environment, immutable recipe and current execution.
// Scope is always owned, never an optional external lifetime patch.
struct mgr_child {
    uint64_t id{}, gen{}, version{1}, environment{}, capacity{32};
    std::string name, image;
    std::vector<std::string> args, env;
    uint32_t cpu{};
    proc_quota resources;
    proc_auth auth{};
    cap_owner scope;
    ev_watch retired;
    ubus_data routes, exports{ubus_type::Array}, recipe, origin;
    ubus_msg setup;
    cap_owner observe, manage, root_scope;
    uint64_t root_version{};
    vm_area source;
    std::function<std::expected<proc_image, status_t>()> loading;
    struct snapshot { uint64_t version{}; bool replayable{}, active{}; ubus_msg recipe; };
    std::map<std::string, snapshot, std::less<>> snapshots;
    std::deque<ubus_msg> inbox;
    bool received{}, desired{}, restart{}, relaunch{}, removing{}, reusable{true};
    std::unique_ptr<proc_task> task;
    mgr_state state{mgr_state::Idle};
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
        uint64_t source{}, environment{}, version{}, provider{}, provider_version{}, gen{}, epoch{}, authority{};
        std::string name;
        ubus_request request;
        ubus_msg staged;
        ev_watch terminal;
        std::array<ev_watch, 2> changes;
        std::unique_ptr<ubus_conn> relay;
        bool introduced{};
    };
    struct loan {
        uint64_t source{}, gen{}, authority{};
        cap_owner quota;
        ev_watch drained{};
    };
    std::map<uint64_t, loan> loans_;
    uint64_t next_loan_{};
    bool durable_{};
    std::map<std::string, uint64_t, std::less<>> revisions_;
    std::deque<std::shared_ptr<mgr_child>> children_;
    std::map<uint64_t, publication> publications_;
    std::vector<std::unique_ptr<pending>> pending_;
    struct facet {
        uint64_t parent{}, target{}, run{};
        word_t rights{};
        bool closed{};
        uint64_t grant{};
        ev_watch drained{};
    };
    std::map<word_t, facet> facets_;
    ubus_object object_{"ubus.environment", 2, 0,
        [this](word_t facet, std::string_view method) { return access(facet, method); }};
    uint64_t next_{}, next_badge_{3}, next_publication_{}, identity_{}, cspace_id_{}, epoch_{1}, deadline_{};
    size_t capacity_{128};
    std::function<std::expected<proc_image, status_t>(mgr_child&)> load_;
    auto environment(word_t) -> mgr_child*;
    auto live(word_t) -> bool;
    auto descends(word_t, word_t) const -> bool;
    auto access(word_t, std::string_view) -> status_t;
    auto issue(word_t parent, word_t rights, uint64_t target = 0, uint64_t run = 0, bool independent = false) -> std::expected<cap_owner, status_t>;
    auto admit(const ubus_request&) -> status_t;
    auto request(std::string_view, ubus_request&, ubus_msg&) -> status_t;
    auto resolve(mgr_child*, std::string_view, ubus_msg&, pending*) -> status_t;
    auto deliver(mgr_child&, ubus_msg&) -> status_t;
    auto defer(std::unique_ptr<pending>, ubus_request&) -> status_t;
    auto cycle(uint64_t from, uint64_t target) const -> bool;
    void withdraw(mgr_child&);
public:
    explicit mgr(std::function<std::expected<proc_image, status_t>(mgr_child&)> load) : load_(std::move(load)) {}
    auto add(std::string name, std::string image, proc_quota = {}) -> mgr_child&;
    auto configure(ubus_msg recipe, bool spawn = false, bool validate = false) -> std::expected<std::shared_ptr<mgr_child>, status_t>;
    auto recover(std::function<std::expected<ubus_msg, status_t>(ubus_msg)> = {}) -> status_t;
    auto start(mgr_child&) -> status_t;
    auto poll() -> status_t;
    auto open() -> status_t;
    auto attach(ubus_conn&) -> status_t;
    auto connector(word_t rights = mgr_observe | mgr_control) -> std::expected<cap_owner, status_t> { return issue(3, rights); }
    auto deadline() const -> uint64_t { return deadline_; }
    auto object() -> ubus_object& { return object_; }
    auto lookup(std::string_view name) -> std::expected<cap_owner, status_t>;
    auto known(std::string_view name) const -> bool { return revisions_.contains(name); }
    auto children() const -> const auto& { return children_; }
    auto action(mgr_child&, std::string_view) -> status_t;
    auto erase(uint64_t id) -> status_t;
    auto find(uint64_t id) -> std::shared_ptr<mgr_child>;
    auto find(std::string_view name) -> std::shared_ptr<mgr_child>;
};
