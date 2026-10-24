#pragma once
#include <uapi/start.h>
#include <span>
#include <cstddef>
#include <sync.hpp>

extern "C" char** environ;

// Native execution context only. The runtime never interprets the environment.
struct rt_env {
    cap_t quota{}, scope{}, vspace{}, cspace{}, domain{}, environment{}, lifecycle{};
    auto alloc() const noexcept -> CreateAuth { return {quota, scope}; }
    word_t cpus{};
    std::span<const std::byte> boot{};
};
auto rt_startup() noexcept -> const rt_env&;
auto rt_root() noexcept -> const RootInfo&;

struct rt_context {
    word_t ipc{};
    cap_t events{}, park{};
    uint64_t tid{1};
    void* thread{};
    rt_wait_node waiter;
};
extern thread_local rt_context rt_local;
inline auto rt_current() noexcept -> rt_context& { return rt_local; }
auto rt_events() noexcept -> cap_t;

inline constexpr word_t rt_life_version = 1, rt_life_register = 1, rt_life_exit = 2, rt_life_reap = 3;
struct rt_bootstrap {
    uintptr_t stack{};
    size_t stack_size{};
    cap_t thread{}, sc{}, stack_memory{}, ipc_memory{};
};
auto rt_initial() noexcept -> const rt_bootstrap&;
