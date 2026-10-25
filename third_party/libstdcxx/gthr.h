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
using __gthread_once_t = _once;
using __gthread_mutex_t = _mutex;
using __gthread_recursive_mutex_t = _mutex;
using __gthread_cond_t = _cond;
using __gthread_time_t = timespec;
#define __GTHREAD_ONCE_INIT {}
#define __GTHREAD_MUTEX_INIT {}
#define __GTHREAD_RECURSIVE_MUTEX_INIT {0, 0, 1, 0}
#define __GTHREAD_COND_INIT {}

int _gthread_create(uint64_t*, void* (*)(void*), void*);
int _gthread_join(uint64_t, void**);
int _gthread_detach(uint64_t);
uint64_t _thread_self() noexcept;
void _thread_yield() noexcept;
int _key_create(unsigned*, void (*)(void*));
int _key_delete(unsigned);
int _key_set(unsigned, const void*);
void* _key_get(unsigned);
int _mutex_clocklock(_mutex*, const timespec*);
int _cond_clockwait(_cond*, _mutex*, const timespec*);
uint64_t _clock_ns() noexcept;
void _sleep_ns(uint64_t);
unsigned _cpu_count() noexcept;

static inline int __gthread_active_p() { return 1; }
static inline int __gthread_create(__gthread_t* id, void* (*entry)(void*), void* arg) { return _gthread_create(id, entry, arg); }
static inline int __gthread_join(__gthread_t id, void** value) { return _gthread_join(id, value); }
static inline int __gthread_detach(__gthread_t id) { return _gthread_detach(id); }
static inline int __gthread_equal(__gthread_t a, __gthread_t b) { return a == b; }
static inline __gthread_t __gthread_self() { return _thread_self(); }
static inline int __gthread_yield() { _thread_yield(); return 0; }
static inline int __gthread_once(__gthread_once_t* once, void (*function)()) { _call_once(once, function); return 0; }
static inline int __gthread_key_create(__gthread_key_t* key, void (*destroy)(void*)) { return _key_create(key, destroy); }
static inline int __gthread_key_delete(__gthread_key_t key) { return _key_delete(key); }
static inline void* __gthread_getspecific(__gthread_key_t key) { return _key_get(key); }
static inline int __gthread_setspecific(__gthread_key_t key, const void* value) { return _key_set(key, value); }
static inline void __gthread_mutex_init_function(_mutex* m) { *m = {}; }
static inline int __gthread_mutex_destroy(_mutex* m) { return __atomic_load_n(&m->state, __ATOMIC_RELAXED) ? EBUSY : 0; }
static inline int __gthread_mutex_lock(_mutex* m) { return _mutex_lock(m); }
static inline int __gthread_mutex_trylock(_mutex* m) { return _mutex_trylock(m); }
static inline int __gthread_mutex_unlock(_mutex* m) { return _mutex_unlock(m); }
static inline int __gthread_mutex_timedlock(_mutex*, const timespec*) { return ENOTSUP; }
static inline int __gthread_mutex_clocklock(_mutex* m, clockid_t clock, const timespec* time) { return clock == CLOCK_MONOTONIC ? _mutex_clocklock(m, time) : ENOTSUP; }
static inline int __gthread_recursive_mutex_init_function(_mutex* m) { *m = {}; m->recursive = 1; return 0; }
static inline int __gthread_recursive_mutex_destroy(_mutex* m) { return __gthread_mutex_destroy(m); }
static inline int __gthread_recursive_mutex_lock(_mutex* m) { return _mutex_lock(m); }
static inline int __gthread_recursive_mutex_trylock(_mutex* m) { return _mutex_trylock(m); }
static inline int __gthread_recursive_mutex_unlock(_mutex* m) { return _mutex_unlock(m); }
static inline int __gthread_recursive_mutex_timedlock(_mutex*, const timespec*) { return ENOTSUP; }
static inline int __gthread_cond_destroy(_cond*) { return 0; }
static inline void __gthread_cond_init_function(_cond* c) { *c = {}; }
static inline int __gthread_cond_wait(_cond* c, _mutex* m) { return _cond_wait(c, m); }
static inline int __gthread_cond_wait_recursive(_cond* c, _mutex* m) { return _cond_wait(c, m); }
static inline int __gthread_cond_timedwait(_cond*, _mutex*, const timespec*) { return ENOTSUP; }
static inline int __gthread_cond_clockwait(_cond* c, _mutex* m, clockid_t clock, const timespec* time) { return clock == CLOCK_MONOTONIC ? _cond_clockwait(c, m, time) : ENOTSUP; }
static inline int __gthread_cond_signal(_cond* c) { _cond_wake(c); return 0; }
static inline int __gthread_cond_broadcast(_cond* c) { _cond_wake(c, true); return 0; }
