#pragma once
#include <proc.hpp>
#include <vm.hpp>
#include <ubus/ubus.hpp>
#include <deque>
#include <functional>

enum : word_t { mgr_observe = 1, mgr_control = 2 };
inline constexpr rt_key mgr_children{"bus.children", ObjChannel};
inline constexpr rt_key mgr_jobs{"jobs", ObjChannel};
inline constexpr rt_key mgr_read_bus{"bus.read", ObjChannel};

// Project native facts as public values; no additional source of object state.
auto mgr_method(ubus_object&, std::string name, ubus_handler) -> status_t;
auto mgr_inspect(cap_t scope, cap_t cspace, ubus_value query, ubus_msg& result) -> status_t;
auto mgr_revoke(cap_t cspace, ubus_value query, ubus_msg& result) -> status_t;
auto mgr_row(proc_task* task, uint64_t id, uint64_t gen, std::string_view name,
             std::string_view state, bool control) -> ubus_data;
auto mgr_view(ubus_conn& directory, std::string_view prefix, uint64_t rights,
              cap_t scope, uint64_t quota) -> std::expected<cap_owner, status_t>;
auto mgr_role(ubus_conn& directory, std::string_view alias, std::string_view publication) -> status_t;

struct mgr_child {
    uint64_t id{}, gen{}, publications{64};
    std::string name, image;
    std::vector<std::string> args;
    proc_quota quota;
    std::optional<proc_auth> auth;
    cap_t scope{}; // Optional deployment Scope containing this execution and its resources.
    std::vector<proc_cap> grants;
    struct grant { std::string name; cap_owner cap; word_t rights; uint16_t kind; uint64_t bytes; bool linear{}; };
    std::deque<grant> owned;
    std::unique_ptr<proc_task> task;
    ev_notice ready;
    std::string_view state{"stopped"};
    status_t result{};
    bool desired{true}, await_ready{true}, reusable{true};
};

// Deferred observation uses native Source/Grant versions and owns its Reply.
// Native Port capacity bounds admission; closing the peer drops all watches.
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

class mgr {
    std::deque<std::unique_ptr<mgr_child>> children_;
    ubus_object object_{"myos.manager"};
    ubus_conn directory_;
    mgr_wait waiters_;
    cap_t lookup_{};
    uint64_t next_{}, epoch_{1};
    std::function<std::expected<proc_image, status_t>(std::string_view)> image_;
    auto request(std::string_view, ubus_request&, ubus_msg&) -> status_t;
public:
    explicit mgr(std::function<std::expected<proc_image, status_t>(std::string_view)> image) : image_(std::move(image)) {}
    auto directory(cap_t admin, cap_t lookup) -> status_t { lookup_ = lookup; return directory_.bind(admin); }
    auto add(std::string name, std::string image, proc_quota quota = {}) -> mgr_child&;
    auto start(mgr_child&) -> status_t;
    auto poll() -> status_t;
    auto open() -> status_t;
    auto deadline() const -> uint64_t { return waiters_.deadline(); }
    auto object() -> ubus_object& { return object_; }
    auto children() const -> const auto& { return children_; }
    auto action(mgr_child&, std::string_view) -> status_t;
    auto find(uint64_t id) -> mgr_child*;
    // Caller schedules starts: policy chooses ordering and readiness dependencies.
    // This mechanism owns construction, retirement, and the actual result.
};
