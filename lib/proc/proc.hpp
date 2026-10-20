#pragma once
#include <expected>
#include <vm.hpp>
#include <libk/cpio.hpp>
#include <ipc.hpp>
#include <optional>
#include <start.hpp>
#include <string.h>

inline constexpr auto proc_retryable(status_t s) noexcept -> bool {
    return s == STATUS_BUSY || s == STATUS_RETRY;
}

inline constexpr word_t proc_base_kinds = OBJ_BIT(ObjThread) | OBJ_BIT(ObjSc) | OBJ_BIT(ObjDomain)
    | OBJ_BIT(ObjCSpace) | OBJ_BIT(ObjMem) | OBJ_BIT(ObjVSpace)
    | OBJ_BIT(ObjIoOp) | OBJ_BIT(ObjVmOp) | OBJ_BIT(ObjAccount) | OBJ_BIT(ObjScope) | OBJ_BIT(ObjNotification) | OBJ_BIT(ObjPort);

struct proc_quota {
    word_t bytes{1024 * 1024}, caps{256}, kinds{proc_base_kinds};
    word_t slots{128}, pages{20}, pool_perms{RIGHT_CREATE};
    word_t watches{64}, hints{16};
};
struct proc_exit { status_t status{}; };
struct proc_auth { CreateAuth alloc{}; uint32_t cpus{}; cap_t domain{}; };

class proc_task {
    enum class state { New, Running, Closing, Consumed };
    state state_{};
    status_t status_{};
    bool closing_{};
    cap_owner account_, scope_, vm_, cs_, thread_, sc_, domain_;
    word_t cpus_{};
    cap_t events_{};
    std::array<ev_watch, 4> watches_;
public:
    proc_task() = default;
    proc_task(const proc_task&) = delete;
    auto operator=(const proc_task&) -> proc_task& = delete;

