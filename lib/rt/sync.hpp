#pragma once
#include <stdint.h>

// Trivially copyable native ABI types bootstrap both C++ gthread and explicit C adapters.
#ifdef __cplusplus
#define RT_INIT(value) {value}
#else
#define RT_INIT(value)
#endif
typedef struct _mutex {
    uint32_t state RT_INIT(0), depth RT_INIT(0), recursive RT_INIT(0);
    uint64_t owner RT_INIT(0);
#ifdef __cplusplus
    void lock();
    auto try_lock() -> bool;
    void unlock();
#endif
} _mutex;
typedef struct _cond { uint32_t reserved RT_INIT(0); } _cond;
typedef struct _once { uint32_t state RT_INIT(0); } _once;
typedef struct _cleanup { void (*function)(void*); void* argument; struct _cleanup* next; } _cleanup;
#undef RT_INIT
#ifdef __cplusplus
struct timespec;
auto _mutex_clocklock(_mutex*, const timespec*) -> int;
auto _cond_clockwait(_cond*, _mutex*, const timespec*) -> int;
auto _mutex_lock(_mutex*) noexcept -> int;
auto _mutex_trylock(_mutex*) noexcept -> int;
auto _mutex_unlock(_mutex*) noexcept -> int;
auto _cond_wait(_cond*, _mutex*, uint64_t deadline = 0) noexcept -> int;
void _cond_wake(_cond*, bool all = false) noexcept;
void _call_once(_once*, void (*)());
auto _wait32(uint32_t*, uint32_t expected, uint64_t deadline = 0) noexcept -> int;
void _wake(const void*, bool all = false) noexcept;
#endif
