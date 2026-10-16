#pragma once
#include <cap.hpp>
#include <optional>
#include <start.hpp>
#include <string.h>

inline constexpr auto pr_retryable(status_t s) noexcept -> bool {
    return s == STATUS_BUSY || s == STATUS_RETRY;
}

inline constexpr word_t pr_base_kinds = OBJ_BIT(ObjThread) | OBJ_BIT(ObjSc)
    | OBJ_BIT(ObjCSpace) | OBJ_BIT(ObjMem) | OBJ_BIT(ObjVSpace)
    | OBJ_BIT(ObjGroup) | OBJ_BIT(ObjNotification);

struct pr_quota {
    word_t bytes{1024 * 1024}, caps{256}, kinds{pr_base_kinds};
    word_t slots{128}, pages{20}, pool_perms{RIGHT_CREATE};
};
struct pr_exit { status_t status{}; };
struct pr_auth { cap_t pool{}, vspace{}; uint32_t cpus{}; };

class pr_task {
    enum class state { New, Running, Closing, Consumed };
    state state_{};
    status_t status_{};
    bool closing_{};
    word_t signals_{};
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
        const auto ended = sys::notification_create(parent, 1);
        if (ended.status != STATUS_OK) return ended.status;
        ended_ = cap_owner{ended.value};
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
        if (!closing_) {
            for (auto* cap : {&thread_, &sc_, &cs_, &vm_}) {
                const auto status = cap->close();
                if (status != STATUS_OK) return status;
            }
            const auto status = sys::resource_close_async(pool(), ended_.selector(), 2).status;
            if (status != STATUS_OK) return status;
            closing_ = true;
        }
        const auto ready = sys::notification_take(ended_.selector());
        if (ready.status == STATUS_OK) signals_ |= ready.value;
        else if (ready.status != STATUS_RETRY) return ready.status;
        if (!(signals_ & 2)) return STATUS_WOULD_BLOCK;
        auto status = group_.close();
        if (status == STATUS_OK) status = ended_.close();
        return status;
    }

public:
    auto thread() const noexcept -> cap_t { return thread_.selector(); }
    auto prepare(cap_t desc, cap_t events, uint32_t cpu) noexcept -> status_t {
        if (thread_) return STATUS_BUSY;
        const auto thread = sys::thread_create(pool(), vspace(), cspace(), desc, 0, cpu);
        thread_ = cap_owner{thread.value};
        if (thread.status != STATUS_OK) return thread.status;
        auto status = sys::exit_bind(thread_.selector(), ended_.selector(), 1).status;
        if (status == STATUS_OK) status = sys::execution_watch(thread_.selector(), events ? events : ended_.selector(), 1).status;
        return status;
    }
    auto start(cap_owner sc) noexcept -> status_t {
        if (!thread_ || sc_ || !sc) return STATUS_BAD_ARGS;
        sc_ = std::move(sc);
        auto status = sys::sc_bind(sc_.selector(), thread_.selector()).status;
        if (status == STATUS_OK) status = sys::execution_set(thread_.selector(), true).status;
        if (status == STATUS_OK) state_ = state::Running;
        return status;
    }
    auto info() const noexcept -> std::expected<ThreadInfo, status_t> {
        const auto result = sys::thread_info(thread_.selector());
        if (result.status != STATUS_OK) return std::unexpected(result.status);
        ThreadInfo info;
        ::memcpy(&info, reinterpret_cast<const void*>(rt_startup().ipc), sizeof(info));
        return info;
    }
    auto memory_info() const noexcept -> std::expected<VSpaceInfo, status_t> {
        const auto result = sys::vspace_info(vspace());
        if (result.status != STATUS_OK) return std::unexpected(result.status);
        VSpaceInfo info;
        ::memcpy(&info, reinterpret_cast<const void*>(rt_startup().ipc), sizeof(info));
        return info;
    }
    auto resource_info() const noexcept -> std::expected<GroupInfo, status_t> {
        const auto result = sys::group_info(pool());
        if (result.status != STATUS_OK) return std::unexpected(result.status);
        GroupInfo info;
        ::memcpy(&info, reinterpret_cast<const void*>(rt_startup().ipc), sizeof(info));
        return info;
    }
    auto capabilities(cap_t cursor) const noexcept -> std::expected<CSpaceInfo, status_t> {
        const auto result = sys::cspace_info(cspace(), cursor);
        if (result.status != STATUS_OK) return std::unexpected(result.status);
        CSpaceInfo info;
        ::memcpy(&info, reinterpret_cast<const void*>(rt_startup().ipc), sizeof(info));
        return info;
    }
    auto execution(bool enabled) noexcept -> sys::SysResult {
        return sys::execution_set(thread_.selector(), enabled);
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
        if (thread_) {
            const auto status = sys::execution_stop(thread_.selector()).status;
            if (status != STATUS_OK) return status;
        }
        state_ = state::Closing;
        status_ = ended.value ? static_cast<status_t>(ended.value2) : reason;
        return STATUS_OK;
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
        if (thread_ && !closing_) {
            const auto ended = sys::exit_query(thread_.selector());
            if (ended.status != STATUS_OK) return ended.status;
            if (!ended.value) return STATUS_WOULD_BLOCK;
            status_ = static_cast<status_t>(ended.value2);
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
