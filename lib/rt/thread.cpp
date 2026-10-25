#include "ctx.hpp"
#include <thread.hpp>
#include <ipc.hpp>
#include <vm.hpp>
#include <mutex>
#include <atomic>
#include <bit>
#include <cerrno>
#include <memory>
#include <cxxabi.h>
#include <chrono>
#include <exception>

extern "C" void _thread_start();
extern "C" const std::byte __tls_start[], __tls_data_end[], __tls_end[], __tls_align[];
struct _value { unsigned key; void* value; _value* next; };
struct _key { unsigned id; void (*destroy)(void*); _key* next; };
struct _storage {
    _context* context{};
    _vm_area stack, ipc;
    _cap_owner thread, sc;
    _ev_watch stopped;
};
struct _record {
    _record* next{}, *pending_next{};
    std::atomic<unsigned> refs{2};
    _thread id{};
    _entry entry{};
    void* argument{};
    std::unique_ptr<_storage> storage;
    uint32_t reaped{};
    void* result{};
    bool internal{}, detached{}, joining{};
    std::atomic<bool> published{};
};
static constinit _mutex registry_mutex, key_mutex;
static _record* records;
static _record* pending;
static _record** pending_tail = &pending;
static _ev_port* reaper_events;
static _key* keys;
static unsigned next_key;
static uint64_t next_id{1};
static size_t users;
static constinit _mutex reaper_mutex;
static _ev_notice* reaper_notice;
static uint32_t reaper_ready;
static std::atomic<_thread> exit_owner;
static thread_local _cleanup* dtors;
static thread_local _cleanup* cleanups;
static thread_local _cleanup* deferred;
static thread_local _value* values;

