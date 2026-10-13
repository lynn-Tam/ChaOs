#pragma once
#include <task.hpp>
#include <start.hpp>
#include <variant>
#include <span>
#include <string_view>

inline constexpr rt_req rt_domain{"domain", 0, ObjDomain};
inline constexpr rt_req rt_image{"image", 0, ObjMem};

inline constexpr word_t pr_page_size = 4096;
inline constexpr auto pr_retryable(status_t s) noexcept -> bool {
    return s == STATUS_BUSY || s == STATUS_RETRY;
}
struct pr_window {
    word_t address{}, size{};
    static constexpr auto round_size(word_t n) noexcept -> word_t {
        return n <= ~word_t{} - (pr_page_size - 1) ? (n + pr_page_size - 1) & ~(pr_page_size - 1)
                                                   : 0;
    }
    constexpr auto valid() const noexcept -> bool {
        return address && size && !(address % pr_page_size) && !(size % pr_page_size) &&
               size <= ~word_t{} - address;
    }
    constexpr auto overlaps(pr_window b) const noexcept -> bool {
        return valid() && b.valid() && address < b.address + b.size && b.address < address + size;
    }
};

// The caller keeps this source mapping and its capability alive through exec.
// perms records the offered view; the kernel still checks every derived map.
struct pr_image {
    std::span<const std::byte> bytes;
    cap_t memory{};
    uint64_t offset{};
    word_t perms{VM_READ};
};

struct pr_cap {
    rt_req contract{};
    uint64_t bytes{};
    cap_t cap{};
    std::variant<word_t, CapView> limit{}; // Rights-only duplication or a derived typed view.
    word_t badge{}; // Nonzero requests a Channel endpoint minted by the parent.
};
struct pr_layout {
    pr_window stack{0x40010000, 0x10000};
    word_t bindings{0x40020000};
    // Launcher defaults; the actual IPC VA travels in ThreadInit and auxv.
    word_t ipc{0x40000000};
};
struct pr_opts {
    std::span<const std::string_view> argv{};
    std::span<const pr_cap> caps{};
    pr_quota resources{};
    pr_sched sched{};
    pr_layout va{};
    cap_t events{}; // Borrowed parent receive capability; caller keeps it alive.
};
auto pr_exec(pr_task& task, const pr_auth& auth, const pr_image& image, const pr_opts& opts = {},
             uint32_t first_cpu = 0) noexcept -> status_t;
