#pragma once
#include <task.hpp>
#include <start.hpp>
#include <variant>
#include <span>
#include <string_view>

inline constexpr rt_req rt_domain{"domain", 0, ObjDomain};
inline constexpr rt_req rt_image{"image", 0, ObjMem};

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
    word_t stack{0x40010000}, stack_size{0x10000};
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
