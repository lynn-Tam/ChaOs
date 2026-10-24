#pragma once
#include <expected>
#include <vm.hpp>
#include <libk/cpio.hpp>
#include <ipc.hpp>
#include <vector>
#include <start.hpp>
#include <string.h>

inline constexpr auto proc_retryable(status_t s) noexcept -> bool {
    return s == STATUS_BUSY || s == STATUS_RETRY;
}

inline constexpr word_t proc_base_kinds = OBJ_BIT(ObjThread) | OBJ_BIT(ObjSc) | OBJ_BIT(ObjDomain)
    | OBJ_BIT(ObjCSpace) | OBJ_BIT(ObjMem) | OBJ_BIT(ObjVSpace)
    | OBJ_BIT(ObjChannel) | OBJ_BIT(ObjIoOp) | OBJ_BIT(ObjVmOp) | OBJ_BIT(ObjQuota) | OBJ_BIT(ObjScope) | OBJ_BIT(ObjNotification) | OBJ_BIT(ObjPort);

struct proc_quota {
    word_t bytes{1024 * 1024}, caps{256}, kinds{proc_base_kinds};
    word_t slots{128}, pages{20}, quota_perms{RIGHT_CREATE | RIGHT_INSPECT};
    word_t watches{64}, hints{16}, threads{64};
    word_t max_bytes{}, max_caps{}; // Zero keeps that dimension's initial ceiling.
};
struct proc_auth { CreateAuth alloc{}; uint32_t cpus{}; cap_t domain{}; };

class proc_task {
    enum class state { New, Running, Closing, Consumed };
    state state_{};
    status_t status_{};
    bool closing_{};
    cap_owner quota_, scope_, vm_, cs_, thread_, sc_, domain_;
    ch_pair lifecycle_;
    struct observer { cap_owner thread; ev_watch exit; uint64_t id{}; word_t role{}; };
    std::vector<observer> threads_;
    size_t thread_limit_{};
    uint64_t vm_id_{}, initial_id_{};
    word_t cpus_{};
    cap_t events_{};
    mutable uint64_t cpu_at_{};
    mutable status_t cpu_status_{};
    mutable std::pair<uint64_t, std::vector<uint64_t>> cpu_;
    std::array<ev_watch, 4> watches_;
    auto close() noexcept -> status_t;
    auto supervise(cap_owner, word_t role, bool reap = false) -> status_t;
public:
    proc_task() = default;
    proc_task(const proc_task&) = delete;
    auto operator=(const proc_task&) -> proc_task& = delete;
    ~proc_task();
    auto quota() const noexcept -> cap_t { return quota_.selector(); }
    auto scope() const noexcept -> cap_t { return scope_.selector(); }
    auto alloc() const noexcept -> CreateAuth { return {quota(), scope()}; }
    auto vspace() const noexcept -> cap_t { return vm_.selector(); }
    auto domain() const noexcept -> cap_t { return domain_.selector(); }
    auto cspace() const noexcept -> cap_t { return cs_.selector(); }
    auto lifecycle() const noexcept -> cap_t { return lifecycle_.ends[1].selector(); }
    auto initial_thread() const noexcept -> cap_t { return thread_.selector(); }
    auto initial_sc() const noexcept -> cap_t { return sc_.selector(); }
    auto closing() const noexcept -> bool { return state_ == state::Closing; }
    auto open(const proc_auth&, const proc_quota&) noexcept -> status_t;
    auto prepare(cap_t desc, cap_t events, uint32_t cpu) noexcept -> status_t;
    auto start() noexcept -> status_t;
    auto execution_info() const noexcept -> std::expected<DomainInfo, status_t>;
    auto execution(bool enabled) noexcept -> sys::SysResult { return sys::domain_execution(domain(), enabled); }
    auto cpu_info() const -> std::expected<std::pair<uint64_t, std::vector<uint64_t>>, status_t>;
    auto memory_info() const noexcept -> std::expected<VSpaceInfo, status_t>;
    auto resource_info() const noexcept -> std::expected<QuotaInfo, status_t>;
    auto observe() noexcept -> sys::SysResult;
    auto stop(status_t reason = STATUS_CANCELED) noexcept -> status_t;
    auto poll() noexcept -> status_t;
    auto polling() const noexcept -> bool { return quota_ && (!events_ || (state_ == state::Closing && !(watches_[2] && watches_[3]))); }
    auto collect() noexcept -> sys::SysResult;
};

#include <variant>
#include <span>
#include <string_view>

// The caller keeps this source mapping and its capability alive through exec.
// perms records the offered view; the kernel still checks every derived map.
struct proc_image {
    std::span<const std::byte> bytes;
    cap_t memory{};
    uint64_t offset{};
    word_t perms{VM_READ};
    static auto map(vm_area&, cap_owner, uint64_t offset, uint64_t size, word_t perms = VM_READ)
        -> std::expected<proc_image, status_t>;
};

struct proc_layout {
    // Static TLS/alignment are added by the loader to this stack reservation.
    word_t stack{0x40010000}, stack_size{0x10000};
    // Launcher defaults; the actual IPC VA travels in ThreadInit and auxv.
    word_t ipc{0x40000000};
};
struct proc_opts {
    std::span<const std::string_view> argv{};
    std::span<const std::string_view> envp{};
    cap_t environment{}; // Opaque, copied into the child; no business startup bindings.
    proc_quota resources{};
    uint32_t cpu{};
    proc_layout va{};
    cap_t events{}; // Borrowed parent receive capability; caller keeps it alive.
};
auto proc_spawn(proc_task& task, const proc_auth& auth, const proc_image& image, const proc_opts& opts = {}) noexcept -> status_t;

struct proc_archive {
    vm_area memory;
    libk::cpio_view files;
    auto open(cap_t cap, size_t bytes) -> status_t;
    auto image(std::string_view name) const -> std::expected<proc_image, status_t>;
};