    ~proc_task() { if (account_) sys::exit(STATUS_BUSY); }
    auto pool() const noexcept -> cap_t { return account_.selector(); }
    auto scope() const noexcept -> cap_t { return scope_.selector(); }
    auto alloc() const noexcept -> CreateAuth { return {pool(), scope()}; }
    auto vspace() const noexcept -> cap_t { return vm_.selector(); }
    auto domain() const noexcept -> cap_t { return domain_.selector(); }
    auto cspace() const noexcept -> cap_t { return cs_.selector(); }
    auto open(const proc_auth& auth, const proc_quota& q) noexcept -> status_t {
        if (state_ != state::New || account_) return STATUS_BUSY;
        const auto g = sys::account_create(auth.alloc, q.bytes, q.caps, q.kinds);
        if (g.value) account_ = cap_owner{g.value};
        if (g.status != STATUS_OK || !account_) return g.status == STATUS_OK ? STATUS_INVALID_CAP : g.status;
        const auto owner = sys::scope_create(auth.alloc);
        if (owner.value) scope_ = cap_owner{owner.value};
        if (owner.status != STATUS_OK || !scope_) return owner.status == STATUS_OK ? STATUS_INVALID_CAP : owner.status;
        cpus_ = auth.cpus >= 64 ? ~word_t{} : (word_t{1} << auth.cpus) - 1;
        const auto d = sys::domain_create(alloc(), auth.domain, 0, 0, 1024, cpus_);
        if (d.value) domain_ = cap_owner{d.value};
        if (d.status != STATUS_OK || !domain_) return d.status == STATUS_OK ? STATUS_INVALID_CAP : d.status;
        const auto v = sys::vspace_create(alloc());
        if (v.value) vm_ = cap_owner{v.value};
        if (v.status != STATUS_OK || !vm_) return v.status == STATUS_OK ? STATUS_INVALID_CAP : v.status;
        const auto c = sys::cspace_create(alloc(), q.slots, q.pages);
        if (c.value) cs_ = cap_owner{c.value};
        return c.status == STATUS_OK && !cs_ ? STATUS_INVALID_CAP : c.status;
    }
private:
    auto close() noexcept -> status_t {
        if (!account_) return STATUS_OK;
        if (!closing_) {
            if (scope_) {
                const auto status = sys::scope_close(scope()).status;
                if (status != STATUS_OK) return status;
            }
            closing_ = true;
            for (auto* cap : {&thread_, &sc_, &cs_, &vm_, &domain_}) {
                const auto status = cap->close();
                if (status != STATUS_OK) return status;
            }
        }
        if (scope_) {
            const auto status = sys::source_info(scope());
            if (status.status != STATUS_OK) return status.status;
            if (!(status.value & STATE_DRAINED)) return STATUS_WOULD_BLOCK;
            const auto closed = scope_.close();
            if (closed != STATUS_OK) return closed;
        }
        const auto ready = sys::source_info(pool());
        if (ready.status != STATUS_OK) return ready.status;
        if (ready.value & STATE_OPEN) {
            const auto status = sys::account_close(pool()).status;
            return status == STATUS_OK ? STATUS_WOULD_BLOCK : status;
        }
        if (!(ready.value & STATE_DRAINED)) return STATUS_WOULD_BLOCK;
        const auto destroyed = sys::object_destroy(pool()).status;
        if (destroyed != STATUS_OK) return destroyed;
        for (auto& watch : watches_) if (watch) {
            if (const auto status = watch.close(); status != STATUS_OK) return status;
        }
        return account_.close();
    }

public:
    auto thread() const noexcept -> cap_t { return thread_.selector(); }
    auto closing() const noexcept -> bool { return state_ == state::Closing; }
    auto prepare(cap_t desc, cap_t events, uint32_t cpu) noexcept -> status_t {
        if (thread_) return STATUS_BUSY;
        const auto thread = sys::thread_create(alloc(), vspace(), cspace(), desc, 0, cpu);
        thread_ = cap_owner{thread.value};
        if (thread.status != STATUS_OK) return thread.status;
        events_ = events;
        if (!events_) return STATUS_OK;
        const std::array<cap_t, 4> sources{thread_.selector(), thread_.selector(), scope(), pool()};
        for (size_t i = 0; i != sources.size(); ++i) {
            const auto status = watches_[i].open(events_, sources[i], i == 0 ? 0 : i == 1 ? SOURCE_READY : STATE_DRAINED,
                0, i == 0 ? WATCH_CHANGED : 0, i == 1 ? SOURCE_EXIT : SOURCE_STATE);
            if (status != STATUS_OK) return status;
        }
        return STATUS_OK;
    }
    auto start(cap_owner sc = {}) noexcept -> status_t {
        if (!thread_ || sc_) return STATUS_BAD_ARGS;
        if (!sc) {
            const auto created = sys::sc_create(alloc(), domain(), 0, 0, 1024, cpus_);
            if (created.status != STATUS_OK) return created.status;
            sc = cap_owner{created.value};
        }
        sc_ = std::move(sc);
        auto status = sys::sc_bind(sc_.selector(), thread_.selector()).status;
        if (status == STATUS_OK) status = sys::execution_set(thread_.selector(), true).status;
        if (status == STATUS_OK) state_ = state::Running;
        return status;
    }
    auto info() const noexcept -> std::expected<ThreadInfo, status_t> {
        if (!thread_) return std::unexpected(STATUS_CLOSED);
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
    auto resource_info() const noexcept -> std::expected<AccountInfo, status_t> {
        const auto result = sys::account_info(pool());
        if (result.status != STATUS_OK) return std::unexpected(result.status);
        AccountInfo info;
        ::memcpy(&info, reinterpret_cast<const void*>(rt_startup().ipc), sizeof(info));
        return info;
    }
    auto execution(bool enabled) noexcept -> sys::SysResult {
        return sys::execution_set(thread_.selector(), enabled);
    }
    auto observe() noexcept -> sys::SysResult {
        if (state_ == state::Consumed) return {.status = STATUS_INVALID_CAP};
        if (state_ == state::Closing) return {.status = STATUS_OK, .value = 1, .value2 = static_cast<word_t>(status_)};
        if (state_ == state::Running && watches_[0]) {
            const auto fact = sys::source_info(thread_.selector());
            if (fact.status != STATUS_OK) return fact;
            const auto armed = watches_[0].arm(fact.value2);
            if (armed.status != STATUS_OK) return armed;
        }
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
    auto result() const noexcept -> std::optional<proc_exit> { return state_ == state::Closing && !account_ ? std::optional{proc_exit{status_}} : std::nullopt; }
    // Only a partially built execution can lack a real retirement doorbell.
    auto polling() const noexcept -> bool {
        return state_ == state::Closing && account_ && !(watches_[1] && watches_[2] && watches_[3]);
    }
    auto collect() noexcept -> sys::SysResult {
        const auto status = poll();
        if (status != STATUS_OK) return {.status = status};
        state_ = state::Consumed;
        return {.status = STATUS_OK, .value = static_cast<word_t>(status_)};
    }
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
};

struct proc_cap {
    rt_key key{};
    uint64_t bytes{};
    std::variant<cap_t, cap_owner*> cap{}; // Borrow/copy, or consume an owned capability.
    std::variant<word_t, CapView> limit{}; // Rights-only duplication or a derived typed view.
    word_t badge{}; // Nonzero requests a Channel endpoint minted by the parent.
};
struct proc_layout {
    word_t stack{0x40010000}, stack_size{0x10000};
    word_t bindings{0x40020000};
    // Launcher defaults; the actual IPC VA travels in ThreadInit and auxv.
    word_t ipc{0x40000000};
};
struct proc_opts {
    std::span<const std::string_view> argv{};
    std::span<const std::string_view> envp{};
    std::span<const proc_cap> caps{};
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

