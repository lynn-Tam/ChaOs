#pragma once
#include <vm.hpp>
#include <ipc.hpp>
#include <span>
#include <string_view>

inline constexpr word_t _proc_base_kinds = OBJ_BIT(ObjThread) | OBJ_BIT(ObjSc) | OBJ_BIT(ObjDomain)
    | OBJ_BIT(ObjCSpace) | OBJ_BIT(ObjMem) | OBJ_BIT(ObjVSpace)
    | OBJ_BIT(ObjChannel) | OBJ_BIT(ObjIoOp) | OBJ_BIT(ObjVmOp) | OBJ_BIT(ObjQuota) | OBJ_BIT(ObjScope) | OBJ_BIT(ObjNotification) | OBJ_BIT(ObjPort) | OBJ_BIT(ObjPager);

struct _proc_limits {
    word_t bytes{1024 * 1024}, caps{256}, kinds{_proc_base_kinds};
    word_t slots{128}, pages{20}, quota_perms{RIGHT_CREATE | RIGHT_INSPECT};
    word_t watches{64}, hints{16};
    word_t max_bytes{}, max_caps{}; // Zero keeps that dimension's initial ceiling.
};
struct _proc_auth { CreateAuth alloc{}; cap_t domain{}; };

struct _proc_result {
    int code{};
    uint64_t thread{};
    ThreadExit exit{.origin=EXIT_NORMAL, .kind=FAULT_NONE, .status=STATUS_OK, .reserved=0, .pc=0, .address=0};
};

// The caller keeps this source mapping and its capability alive through create.
// perms records the offered view; the kernel still checks every derived map.
struct _proc_image {
    std::span<const std::byte> bytes;
    cap_t memory{};
    uint64_t offset{};
    word_t perms{VM_READ};
    static auto map(_vm_area&, _cap_owner, uint64_t offset, uint64_t size, word_t perms = VM_READ)
        -> std::expected<_proc_image, status_t>;
};

struct _proc_layout {
    size_t stack_size{64 * 1024}, guard_size{4096};
    word_t stack{}, ipc{};
};
struct _proc_opts {
    std::span<const std::string_view> argv{}, envp{};
    cap_t bootstrap{}, pager{};
    _proc_limits resources{};
    _proc_layout layout{};
    std::optional<unsigned> cpu{};
    _ev_port* events{}; // Borrowed; outlives the process owner and its watches.
};
class _proc {
    enum class state { New, Running, Stopping, Exited, Closing, Drained };
    enum watch { Life, Exec, Scope, Quota, Watches };
    state state_{};
    bool retiring_{}, dirty_{true};
    _proc_result result_;
    _cap_owner quota_, scope_, vm_, cs_, thread_, sc_, domain_;
    _ch_pair lifecycle_;
    word_t cpus_{};
    _ev_port* events_{};
    std::array<_ev_watch, Watches> watches_;
    auto open(const _proc_auth&, const _proc_limits&) noexcept -> status_t;
    auto prepare(cap_t, _ev_port*, uint32_t) noexcept -> status_t;
    auto close() noexcept -> status_t;
    friend auto _proc_create(const _proc_auth&, const _proc_image&, const _proc_opts&) noexcept -> std::expected<_proc, status_t>;
public:
    _proc() = default;
    _proc(const _proc&) = delete;
    auto operator=(const _proc&) -> _proc& = delete;
    _proc(_proc&&) noexcept = default;
    auto operator=(_proc&& other) noexcept -> _proc& {
        if (this != &other) { this->~_proc(); std::construct_at(this, std::move(other)); }
        return *this;
    }
    ~_proc();
    auto quota() const noexcept -> cap_t { return quota_.selector(); }
    auto scope() const noexcept -> cap_t { return scope_.selector(); }
    auto alloc() const noexcept -> CreateAuth { return {quota(), scope()}; }
    auto vspace() const noexcept -> cap_t { return vm_.selector(); }
    auto domain() const noexcept -> cap_t { return domain_.selector(); }
    auto cspace() const noexcept -> cap_t { return cs_.selector(); }
    auto lifecycle() const noexcept -> cap_t { return lifecycle_.ends[1].selector(); }
    auto initial_thread() const noexcept -> cap_t { return thread_.selector(); }
    auto initial_sc() const noexcept -> cap_t { return sc_.selector(); }
    auto retired() const noexcept -> bool { return state_ == state::Drained; }
    auto stopping() const noexcept -> bool { return state_ >= state::Stopping; }
    auto stopped() const noexcept -> bool { return state_ >= state::Exited; }
    auto start() noexcept -> status_t;
    auto stop(int reason = STATUS_CANCELED) noexcept -> status_t;
    auto poll() noexcept -> status_t;
    auto retire() noexcept -> status_t;
    // Waits through retirement. Timeout leaves execution and ownership intact.
    auto join(word_t deadline = 0) noexcept -> std::expected<_proc_result, status_t>;
    auto result() const noexcept -> std::optional<_proc_result> { return stopped() ? std::optional{result_} : std::nullopt; }
    auto pending() const noexcept -> bool { return dirty_ || (!events_ && quota_) || std::ranges::any_of(watches_, [](const auto& w) { return w.pending(); }); }
};
// create returns a complete, disabled initial Thread; spawn also enables it.
auto _proc_create(const _proc_auth&, const _proc_image&, const _proc_opts& = {}) noexcept -> std::expected<_proc, status_t>;
auto _proc_create(const _proc_image&, const _proc_opts& = {}) noexcept -> std::expected<_proc, status_t>;
auto _proc_spawn(const _proc_auth&, const _proc_image&, const _proc_opts& = {}) noexcept -> std::expected<_proc, status_t>;
auto _proc_spawn(const _proc_image&, const _proc_opts& = {}) noexcept -> std::expected<_proc, status_t>;
