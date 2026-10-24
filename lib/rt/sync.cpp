#include <sync.hpp>
#include <start.hpp>
#include <sys/syscall.hpp>
#include <atomic>
#include <cerrno>
#include <time.h>

struct rt_wait_bucket { std::atomic_flag lock{}; rt_wait_node* head{}; };
static constinit rt_wait_bucket buckets[64];
static auto rt_bucket(const void* key) -> rt_wait_bucket& {
    auto value = reinterpret_cast<uintptr_t>(key);
    return buckets[((value >> 3) ^ (value >> 11)) % 64];
}
static void rt_bucket_lock(rt_wait_bucket& bucket) {
    while (bucket.lock.test_and_set(std::memory_order_acquire))
        while (bucket.lock.test(std::memory_order_relaxed)) asm volatile("nop");
}
static void rt_bucket_unlock(rt_wait_bucket& bucket) { bucket.lock.clear(std::memory_order_release); }
static auto rt_park_prepare(const void* key, uint32_t* value = nullptr, uint32_t expected = 0) -> bool {
    auto& current = rt_current();
    if (!current.park) {
        const auto made = sys::notification_create(rt_startup().alloc(), 1);
        if (made.status != STATUS_OK) sys::exit(made.status);
        current.park = made.value;
    }
    auto& bucket = rt_bucket(key); rt_bucket_lock(bucket);
    auto& node = current.waiter;
    if (node.queued) sys::exit(STATUS_INTERNAL);
    (void)sys::notification_take(current.park);
    if (value && __atomic_load_n(value, __ATOMIC_ACQUIRE) != expected) { rt_bucket_unlock(bucket); return false; }
    node.key = key; node.source = current.park; node.queued = true;
    node.next = bucket.head; bucket.head = &node;
    rt_bucket_unlock(bucket); return true;
}
static auto rt_park_wait(uint64_t deadline) -> int {
    auto& current = rt_current(); auto& node = current.waiter;
    auto status = sys::wait_one(current.park, 1, 0, deadline).status;
    auto& bucket = rt_bucket(node.key); rt_bucket_lock(bucket);
    if (node.queued) {
        auto** link = &bucket.head;
        while (*link != &node) link = &(*link)->next;
        *link = node.next; node.queued = false;
    } else status = STATUS_OK;
    (void)sys::notification_take(current.park);
    rt_bucket_unlock(bucket);
    return status == STATUS_OK ? 0 : status == STATUS_TIMED_OUT ? ETIMEDOUT : EINVAL;
}
auto rt_wait32(uint32_t* value, uint32_t expected, uint64_t deadline) noexcept -> int {
    return rt_park_prepare(value, value, expected) ? rt_park_wait(deadline) : 0;
}
void rt_wake(const void* key, bool all) noexcept {
    auto& bucket = rt_bucket(key); rt_bucket_lock(bucket);
    auto** link = &bucket.head;
    while (*link) {
        auto* node = *link;
        if (node->key != key) { link = &node->next; continue; }
        *link = node->next; node->queued = false;
        const auto status = sys::notification_signal(node->source).status;
        if (status != STATUS_OK) sys::exit(status);
        if (!all) break;
    }
    rt_bucket_unlock(bucket);
}
auto rt_mutex_trylock(rt_mutex* mutex) noexcept -> int {
    if (mutex->recursive && __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) == rt_current().tid) {
        if (mutex->depth == UINT32_MAX) return EAGAIN;
        ++mutex->depth; return 0;
    }
    uint32_t expected{};
    if (!__atomic_compare_exchange_n(&mutex->state, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return EBUSY;
    if (mutex->recursive) { __atomic_store_n(&mutex->owner, rt_current().tid, __ATOMIC_RELAXED); mutex->depth = 1; }
    return 0;
}
static auto rt_lock_until(rt_mutex* mutex, uint64_t deadline) noexcept -> int {
    const auto status = rt_mutex_trylock(mutex);
    if (status != EBUSY) return status;
    while (__atomic_exchange_n(&mutex->state, 2, __ATOMIC_ACQUIRE)) {
        const auto waited = rt_wait32(&mutex->state, 2, deadline);
        if (waited) return waited;
    }
    if (mutex->recursive) { __atomic_store_n(&mutex->owner, rt_current().tid, __ATOMIC_RELAXED); mutex->depth = 1; }
    return 0;
}
auto rt_mutex_lock(rt_mutex* mutex) noexcept -> int { return rt_lock_until(mutex, 0); }
static auto rt_ticks(const timespec* time) -> uint64_t {
    if (time->tv_sec < 0) return 1;
    const auto frequency = sys::clock_frequency().value;
    const auto ticks = static_cast<unsigned __int128>(time->tv_sec) * frequency
        + (static_cast<unsigned __int128>(time->tv_nsec) * frequency + 999999999) / 1000000000;
    return ticks > UINT64_MAX ? UINT64_MAX : ticks ? uint64_t(ticks) : 1;
}
auto rt_mutex_clocklock(rt_mutex* mutex, const timespec* time) -> int {
    if (!time || time->tv_nsec < 0 || time->tv_nsec >= 1000000000) return EINVAL;
    return rt_lock_until(mutex, rt_ticks(time));
}
auto rt_cond_clockwait(rt_cond* cond, rt_mutex* mutex, const timespec* time) -> int {
    if (!time || time->tv_nsec < 0 || time->tv_nsec >= 1000000000) return EINVAL;
    return rt_cond_wait(cond, mutex, rt_ticks(time));
}
auto rt_mutex_unlock(rt_mutex* mutex) noexcept -> int {
    if (mutex->recursive) {
        if (__atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) != rt_current().tid) return EPERM;
        if (--mutex->depth) return 0;
        __atomic_store_n(&mutex->owner, uint64_t{0}, __ATOMIC_RELAXED);
    }
    if (__atomic_exchange_n(&mutex->state, 0, __ATOMIC_RELEASE) == 2) rt_wake(&mutex->state);
    return 0;
}
void rt_mutex::lock() { if (rt_mutex_lock(this)) sys::exit(STATUS_INTERNAL); }
auto rt_mutex::try_lock() -> bool { return !rt_mutex_trylock(this); }
void rt_mutex::unlock() { if (rt_mutex_unlock(this)) sys::exit(STATUS_INTERNAL); }
auto rt_cond_wait(rt_cond* cond, rt_mutex* mutex, uint64_t deadline) noexcept -> int {
    (void)rt_park_prepare(cond);
    const auto unlocked = rt_mutex_unlock(mutex);
    // A valid caller owns the mutex exactly once; the node stays live until wait completes.
    if (unlocked) sys::exit(STATUS_INTERNAL);
    const auto status = rt_park_wait(deadline);
    const auto locked = rt_mutex_lock(mutex);
    return locked ? locked : status;
}
void rt_cond_wake(rt_cond* cond, bool all) noexcept { rt_wake(cond, all); }
void rt_call_once(rt_once* once, void (*function)()) {
    uint32_t expected{};
    if (__atomic_compare_exchange_n(&once->state, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        function(); __atomic_store_n(&once->state, 2, __ATOMIC_RELEASE); rt_wake(&once->state, true);
    } else while (__atomic_load_n(&once->state, __ATOMIC_ACQUIRE) != 2) (void)rt_wait32(&once->state, 1);
}
