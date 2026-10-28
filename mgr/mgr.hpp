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
// Native and public deployments use the same complete recipe at admission.
auto mgr_recipe(std::string_view name, std::string_view program, _proc_limits = {}) -> ubus_msg;

// One deployment owns its environment, immutable recipe and current execution.
// Scope is always owned, never an optional external lifetime patch.
struct mgr_child {
private:
    friend class mgr;
    ubus_msg recipe_;
    auto delivery() -> ubus_msg* {
        if (received) return inbox.empty() ? nullptr : &inbox.front();
        return startup.count || !startup.data.view().data().empty() ? &startup : &recipe_;
    }
public:
    mgr_child(uint64_t id, std::string key) : child_id(id), name(std::move(key)) {}
    auto recipe() const -> const ubus_msg& { return recipe_; }
    auto program() const -> std::string_view { return recipe_.data["program"].as<std::string_view>().value(); }
    auto restart() const -> bool { return recipe_.data["restart"].as<bool>().value_or(false); }
    auto capacity() const -> uint64_t { return recipe_.data["capacity"].as<uint64_t>().value_or(32); }
    // child_id is local to this manager; run_id is the execution Scope object ID.
    const uint64_t child_id;
    const std::string name;
    uint64_t run_id{}, version{1}, environment{};
    _proc_auth auth{};
    _cap_owner scope;
    _ev_watch retired;
    // The hardware owner may supply fresh per-run grants before construction.
    // They are execution obligations, never a second deployment recipe.
    ubus_msg startup;
    _cap_owner observe, manage, root_scope;
    uint64_t root_version{}, started{};
    _vm_area source;
    std::function<std::expected<_proc_image, status_t>()> loading;
    struct snapshot { uint64_t version{}; bool replayable{}, active{}; ubus_msg recipe; };
    std::map<std::string, snapshot, std::less<>> snapshots;
    std::deque<ubus_msg> inbox;
    bool received{}, desired{}, relaunch{}, removing{}, reusable{true};
    struct cpu_sample {
        uint64_t at{}; status_t status{STATUS_WOULD_BLOCK};
        VSpaceRuntime value{};
        void sample(const _proc&, word_t);
    } cpu;
    std::unique_ptr<_proc> task;
    mgr_state state{mgr_state::Idle};
    bool auto_retire{true};
    status_t result{};
};

class mgr {
    struct publication {
        uint64_t id{}, owner{}, run_id{}, version{};
        std::string name, interface;
        _cap_owner connector;
        _ev_watch terminal;
    };
    struct pending {
        enum kind { Get, Receive, Send, Changed, Configure, Spawn, Action } operation{};
        uint64_t source{}, environment{}, version{}, provider{}, provider_version{}, run_id{}, epoch{}, authority{};
        std::string name;
        ubus_request request;
        ubus_msg staged, response;
        word_t control{};
        _ev_watch terminal;
        std::array<_ev_watch, 2> changes;
        std::unique_ptr<ubus_conn> relay;
        bool introduced{};
    };
    struct loan {
        uint64_t source{}, run_id{}, authority{};
        _cap_owner quota;
        _ev_watch drained{};
    };
    std::map<uint64_t, loan> loans_;
    uint64_t next_loan_{};
    bool recoverable_{};
    std::map<std::string, uint64_t, std::less<>> revisions_;
    std::deque<std::shared_ptr<mgr_child>> children_;
    std::map<uint64_t, publication> publications_;
    std::vector<std::unique_ptr<pending>> pending_;
    struct facet {
        uint64_t parent{}, target{}, run_id{};
        word_t rights{};
        bool closed{}, staged{};
        uint64_t grant{};
        _ev_watch drained{};
    };
    std::map<word_t, facet> facets_;
    ubus_object object_{"ubus.environment", 4, 0,
        [this](word_t facet, std::string_view method) { return access(facet, method); }};
    uint64_t next_{}, next_badge_{3}, next_publication_{}, identity_{}, cspace_id_{}, epoch_{1}, deadline_{};
    size_t capacity_{128};
    std::function<std::expected<_proc_image, status_t>(mgr_child&)> load_;
    auto environment(word_t) -> mgr_child*;
    auto live(word_t) -> bool;
    auto descends(word_t, word_t) const -> bool;
    auto access(word_t, std::string_view) -> status_t;
    auto issue(word_t parent, word_t rights, uint64_t target = 0, uint64_t run_id = 0, bool independent = false, word_t* issued = nullptr) -> std::expected<_cap_owner, status_t>;
    auto reserve_reply(ubus_request&, ubus_msg&, word_t) -> std::expected<word_t, status_t>;
    void reply(mgr_child&, ubus_msg&, word_t);
    auto admit(const ubus_request&) -> status_t;
    auto request(std::string_view, ubus_request&, ubus_msg&) -> status_t;
    auto resolve(mgr_child*, std::string_view, ubus_msg&, pending*) -> status_t;
    auto deliver(mgr_child&, ubus_msg&) -> status_t;
    auto defer(std::unique_ptr<pending>, ubus_request&) -> status_t;
    auto cycle(uint64_t from, uint64_t target) const -> bool;
    void withdraw(mgr_child&);
public:
    explicit mgr(std::function<std::expected<_proc_image, status_t>(mgr_child&)> load) : load_(std::move(load)) {}
    auto configure(ubus_msg recipe, bool spawn = false, bool validate = false) -> std::expected<std::shared_ptr<mgr_child>, status_t>;
    auto recover(std::function<std::expected<ubus_msg, status_t>(ubus_msg)> = {}) -> status_t;
    auto start(mgr_child&) -> status_t;
    auto poll() -> status_t;
    auto open() -> status_t;
    auto attach(ubus_conn&) -> status_t;
    auto connector(word_t rights = mgr_observe | mgr_control) -> std::expected<_cap_owner, status_t> { return issue(3, rights); }
    auto deadline() const -> uint64_t { return deadline_; }
    auto object() -> ubus_object& { return object_; }
    auto lookup(std::string_view name) -> std::expected<_cap_owner, status_t>;
    auto known(std::string_view name) const -> bool { return revisions_.contains(name); }
    auto children() const -> const auto& { return children_; }
    auto action(mgr_child&, std::string_view) -> status_t;
    auto erase(uint64_t id) -> status_t;
    auto find(uint64_t id) -> std::shared_ptr<mgr_child>;
    auto find(std::string_view name) -> std::shared_ptr<mgr_child>;
};
