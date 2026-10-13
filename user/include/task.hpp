#pragma once
#include <cap.hpp>
#include <optional>

inline constexpr word_t pr_base_kinds = OBJ_BIT(ObjThread) | OBJ_BIT(ObjSc)
    | OBJ_BIT(ObjCSpace) | OBJ_BIT(ObjMem) | OBJ_BIT(ObjVSpace)
    | OBJ_BIT(ObjGroup) | OBJ_BIT(ObjNotification);

struct pr_quota {
    word_t bytes{1024 * 1024}, caps{256}, kinds{pr_base_kinds};
    word_t slots{128}, pages{20}, pool_perms{RIGHT_CREATE};
};
struct pr_sched {
    word_t budget{500'000}, period{10'000'000}, urgency{8};
    uint32_t cpu{UINT32_MAX};
};
struct pr_exit { status_t status{}; };
struct pr_auth { cap_t pool{}, domain{}, vspace{}; uint32_t cpus{}; };

class pr_task {
    enum class state { New, Running, Closing, Consumed };
    state state_{};
    status_t status_{};
    cap_owner group_, vm_, cs_, thread_, sc_, ended_;
public:
    pr_task() = default;
    pr_task(const pr_task&) = delete;
    auto operator=(const pr_task&) -> pr_task& = delete;

    ~pr_task() { if (group_) sys::exit(STATUS_BUSY); }
    auto pool() const noexcept -> cap_t { return group_.selector(); }
    auto vspace() const noexcept -> cap_t { return vm_.selector(); }
    auto cspace() const noexcept -> cap_t { return cs_.selector(); }
    auto open(cap_t parent, const pr_quota& q) noexcept -> status_t {
        if (state_ != state::New || group_) return STATUS_BUSY;
        const auto g = sys::resource_create_child(parent, q.bytes, q.caps, q.kinds);
        if (g.value) group_ = cap_owner{g.value};
        if (g.status != STATUS_OK || !group_) return g.status == STATUS_OK ? STATUS_INVALID_CAP : g.status;
        const auto v = sys::vspace_create(pool());
        if (v.value) vm_ = cap_owner{v.value};
        if (v.status != STATUS_OK || !vm_) return v.status == STATUS_OK ? STATUS_INVALID_CAP : v.status;
        const auto c = sys::cspace_create(pool(), q.slots, q.pages);
        if (c.value) cs_ = cap_owner{c.value};
        return c.status == STATUS_OK && !cs_ ? STATUS_INVALID_CAP : c.status;
    }
private:
    auto close() noexcept -> status_t {
        if (!group_) return STATUS_OK;
        // Child slots belong to its CSpace, retired by group drain.
        for (auto* cap : {&thread_, &sc_, &ended_, &cs_, &vm_}) {
            const auto status = cap->close();
            if (status != STATUS_OK) return status;
        }
        const auto status = sys::resource_close(pool()).status;
        return status == STATUS_OK ? group_.close() : status;
    }

public:
    auto start(cap_t desc, cap_t domain, uint32_t cpus, const pr_sched& sched,
               cap_t events, uint32_t first_cpu = 0) noexcept -> status_t {
        if (!domain || !cpus || thread_) return STATUS_BAD_ARGS;
        const auto ended = events ? sys::cap_duplicate(events, 0, RIGHT_SIGNAL)
                                  : sys::notification_create(pool(), 1);
        ended_ = cap_owner{ended.value};
        if (ended.status != STATUS_OK) return ended.status;
        const auto thread = sys::thread_create(pool(), vspace(), cspace(), desc, 0);
        thread_ = cap_owner{thread.value};
        if (thread.status != STATUS_OK) return thread.status;
        status_t status = STATUS_BUSY;
        for (uint32_t i = 0; i < (sched.cpu == UINT32_MAX ? cpus : 1); ++i) {
            const auto cpu = sched.cpu == UINT32_MAX ? (first_cpu + i) % cpus : sched.cpu;
            const auto sc = sys::sc_create(pool(), domain, sched.budget, sched.period, sched.urgency, cpu);
            sc_ = cap_owner{sc.value};
            status = sc.status;
            if (status != STATUS_BUSY) break;
        }
        if (status == STATUS_OK) status = sys::sc_bind(sc_.selector(), thread_.selector()).status;
        if (status == STATUS_OK) status = sys::exit_bind(thread_.selector(), ended_.selector(), 1).status;
        if (status == STATUS_OK) status = sys::execution_start(thread_.selector()).status;
        if (status == STATUS_OK) state_ = state::Running;
        return status;
    }
    auto observe() noexcept -> sys::SysResult {
        if (state_ == state::Consumed) return {.status = STATUS_INVALID_CAP};
        if (state_ == state::Closing) return {.status = STATUS_OK, .value = 1, .value2 = static_cast<word_t>(status_)};
        return state_ == state::Running ? sys::exit_query(thread_.selector()) : sys::SysResult{.status = STATUS_WOULD_BLOCK};
    }
    // Initiate stopping before callers wait for any sibling to finish retirement.
    auto stop(status_t reason = STATUS_CANCELED) noexcept -> status_t {
        if (state_ == state::Consumed) return STATUS_INVALID_CAP;
        if (state_ == state::Closing) return STATUS_OK;
        const auto ended = state_ == state::Running ? sys::exit_query(thread_.selector()) : sys::SysResult{};
        if (ended.status != STATUS_OK) return ended.status;
        state_ = state::Closing;
        status_ = ended.value ? static_cast<status_t>(ended.value2) : reason;
        const auto status = close();
        return status == STATUS_BUSY || status == STATUS_RETRY ? STATUS_OK : status;
    }
    auto poll() noexcept -> status_t {
        if (state_ == state::Consumed) return STATUS_INVALID_CAP;
        if (state_ != state::Closing) {
            const auto ended = observe();
            if (ended.status != STATUS_OK) return ended.status;
            if (!ended.value) return STATUS_WOULD_BLOCK;
            const auto status = stop(static_cast<status_t>(ended.value2));
            if (status != STATUS_OK) return status;
        }
        return close();
    }
    auto result() const noexcept -> std::optional<pr_exit> { return state_ == state::Closing && !group_ ? std::optional{pr_exit{status_}} : std::nullopt; }
    auto closing() const noexcept -> bool { return state_ == state::Closing && bool(group_); }
    auto collect() noexcept -> sys::SysResult {
        const auto status = poll();
        if (status != STATUS_OK) return {.status = status};
        state_ = state::Consumed;
        return {.status = STATUS_OK, .value = static_cast<word_t>(status_)};
    }
};
