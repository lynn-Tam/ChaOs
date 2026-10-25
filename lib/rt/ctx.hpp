#pragma once
#include <start.hpp>
#include <thread.hpp>

struct _wait_node {
    const void* key{};
    _wait_node* next{};
    uint64_t source{};
    bool queued{};
};
struct _context {
    word_t ipc{};
    _ev_port* events{};
    cap_t event_cap{}, park{};
    uint64_t tid{1};
    void* thread{};
    _wait_node waiter;
};
extern thread_local _context _local;
inline auto _current() noexcept -> _context& { return _local; }
struct _bootstrap {
    uintptr_t stack{};
    size_t stack_size{}, guard_size{};
    cap_t thread{}, sc{}, stack_memory{}, ipc_memory{}, stack_region{}, ipc_region{};
};
auto _initial() noexcept -> const _bootstrap&;
void _thread_init();
void _thread_fini();
void _finalize();
