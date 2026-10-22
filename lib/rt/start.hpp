#pragma once
#include <uapi/start.h>
#include <span>
#include <cstddef>

extern "C" char** environ;

// Native execution context only. The runtime never interprets the environment.
struct rt_env {
    cap_t quota{}, scope{}, vspace{}, cspace{}, events{}, domain{}, environment{};
    auto alloc() const noexcept -> CreateAuth { return {quota, scope}; }
    word_t cpus{}, ipc{};
    std::span<const std::byte> boot{};
};
auto rt_startup() noexcept -> const rt_env&;
auto rt_root() noexcept -> const RootInfo&;
