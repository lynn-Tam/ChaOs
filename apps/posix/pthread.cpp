#include <pthread.h>
#include <thread.hpp>
#include <sys/syscall.hpp>

extern "C" int pthread_create(pthread_t* id, const pthread_attr_t* attr, rt_entry entry, void* argument) {
    if (!id || !entry) return EINVAL;
    const auto made = attr ? rt_thread_create(entry, argument, {attr->stack_size, attr->guard_size, bool(attr->detached)})
                           : rt_thread_create(entry, argument);
    if (!made) return rt_errno(made.error());
    *id = *made; return 0;
}
extern "C" int pthread_join(pthread_t id, void** value) {
    const auto result = rt_thread_join(id); if (!result) return rt_errno(result.error());
    if (value) *value = *result;
    return 0;
}
extern "C" int pthread_detach(pthread_t id) { return rt_errno(rt_thread_detach(id)); }
extern "C" pthread_t pthread_self() { return rt_thread_self(); }
extern "C" int pthread_equal(pthread_t a, pthread_t b) { return a == b; }
extern "C" [[noreturn]] void pthread_exit(void* value) { rt_thread_exit(value); }
extern "C" int pthread_attr_init(pthread_attr_t* a) { *a = {64 * 1024, 4096, PTHREAD_CREATE_JOINABLE}; return 0; }
extern "C" int pthread_attr_destroy(pthread_attr_t*) { return 0; }
extern "C" int pthread_attr_setdetachstate(pthread_attr_t* a, int value) {
    if (value != PTHREAD_CREATE_JOINABLE && value != PTHREAD_CREATE_DETACHED) return EINVAL;
    a->detached = value; return 0;
}
extern "C" int pthread_attr_getdetachstate(const pthread_attr_t* a, int* value) { *value = a->detached; return 0; }
extern "C" int pthread_attr_setstacksize(pthread_attr_t* a, size_t size) {
    if (size < PTHREAD_STACK_MIN || size > SIZE_MAX / 2) return EINVAL;
    a->stack_size = size; return 0;
}
extern "C" int pthread_attr_getstacksize(const pthread_attr_t* a, size_t* size) { *size = a->stack_size; return 0; }
extern "C" int pthread_attr_setguardsize(pthread_attr_t* a, size_t size) {
    if (size > SIZE_MAX / 2) return EINVAL;
    a->guard_size = size; return 0;
}
extern "C" int pthread_attr_getguardsize(const pthread_attr_t* a, size_t* size) { *size = a->guard_size; return 0; }
extern "C" int pthread_mutexattr_init(pthread_mutexattr_t* a) { *a = {}; return 0; }
extern "C" int pthread_mutexattr_destroy(pthread_mutexattr_t*) { return 0; }
extern "C" int pthread_mutexattr_settype(pthread_mutexattr_t* a, int type) {
    if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE) return ENOTSUP;
    a->type = type; return 0;
}
extern "C" int pthread_mutexattr_gettype(const pthread_mutexattr_t* a, int* type) { *type = a->type; return 0; }
extern "C" int pthread_mutexattr_setpshared(pthread_mutexattr_t*, int type) { return type == PTHREAD_PROCESS_PRIVATE ? 0 : ENOTSUP; }
extern "C" int pthread_mutexattr_setprotocol(pthread_mutexattr_t*, int type) { return type == PTHREAD_PRIO_NONE ? 0 : ENOTSUP; }
extern "C" int pthread_mutexattr_setrobust(pthread_mutexattr_t*, int) { return ENOTSUP; }
extern "C" int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a) { *m = {}; m->recursive = a && a->type == PTHREAD_MUTEX_RECURSIVE; return 0; }
extern "C" int pthread_mutex_destroy(pthread_mutex_t* m) { return __atomic_load_n(&m->state, __ATOMIC_RELAXED) ? EBUSY : 0; }
extern "C" int pthread_mutex_lock(pthread_mutex_t* m) { return rt_mutex_lock(m); }
extern "C" int pthread_mutex_trylock(pthread_mutex_t* m) { return rt_mutex_trylock(m); }
extern "C" int pthread_mutex_unlock(pthread_mutex_t* m) { return rt_mutex_unlock(m); }
extern "C" int pthread_mutex_timedlock(pthread_mutex_t*, const timespec*) { return ENOTSUP; }
extern "C" int pthread_mutex_clocklock(pthread_mutex_t* m, clockid_t clock, const timespec* time) {
    return clock == CLOCK_MONOTONIC ? rt_mutex_clocklock(m, time) : ENOTSUP;
}
extern "C" int pthread_condattr_init(pthread_condattr_t* a) { a->clock = CLOCK_REALTIME; return 0; }
extern "C" int pthread_condattr_destroy(pthread_condattr_t*) { return 0; }
extern "C" int pthread_condattr_setclock(pthread_condattr_t* a, clockid_t clock) {
    if (clock != CLOCK_MONOTONIC) return ENOTSUP;
    a->clock = clock; return 0;
}
extern "C" int pthread_condattr_getclock(const pthread_condattr_t* a, clockid_t* clock) { *clock = a->clock; return 0; }
extern "C" int pthread_condattr_setpshared(pthread_condattr_t*, int type) { return type == PTHREAD_PROCESS_PRIVATE ? 0 : ENOTSUP; }
extern "C" int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* a) { *c = {{}, a ? a->clock : CLOCK_REALTIME}; return 0; }
extern "C" int pthread_cond_destroy(pthread_cond_t*) { return 0; } // Caller owns the no-waiters precondition.
extern "C" int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) { return rt_cond_wait(&c->wait, m); }
extern "C" int pthread_cond_clockwait(pthread_cond_t* c, pthread_mutex_t* m, clockid_t clock, const timespec* time) {
    return clock == CLOCK_MONOTONIC ? rt_cond_clockwait(&c->wait, m, time) : ENOTSUP;
}
extern "C" int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const timespec* time) { return pthread_cond_clockwait(c, m, c->clock, time); }
extern "C" int pthread_cond_signal(pthread_cond_t* c) { rt_cond_wake(&c->wait); return 0; }
extern "C" int pthread_cond_broadcast(pthread_cond_t* c) { rt_cond_wake(&c->wait, true); return 0; }
extern "C" int pthread_once(pthread_once_t* once, void (*function)()) { rt_call_once(once, function); return 0; }
extern "C" int pthread_key_create(pthread_key_t* key, void (*destroy)(void*)) { return rt_key_create(key, destroy); }
extern "C" int pthread_key_delete(pthread_key_t key) { return rt_key_delete(key); }
extern "C" int pthread_setspecific(pthread_key_t key, const void* value) { return rt_key_set(key, value); }
extern "C" void* pthread_getspecific(pthread_key_t key) { return rt_key_get(key); }
extern "C" int pthread_cancel(pthread_t) { return ENOTSUP; }
extern "C" void px_cleanup_push(rt_cleanup* node, void (*function)(void*), void* argument) { rt_cleanup_push(node, function, argument); }
extern "C" void px_cleanup_pop(rt_cleanup* node, int execute) { rt_cleanup_pop(node, execute); }
