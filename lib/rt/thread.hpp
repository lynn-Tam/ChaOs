#pragma once
#include <start.hpp>
#include <expected>

using rt_thread = uint64_t;
using rt_entry = void* (*)(void*);
struct rt_thread_attr { size_t stack_size{64 * 1024}, guard_size{4096}; bool detached{}; };
auto rt_thread_create(rt_entry, void*, const rt_thread_attr& = {}) -> std::expected<rt_thread, status_t>;
auto rt_thread_join(rt_thread) -> std::expected<void*, status_t>;
auto rt_thread_detach(rt_thread) -> status_t;
auto rt_thread_self() noexcept -> rt_thread;
[[noreturn]] void rt_thread_exit(void*);
[[noreturn]] void rt_exit(int);
void rt_thread_init();
void rt_thread_fini();
void rt_finalize();
auto rt_key_create(unsigned*, void (*)(void*)) -> int;
auto rt_key_delete(unsigned) -> int;
auto rt_key_set(unsigned, const void*) -> int;
auto rt_key_get(unsigned) -> void*;

auto rt_errno(status_t) noexcept -> int;

void rt_cleanup_push(rt_cleanup*, void (*)(void*), void*);
void rt_cleanup_pop(rt_cleanup*, bool);
// Library state outlives application TLS and key destructors.
void rt_cleanup_defer(rt_cleanup*, void (*)(void*), void*);

void rt_thread_yield() noexcept;
auto rt_cpu_count() noexcept -> unsigned;
auto rt_clock_ns() noexcept -> uint64_t;
void rt_sleep_ns(uint64_t);