static void _release(_record* record) {
    if (record->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete record;
}
static void _unlink(_record* record) {
    auto** link = &records;
    while (*link != record) link = &(*link)->next;
    *link = record->next;
}
static auto _find(_thread id) -> _record* {
    for (auto* record = records; record; record = record->next) if (record->id == id) return record;
    return nullptr;
}
static void _destroy(_cap_owner& cap) {
    if (!cap) return;
    const auto status = _object_destroy(cap.selector()).status;
    if (status != STATUS_OK) _thread_exit(status);
    cap = {};
}
static void _stop(cap_t thread) {
    const auto stopped = _execution_stop(thread).status;
    if (stopped != STATUS_OK) _thread_exit(stopped);
    const auto waited = _wait_one(thread, THREAD_STOPPED).status;
    if (waited != STATUS_OK) _thread_exit(waited);
}
static void _unmap(_vm_area& area) {
    auto memory = std::move(area.memory);
    const auto status = area.close();
    if (status != STATUS_OK) _thread_exit(status);
    _destroy(memory);
}
static void _reclaim(_record& record) {
    auto& storage = *record.storage;
    const auto status = storage.stopped.close(); if (status != STATUS_OK) _thread_exit(status);
    if (storage.context) {
        delete storage.context->events;
        for (auto selector : {storage.context->event_cap, storage.context->park}) if (selector) {
            _cap_owner cap{selector}; _destroy(cap);
        }
    }
    _unmap(storage.stack); _unmap(storage.ipc);
    _destroy(storage.sc); _destroy(storage.thread);
    record.storage.reset();
}
static void _signal_reaper() {
    if (reaper_notice) {
        const auto status = _notification_signal(reaper_notice->selector()).status;
        if (status != STATUS_OK) _thread_exit(status);
    }
}
static auto _prepare(_entry entry, void* argument, const _thread_attr& attr, bool internal) -> std::expected<_record*, status_t> {
    const auto& env = _startup();
    _thread id;
    { std::lock_guard lock{registry_mutex};
      if (next_id == UINT64_MAX) return std::unexpected(STATUS_EXHAUSTED);
      id = ++next_id; }
    auto* record = new (std::nothrow) _record;
    if (!record) return std::unexpected(STATUS_NO_MEMORY);
    record->storage.reset(new (std::nothrow) _storage);
    if (!record->storage) { delete record; return std::unexpected(STATUS_NO_MEMORY); }
    record->id = id;
    _cap_owner descriptor;
    auto& storage = *record->storage;
    record->entry = entry; record->argument = argument; record->internal = internal; record->detached = attr.detached;
    const auto build = [&]() -> status_t {
        const auto tls_size = size_t(__tls_end - __tls_start), alignment = reinterpret_cast<uintptr_t>(__tls_align);
        const auto guard = (std::max(size_t{4096}, attr.guard_size) + 4095) & ~size_t{4095};
        if (tls_size > SIZE_MAX - attr.stack_size - alignment - 4096) return STATUS_BAD_ARGS;
        const auto bytes = (attr.stack_size + tls_size + alignment + 4095) & ~size_t{4095};
        if (guard > SIZE_MAX - bytes) return STATUS_BAD_ARGS;
        auto status = storage.stack.open(_vm(), 0, guard + bytes);
        if (status != STATUS_OK) return status;
        status = storage.stack.guard(guard);
        if (status != STATUS_OK) return status;
        const auto memory = _memory_create(env.alloc(), bytes, VM_READ | VM_WRITE);
        storage.stack.memory = _cap_owner{memory.value};
        if (memory.status != STATUS_OK) return memory.status;
        status = storage.stack.attach(memory.value, 0, bytes, VM_READ | VM_WRITE, guard);
        if (status != STATUS_OK) return status;
        const auto tp = (storage.stack.address + storage.stack.size - tls_size) & ~(alignment - 1);
        ::memcpy(reinterpret_cast<void*>(tp), __tls_start, __tls_data_end - __tls_start);
        // Fresh anonymous backing supplies TLS BSS without eagerly touching it.
        auto ipc = _vm_area::create(env.alloc(), _vm(), 0, 4096);
        if (!ipc) return ipc.error();
        storage.ipc = std::move(*ipc);
        const auto tls_offset = reinterpret_cast<uintptr_t>(&_local) - reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
        storage.context = reinterpret_cast<_context*>(tp + tls_offset);
        const auto park = _notification_create(env.alloc(), 1);
        if (park.status != STATUS_OK) return park.status;
        storage.context->park = park.value;
        status = _memory_populate(storage.ipc.memory.selector(), 0).status;
        if (status != STATUS_OK) return status;
        const auto desc = _memory_create(env.alloc(), 4096, VM_READ | VM_WRITE); descriptor = _cap_owner{desc.value};
        if (desc.status != STATUS_OK) return desc.status;
        status = _domain_info(env.domain).status;
        if (status != STATUS_OK) return status;
        const auto cpus = reinterpret_cast<const DomainInfo*>(_ipc())->cpus;
        ThreadInit init{.version=THREAD_START_VERSION, .flags=0,
            .entry=reinterpret_cast<word_t>(&_thread_start), .stack=(tp - 16) & ~word_t{15},
            .arguments={reinterpret_cast<word_t>(record), tp},
            .ipc={.memory=storage.ipc.memory.selector(), .page=0, .address=storage.ipc.address, .pages=1}};
        status = _mem_write(descriptor.selector(), 0, std::as_bytes(std::span{&init, 1})).status;
        if (status != STATUS_OK) return status;
        auto home = cpus;
        for (auto skip = (record->id - 1) % std::popcount(cpus); skip; --skip) home &= home - 1;
        const auto made = _thread_create(env.alloc(), env.vspace, env.cspace, descriptor.selector(), 0, std::countr_zero(home));
        storage.thread = _cap_owner{made.value}; _destroy(descriptor);
        if (made.status != STATUS_OK) return made.status;
        const auto sc = _sc_create(env.alloc(), env.domain, 0, 0, 1024, cpus); storage.sc = _cap_owner{sc.value};
        if (sc.status != STATUS_OK) return sc.status;
        return _sc_bind(storage.sc.selector(), storage.thread.selector()).status;
    };
    auto status = build();
    _destroy(descriptor);
    if (status == STATUS_OK) {
        std::lock_guard lock{registry_mutex};
        if (exit_owner.load(std::memory_order_acquire)) status = STATUS_CANCELED;
        else { record->next = records; records = record; if (!internal) ++users; }
    }
    if (status != STATUS_OK) {
        if (record->storage->thread) _stop(record->storage->thread.selector());
        _reclaim(*record);
        delete record; return std::unexpected(status);
    }
    return record;
}
static void _enqueue(_record& record) {
    record.pending_next = nullptr;
    *pending_tail = &record; pending_tail = &record.pending_next;
}
static auto _reaper(void*) -> void* {
    auto& notice = *reaper_notice;
    __atomic_store_n(&reaper_ready, 1, __ATOMIC_RELEASE); _wake(&reaper_ready, true);
    for (;;) {
        (void)notice.take();
        // Only threads which finished TLS cleanup enter this queue. At most
        // one bounded batch owns stopped watches; live threads need no watch.
        std::array<_record*, 63> batch{};
        for (;;) {
            size_t count{};
            {
                std::lock_guard lock{registry_mutex};
                while (pending && count < batch.size()) {
                    auto* record = pending; pending = record->pending_next;
                    if (!pending) pending_tail = &pending;
                    batch[count++] = record;
                }
            }
            if (!count) break;
            size_t left = count;
            while (left) {
                for (size_t i = 0; i < count; ++i) {
                    auto* record = batch[i];
                    if (!record) continue;
                    auto& watch = record->storage->stopped;
                    if (watch && !watch.pending()) continue;
                    const auto facts = _source_info(record->storage->thread.selector());
                    if (facts.status != STATUS_OK) _thread_exit(facts.status);
                    if (!(facts.value & THREAD_STOPPED)) {
                        const auto status = watch ? watch.arm(facts.value2).status
                            : watch.open(reaper_events, record->storage->thread.selector(), THREAD_STOPPED);
                        if (status != STATUS_OK) _thread_exit(status);
                        continue;
                    }
                    _reclaim(*record); batch[i] = nullptr; --left;
                    bool remove, last;
                    {
                        std::lock_guard lock{registry_mutex};
                        last = !--users;
                        if (last) { _thread none{}; (void)exit_owner.compare_exchange_strong(none, _thread_self(), std::memory_order_acq_rel); }
                        __atomic_store_n(&record->reaped, 1, __ATOMIC_RELEASE);
                        remove = record->detached;
                        if (remove) _unlink(record);
                    }
                    _wake(&record->reaped, true);
                    if (remove) _release(record);
                    _release(record);
                    if (last) _exit(0);
                }
                if (left) {
                    if (const auto status = notice.arm(); status != STATUS_OK) _thread_exit(status);
                    if (const auto status = _ev_wait(reaper_events).status; status != STATUS_OK) _thread_exit(status);
                    (void)notice.take();
                }
            }
        }
        if (const auto status = notice.arm(); status != STATUS_OK) _thread_exit(status);
        const auto status = _ev_wait(&_events()).status;
        if (status != STATUS_OK) _thread_exit(status);
    }
}
static auto _make_reaper() -> status_t {
    std::lock_guard lock{reaper_mutex};
    if (__atomic_load_n(&reaper_ready, __ATOMIC_ACQUIRE)) return STATUS_OK;
    auto prepared = _prepare(_reaper, nullptr, {}, true);
    if (!prepared) return prepared.error();
    auto& record = **prepared;
    auto* notice = new (std::nothrow) _ev_notice;
    auto status = notice ? STATUS_OK : STATUS_NO_MEMORY;
    if (status == STATUS_OK) {
        auto* port = new (std::nothrow) _ev_port;
        record.storage->context->events = port;
        reaper_events = port;
        status = port ? port->open(_startup().alloc(), 64, 16) : STATUS_NO_MEMORY;
        if (status == STATUS_OK) status = notice->open(_startup().alloc(), port);
    }
    if (status != STATUS_OK) {
        { std::lock_guard registry{registry_mutex}; _unlink(&record); }
        _stop(record.storage->thread.selector());
        delete notice;
        reaper_events = nullptr; _reclaim(record);
        delete &record; return status;
    }
    reaper_notice = notice; // Execution owns this one lazily allocated doorbell.
    record.published.store(true, std::memory_order_release);
    const auto enabled = _execution_set(record.storage->thread.selector(), true).status;
    if (enabled != STATUS_OK) _thread_exit(enabled);
    _release(&record);
    while (!__atomic_load_n(&reaper_ready, __ATOMIC_ACQUIRE)) (void)_wait32(&reaper_ready, 0);
    return STATUS_OK;
}
void _thread_init() {
    if (!_initial().thread) return;
    const auto& initial = _initial();
    auto* record = new _record;
    record->storage = std::make_unique<_storage>();
    auto& storage = *record->storage;
    record->refs.store(1, std::memory_order_relaxed);
    record->id = 1; storage.context = &_current();
    storage.thread = _cap_owner{initial.thread}; storage.sc = _cap_owner{initial.sc};
    storage.stack = _vm_area::bootstrap(_cap_owner{initial.stack_region}, _cap_owner{initial.stack_memory},
        initial.stack, initial.stack_size, initial.guard_size);
    storage.ipc = _vm_area::bootstrap(_cap_owner{initial.ipc_region}, _cap_owner{initial.ipc_memory}, _ipc(), 4096);
    records = record; users = 1; _current().thread = record;
}
extern "C" [[noreturn]] void _thread_boot(_record* record) {
    if (!record->published.load(std::memory_order_acquire)) _thread_exit(STATUS_INTERNAL);
    auto& current = _current();
    current.ipc = record->storage->ipc.address; current.tid = record->id; current.thread = record; record->storage->context = &current;
    // Exit may win after creation's last check but before this first dispatch.
    auto* result = !record->internal && exit_owner.load(std::memory_order_acquire)
        ? nullptr : record->entry(record->argument);
    if (record->internal) _thread_exit(STATUS_INTERNAL);
    _thread_exit(result);
}
auto _thread_self() noexcept -> _thread { return _current().tid; }
auto _thread_create(_entry entry, void* argument, const _thread_attr& attr) -> std::expected<_thread, status_t> {
    if (!entry || !attr.stack_size || attr.stack_size > SIZE_MAX / 2 || attr.guard_size > SIZE_MAX / 2) return std::unexpected(STATUS_BAD_ARGS);
    if (!_current().thread || exit_owner.load(std::memory_order_acquire)) return std::unexpected(STATUS_DENIED);
    if (!_current().park) {
        const auto park = _notification_create(_startup().alloc(), 1);
        if (park.status != STATUS_OK) return std::unexpected(park.status);
        _current().park = park.value;
    }
    if (const auto status = _make_reaper(); status != STATUS_OK) return std::unexpected(status);
    auto prepared = _prepare(entry, argument, attr, false);
    if (!prepared) return std::unexpected(prepared.error());
    auto* record = *prepared;
    const auto id = record->id;
    record->published.store(true, std::memory_order_release);
    const auto status = _execution_set(record->storage->thread.selector(), true).status;
    if (status != STATUS_OK) {
        // The creator still owns all heavy resources; no queue sees this record.
        _stop(record->storage->thread.selector()); _reclaim(*record);
        { std::lock_guard lock{registry_mutex}; _unlink(record); --users; }
        _release(record); _release(record); return std::unexpected(status);
    }
    _release(record);
    return id;
}
auto _thread_join(_thread id) -> std::expected<void*, status_t> {
    if (id == _thread_self()) return std::unexpected(STATUS_DEADLOCK);
    _record* record;
    {
        std::lock_guard lock{registry_mutex}; record = _find(id);
        if (!record || record->internal) return std::unexpected(STATUS_NOT_FOUND);
        if (record->detached || record->joining) return std::unexpected(STATUS_BAD_ARGS);
        record->joining = true; record->refs.fetch_add(1, std::memory_order_relaxed);
    }
    while (!__atomic_load_n(&record->reaped, __ATOMIC_ACQUIRE)) (void)_wait32(&record->reaped, 0);
    auto* result = record->result;
    { std::lock_guard lock{registry_mutex}; _unlink(record); }
    _release(record); _release(record); return result;
}
auto _thread_detach(_thread id) -> status_t {
    _record* record; bool remove{};
    {
        std::lock_guard lock{registry_mutex}; record = _find(id);
        if (!record || record->internal) return STATUS_NOT_FOUND;
        if (record->detached || record->joining) return STATUS_BAD_ARGS;
        record->detached = true;
        if (__atomic_load_n(&record->reaped, __ATOMIC_ACQUIRE)) { _unlink(record); remove = true; }
    }
    if (remove) _release(record);
    return STATUS_OK;
}
extern "C" int __cxxabiv1::__cxa_thread_atexit(void (*function)(void*), void* argument, void*) noexcept {
    auto* entry = new _cleanup{function, argument, dtors}; dtors = entry; return 0;
}
void _cleanup_push(_cleanup* node, void (*function)(void*), void* argument) {
    *node = {function, argument, cleanups}; cleanups = node;
}
void _cleanup_pop(_cleanup* node, bool execute) {
    if (cleanups != node) _thread_exit(STATUS_INTERNAL);
    cleanups = node->next; if (execute) node->function(node->argument);
}
void _cleanup_defer(_cleanup* node, void (*function)(void*), void* argument) {
    *node = {function, argument, deferred}; deferred = node;
}
void _thread_fini() {
    while (cleanups) _cleanup_pop(cleanups, true);
    while (dtors) { auto* entry = dtors; dtors = entry->next; entry->function(entry->argument); delete entry; }
    for (unsigned pass = 0; pass < 4; ++pass) {
        bool called{};
        for (auto* entry = values; entry; entry = entry->next) if (entry->value) {
            void (*destroy)(void*){};
            { std::lock_guard lock{key_mutex}; for (auto* key = keys; key; key = key->next) if (key->id == entry->key) { destroy = key->destroy; break; } }
            if (destroy) { auto* value = std::exchange(entry->value, nullptr); destroy(value); called = true; }
        }
        if (!called) break;
    }
    while (values) { auto* entry = values; values = entry->next; delete entry; }
    while (deferred) { auto* entry = deferred; deferred = entry->next; entry->function(entry->argument); }
}
[[noreturn]] void _thread_exit(void* result) {
    auto* record = static_cast<_record*>(_current().thread);
    if (!record) _exit(0);
    _thread_fini(); record->result = result;
    if (!__atomic_load_n(&reaper_ready, __ATOMIC_ACQUIRE)) _exit(0);
    { std::lock_guard lock{registry_mutex}; record->refs.fetch_add(1, std::memory_order_relaxed); _enqueue(*record); }
    _signal_reaper();
    _thread_exit(STATUS_OK);
}
[[noreturn]] void _exit(int status) {
    _thread expected{};
    if (!exit_owner.compare_exchange_strong(expected, _thread_self(), std::memory_order_acq_rel)
        && expected != _thread_self()) {
        for (;;) {
            if (_startup().lifecycle) (void)_wait_one(_startup().lifecycle, SOURCE_TERMINAL, 0, 0, 0, SOURCE_WRITE);
            else _yield();
        }
    }
    _thread_fini(); _finalize(); _thread_fini();
    if (_startup().lifecycle) {
        const word_t message[]{PROGRAM_EXIT_VERSION, word_t(status)};
        const auto sent = _ch_send(_startup().lifecycle, message).status;
        if (sent != STATUS_OK) _thread_exit(sent);
        _thread_exit(STATUS_OK);
    }
    _thread_exit(status);
}
auto _key_create(unsigned* id, void (*destroy)(void*)) -> int {
    auto* key = new (std::nothrow) _key{}; if (!key) return ENOMEM;
    { std::lock_guard lock{key_mutex}; if (next_key == UINT32_MAX) { delete key; return EAGAIN; }
      key->id = ++next_key; key->destroy = destroy; key->next = keys; keys = key; *id = key->id; }
    return 0;
}
auto _key_delete(unsigned id) -> int {
    _key* key;
    { std::lock_guard lock{key_mutex}; auto** link = &keys; while (*link && (*link)->id != id) link = &(*link)->next;
      if (!*link) return EINVAL;
      key = *link; *link = key->next; }
    delete key; return 0;
}
auto _key_get(unsigned id) -> void* {
    for (auto* entry = values; entry; entry = entry->next) if (entry->key == id) return entry->value;
    return nullptr;
}
auto _key_set(unsigned id, const void* value) -> int {
    { std::lock_guard lock{key_mutex}; bool valid{}; for (auto* key = keys; key; key = key->next) if (key->id == id) { valid = true; break; }
      if (!valid) return EINVAL; }
    for (auto* entry = values; entry; entry = entry->next) if (entry->key == id) { entry->value = const_cast<void*>(value); return 0; }
    auto* entry = new (std::nothrow) _value{id, const_cast<void*>(value), values};
    if (!entry) return ENOMEM;
    values = entry; return 0;
}

auto _errno(status_t status) noexcept -> int {
    switch (status) {
    case STATUS_OK: return 0;
    case STATUS_NO_MEMORY: return ENOMEM;
    case STATUS_EXHAUSTED: return EAGAIN;
    case STATUS_BUSY: return EBUSY;
    case STATUS_DENIED: case STATUS_BAD_RIGHTS: return EPERM;
    case STATUS_DEADLOCK: return EDEADLK;
    case STATUS_NOT_FOUND: return ESRCH;
    case STATUS_TIMED_OUT: return ETIMEDOUT;
    default: return EINVAL;
    }
}
int _gthread_create(uint64_t* id, _entry entry, void* argument) {
    const auto made = _thread_create(entry, argument);
    if (!made) return _errno(made.error());
    *id = *made; return 0;
}
int _gthread_join(uint64_t id, void** value) {
    const auto result = _thread_join(id);
    if (!result) return _errno(result.error());
    if (value) *value = *result;
    return 0;
}
int _gthread_detach(uint64_t id) { return _errno(_thread_detach(id)); }
void _thread_yield() noexcept { _yield(); }
unsigned _cpu_count() noexcept { return _startup().cpus; }
auto _time_scale(unsigned __int128 value, uint64_t numerator, uint64_t denominator, bool ceil) noexcept -> std::optional<uint64_t> {
    if (!denominator || (numerator && value > ~static_cast<unsigned __int128>(0) / numerator)) return std::nullopt;
    const auto product = value * numerator;
    const auto result = product / denominator + (ceil && product % denominator != 0);
    return result <= UINT64_MAX ? std::optional{uint64_t(result)} : std::nullopt;
}
uint64_t _clock_ns() noexcept {
    return std::min(_time_scale(_clock_now().value, 1000000000, _startup().frequency).value_or(UINT64_MAX), uint64_t(INT64_MAX));
}
void _sleep_ns(uint64_t ns) {
    if (!ns) return;
    const auto ticks = _time_scale(ns, _startup().frequency, 1000000000, true).value_or(UINT64_MAX);
    const auto now = _clock_now().value;
    const auto deadline = ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
    uint32_t unchanged{};
    while (_clock_now().value < deadline) (void)_wait32(&unchanged, 0, deadline);
}
auto std::chrono::steady_clock::now() noexcept -> time_point { return time_point{duration{_clock_ns()}}; }
auto std::chrono::system_clock::now() noexcept -> time_point { _thread_exit(STATUS_INVALID_OP); }
[[noreturn]] void std::terminate() noexcept { _thread_exit(STATUS_INTERNAL); }
[[noreturn]] void std::__throw_system_error(int) { _thread_exit(STATUS_INTERNAL); }
