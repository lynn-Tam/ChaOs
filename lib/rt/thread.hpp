#pragma once
#include <start.hpp>
#include <sys/syscall.hpp>
#include <expected>
#include <chrono>
#include <optional>
#include <algorithm>

using _thread = uint64_t;
using _entry = void* (*)(void*);
struct _thread_attr { size_t stack_size{64 * 1024}, guard_size{4096}; bool detached{}; };
auto _thread_create(_entry, void*, const _thread_attr& = {}) -> std::expected<_thread, status_t>;
auto _thread_join(_thread) -> std::expected<void*, status_t>;
auto _thread_detach(_thread) -> status_t;
auto _thread_self() noexcept -> _thread;
[[noreturn]] void _thread_exit(void*);
[[noreturn]] void _exit(int);
auto _key_create(unsigned*, void (*)(void*)) -> int;
auto _key_delete(unsigned) -> int;
auto _key_set(unsigned, const void*) -> int;
auto _key_get(unsigned) -> void*;

auto _errno(status_t) noexcept -> int;

void _cleanup_push(_cleanup*, void (*)(void*), void*);
void _cleanup_pop(_cleanup*, bool);
// Library state outlives application TLS and key destructors.
void _cleanup_defer(_cleanup*, void (*)(void*), void*);

void _thread_yield() noexcept;
auto _cpu_count() noexcept -> unsigned;
auto _clock_ns() noexcept -> uint64_t;
void _sleep_ns(uint64_t);

// Checked rational conversion; deadlines round toward the next hardware tick.
auto _time_scale(unsigned __int128, uint64_t numerator, uint64_t denominator, bool ceil = false) noexcept -> std::optional<uint64_t>;
template<class Rep, class Period> requires std::is_integral_v<Rep>
auto _after(std::chrono::duration<Rep, Period> duration) noexcept -> std::optional<uint64_t> {
    if (duration.count() < 0) return std::nullopt;
    const auto ticks = _time_scale(static_cast<unsigned __int128>(duration.count()) * Period::num,
        _startup().frequency, Period::den, true);
    const auto now = _clock_now();
    if (!ticks || now.status != STATUS_OK || *ticks > UINT64_MAX - now.value) return std::nullopt;
    return std::max(uint64_t{1}, now.value + *ticks);
}
inline auto _deadline_status(uint64_t deadline) noexcept -> status_t {
    if (!deadline) return STATUS_OK;
    const auto now = _clock_now();
    return now.status != STATUS_OK ? now.status : now.value >= deadline ? STATUS_TIMED_OUT : STATUS_OK;
}
