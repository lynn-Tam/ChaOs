#pragma once
// GCC gthread ABI over the native runtime. No hosted/POSIX dependency.
#include <sync.hpp>
#include <time.h>
#include <errno.h>
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 4
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 1
#endif

static_assert(__atomic_always_lock_free(4, nullptr) && __atomic_always_lock_free(8, nullptr));

#define __GTHREADS 1
#define __GTHREADS_CXX0X 1
#define __GTHREAD_HAS_COND 1
using __gthread_t = uint64_t;
using __gthread_key_t = unsigned;
using __gthread_once_t = rt_once;
using __gthread_mutex_t = rt_mutex;
using __gthread_recursive_mutex_t = rt_mutex;
using __gthread_cond_t = rt_cond;
using __gthread_time_t = timespec;
#define __GTHREAD_ONCE_INIT {}
#define __GTHREAD_MUTEX_INIT {}
#define __GTHREAD_RECURSIVE_MUTEX_INIT {0, 0, 1, 0}
#define __GTHREAD_COND_INIT {}

int rt_gthread_create(uint64_t*, void* (*)(void*), void*);
int rt_gthread_join(uint64_t, void**);
int rt_gthread_detach(uint64_t);
uint64_t rt_thread_self() noexcept;
void rt_thread_yield() noexcept;
int rt_key_create(unsigned*, void (*)(void*));
int rt_key_delete(unsigned);
int rt_key_set(unsigned, const void*);
void* rt_key_get(unsigned);
int rt_mutex_clocklock(rt_mutex*, const timespec*);
int rt_cond_clockwait(rt_cond*, rt_mutex*, const timespec*);
uint64_t rt_clock_ns() noexcept;
void rt_sleep_ns(uint64_t);
unsigned rt_cpu_count() noexcept;

static inline int __gthread_active_p() { return 1; }
static inline int __gthread_create(__gthread_t* id, void* (*entry)(void*), void* arg) { return rt_gthread_create(id, entry, arg); }
static inline int __gthread_join(__gthread_t id, void** value) { return rt_gthread_join(id, value); }
static inline int __gthread_detach(__gthread_t id) { return rt_gthread_detach(id); }
static inline int __gthread_equal(__gthread_t a, __gthread_t b) { return a == b; }
static inline __gthread_t __gthread_self() { return rt_thread_self(); }
static inline int __gthread_yield() { rt_thread_yield(); return 0; }
static inline int __gthread_once(__gthread_once_t* once, void (*function)()) { rt_call_once(once, function); return 0; }
static inline int __gthread_key_create(__gthread_key_t* key, void (*destroy)(void*)) { return rt_key_create(key, destroy); }
static inline int __gthread_key_delete(__gthread_key_t key) { return rt_key_delete(key); }
static inline void* __gthread_getspecific(__gthread_key_t key) { return rt_key_get(key); }
static inline int __gthread_setspecific(__gthread_key_t key, const void* value) { return rt_key_set(key, value); }
static inline void __gthread_mutex_init_function(rt_mutex* m) { *m = {}; }
static inline int __gthread_mutex_destroy(rt_mutex* m) { return __atomic_load_n(&m->state, __ATOMIC_RELAXED) ? EBUSY : 0; }
static inline int __gthread_mutex_lock(rt_mutex* m) { return rt_mutex_lock(m); }
static inline int __gthread_mutex_trylock(rt_mutex* m) { return rt_mutex_trylock(m); }
static inline int __gthread_mutex_unlock(rt_mutex* m) { return rt_mutex_unlock(m); }
static inline int __gthread_mutex_timedlock(rt_mutex*, const timespec*) { return ENOTSUP; }
static inline int __gthread_mutex_clocklock(rt_mutex* m, clockid_t clock, const timespec* time) { return clock == CLOCK_MONOTONIC ? rt_mutex_clocklock(m, time) : ENOTSUP; }
static inline int __gthread_recursive_mutex_init_function(rt_mutex* m) { *m = {}; m->recursive = 1; return 0; }
static inline int __gthread_recursive_mutex_destroy(rt_mutex* m) { return __gthread_mutex_destroy(m); }
static inline int __gthread_recursive_mutex_lock(rt_mutex* m) { return rt_mutex_lock(m); }
static inline int __gthread_recursive_mutex_trylock(rt_mutex* m) { return rt_mutex_trylock(m); }
static inline int __gthread_recursive_mutex_unlock(rt_mutex* m) { return rt_mutex_unlock(m); }
static inline int __gthread_recursive_mutex_timedlock(rt_mutex*, const timespec*) { return ENOTSUP; }
static inline int __gthread_cond_destroy(rt_cond*) { return 0; }
static inline void __gthread_cond_init_function(rt_cond* c) { *c = {}; }
static inline int __gthread_cond_wait(rt_cond* c, rt_mutex* m) { return rt_cond_wait(c, m); }
static inline int __gthread_cond_wait_recursive(rt_cond* c, rt_mutex* m) { return rt_cond_wait(c, m); }
static inline int __gthread_cond_timedwait(rt_cond*, rt_mutex*, const timespec*) { return ENOTSUP; }
static inline int __gthread_cond_clockwait(rt_cond* c, rt_mutex* m, clockid_t clock, const timespec* time) { return clock == CLOCK_MONOTONIC ? rt_cond_clockwait(c, m, time) : ENOTSUP; }
static inline int __gthread_cond_signal(rt_cond* c) { rt_cond_wake(c); return 0; }
static inline int __gthread_cond_broadcast(rt_cond* c) { rt_cond_wake(c, true); return 0; }
