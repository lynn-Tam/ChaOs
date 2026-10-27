#pragma once
#include <uapi/start.h>
#include <span>
#include <cstddef>
#include <sync.hpp>

extern "C" char** environ;

// Native execution context only. The runtime never interprets the environment.
struct _env {
    cap_t quota{}, scope{}, vspace{}, cspace{}, domain{}, bootstrap{}, lifecycle{}, pager{};
    auto alloc() const noexcept -> CreateAuth { return {quota, scope}; }
    word_t cpus{}, frequency{};
    std::span<const std::byte> boot{};
};
auto _startup() noexcept -> const _env&;
auto _root() noexcept -> const RootInfo&;

struct _ev_port;
auto _events() noexcept -> _ev_port&;
auto _ipc() noexcept -> word_t;
