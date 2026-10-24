#pragma once
#include <stdint.h>

// Trivially copyable native ABI types bootstrap both C++ gthread and explicit C adapters.
#ifdef __cplusplus
#define RT_INIT(value) {value}
#else
#define RT_INIT(value)
#endif
typedef struct rt_mutex {
    uint32_t state RT_INIT(0), depth RT_INIT(0), recursive RT_INIT(0);
    uint64_t owner RT_INIT(0);
#ifdef __cplusplus
    void lock();
    auto try_lock() -> bool;
    void unlock();
#endif
} rt_mutex;
typedef struct rt_cond { uint32_t reserved RT_INIT(0); } rt_cond;
typedef struct rt_once { uint32_t state RT_INIT(0); } rt_once;
typedef struct rt_cleanup { void (*function)(void*); void* argument; struct rt_cleanup* next; } rt_cleanup;
#undef RT_INIT
#ifdef __cplusplus
struct timespec;
auto rt_mutex_clocklock(rt_mutex*, const timespec*) -> int;
auto rt_cond_clockwait(rt_cond*, rt_mutex*, const timespec*) -> int;
struct rt_wait_node {
    const void* key{};
    rt_wait_node* next{};
    uint64_t source{};
    bool queued{};
};
auto rt_mutex_lock(rt_mutex*) noexcept -> int;
auto rt_mutex_trylock(rt_mutex*) noexcept -> int;
auto rt_mutex_unlock(rt_mutex*) noexcept -> int;
auto rt_cond_wait(rt_cond*, rt_mutex*, uint64_t deadline = 0) noexcept -> int;
void rt_cond_wake(rt_cond*, bool all = false) noexcept;
void rt_call_once(rt_once*, void (*)());
auto rt_wait32(uint32_t*, uint32_t expected, uint64_t deadline = 0) noexcept -> int;
void rt_wake(const void*, bool all = false) noexcept;
#endif
