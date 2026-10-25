#include "ctx.hpp"
#include <sync.hpp>
#include <start.hpp>
#include <sys/syscall.hpp>
#include <atomic>
#include <cerrno>
#include <time.h>

struct _wait_bucket { std::atomic_flag lock{}; _wait_node* head{}; };
static constinit _wait_bucket buckets[64];
static auto _bucket(const void* key) -> _wait_bucket& {
    auto value = reinterpret_cast<uintptr_t>(key);
    return buckets[((value >> 3) ^ (value >> 11)) % 64];
}
static void _bucket_lock(_wait_bucket& bucket) {
    while (bucket.lock.test_and_set(std::memory_order_acquire))
        while (bucket.lock.test(std::memory_order_relaxed)) asm volatile("nop");
}
static void _bucket_unlock(_wait_bucket& bucket) { bucket.lock.clear(std::memory_order_release); }
static auto _park_prepare(const void* key, uint32_t* value = nullptr, uint32_t expected = 0) -> bool {
    auto& current = _current();
    if (!current.park) {
        const auto made = _notification_create(_startup().alloc(), 1);
        if (made.status != STATUS_OK) _thread_exit(made.status);
        current.park = made.value;
    }
    auto& bucket = _bucket(key); _bucket_lock(bucket);
    auto& node = current.waiter;
    if (node.queued) _thread_exit(STATUS_INTERNAL);
    (void)_notification_take(current.park);
    if (value && __atomic_load_n(value, __ATOMIC_ACQUIRE) != expected) { _bucket_unlock(bucket); return false; }
    node.key = key; node.source = current.park; node.queued = true;
    node.next = bucket.head; bucket.head = &node;
    _bucket_unlock(bucket); return true;
}
static auto _park_wait(uint64_t deadline) -> int {
    auto& current = _current(); auto& node = current.waiter;
    auto status = _wait_one(current.park, 1, 0, deadline).status;
    auto& bucket = _bucket(node.key); _bucket_lock(bucket);
    if (node.queued) {
        auto** link = &bucket.head;
        while (*link != &node) link = &(*link)->next;
        *link = node.next; node.queued = false;
    } else status = STATUS_OK;
    (void)_notification_take(current.park);
    _bucket_unlock(bucket);
    return status == STATUS_OK ? 0 : status == STATUS_TIMED_OUT ? ETIMEDOUT : EINVAL;
}
auto _wait32(uint32_t* value, uint32_t expected, uint64_t deadline) noexcept -> int {
    return _park_prepare(value, value, expected) ? _park_wait(deadline) : 0;
}
void _wake(const void* key, bool all) noexcept {
    auto& bucket = _bucket(key); _bucket_lock(bucket);
    auto** link = &bucket.head;
    while (*link) {
        auto* node = *link;
        if (node->key != key) { link = &node->next; continue; }
        *link = node->next; node->queued = false;
        const auto status = _notification_signal(node->source).status;
        if (status != STATUS_OK) _thread_exit(status);
        if (!all) break;
    }
    _bucket_unlock(bucket);
}
auto _mutex_trylock(_mutex* mutex) noexcept -> int {
    if (mutex->recursive && __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) == _current().tid) {
        if (mutex->depth == UINT32_MAX) return EAGAIN;
        ++mutex->depth; return 0;
    }
    uint32_t expected{};
    if (!__atomic_compare_exchange_n(&mutex->state, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return EBUSY;
    if (mutex->recursive) { __atomic_store_n(&mutex->owner, _current().tid, __ATOMIC_RELAXED); mutex->depth = 1; }
    return 0;
}
static auto _lock_until(_mutex* mutex, uint64_t deadline) noexcept -> int {
    const auto status = _mutex_trylock(mutex);
    if (status != EBUSY) return status;
    while (__atomic_exchange_n(&mutex->state, 2, __ATOMIC_ACQUIRE)) {
        const auto waited = _wait32(&mutex->state, 2, deadline);
        if (waited) return waited;
    }
    if (mutex->recursive) { __atomic_store_n(&mutex->owner, _current().tid, __ATOMIC_RELAXED); mutex->depth = 1; }
    return 0;
}
auto _mutex_lock(_mutex* mutex) noexcept -> int { return _lock_until(mutex, 0); }
static auto _ticks(const timespec* time) -> uint64_t {
    if (time->tv_sec < 0) return 1;
    const auto ns = static_cast<unsigned __int128>(time->tv_sec) * 1000000000 + time->tv_nsec;
    return std::max(uint64_t{1}, _time_scale(ns, _startup().frequency, 1000000000, true).value_or(UINT64_MAX));
}
auto _mutex_clocklock(_mutex* mutex, const timespec* time) -> int {
    if (!time || time->tv_nsec < 0 || time->tv_nsec >= 1000000000) return EINVAL;
    return _lock_until(mutex, _ticks(time));
}
auto _cond_clockwait(_cond* cond, _mutex* mutex, const timespec* time) -> int {
    if (!time || time->tv_nsec < 0 || time->tv_nsec >= 1000000000) return EINVAL;
    return _cond_wait(cond, mutex, _ticks(time));
}
auto _mutex_unlock(_mutex* mutex) noexcept -> int {
    if (mutex->recursive) {
        if (__atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) != _current().tid) return EPERM;
        if (--mutex->depth) return 0;
        __atomic_store_n(&mutex->owner, uint64_t{0}, __ATOMIC_RELAXED);
    }
    if (__atomic_exchange_n(&mutex->state, 0, __ATOMIC_RELEASE) == 2) _wake(&mutex->state);
    return 0;
}
void _mutex::lock() { if (_mutex_lock(this)) _thread_exit(STATUS_INTERNAL); }
auto _mutex::try_lock() -> bool { return !_mutex_trylock(this); }
void _mutex::unlock() { if (_mutex_unlock(this)) _thread_exit(STATUS_INTERNAL); }
auto _cond_wait(_cond* cond, _mutex* mutex, uint64_t deadline) noexcept -> int {
    (void)_park_prepare(cond);
    const auto unlocked = _mutex_unlock(mutex);
    // A valid caller owns the mutex exactly once; the node stays live until wait completes.
    if (unlocked) _thread_exit(STATUS_INTERNAL);
    const auto status = _park_wait(deadline);
    const auto locked = _mutex_lock(mutex);
    return locked ? locked : status;
}
void _cond_wake(_cond* cond, bool all) noexcept { _wake(cond, all); }
void _call_once(_once* once, void (*function)()) {
    uint32_t expected{};
    if (__atomic_compare_exchange_n(&once->state, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        function(); __atomic_store_n(&once->state, 2, __ATOMIC_RELEASE); _wake(&once->state, true);
    } else while (__atomic_load_n(&once->state, __ATOMIC_ACQUIRE) != 2) (void)_wait32(&once->state, 1);
}
