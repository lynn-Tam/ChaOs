#include <thread.hpp>
#include <vm.hpp>
#include <rpc.hpp>
#include <mutex>
#include <atomic>
#include <bit>
#include <cerrno>
#include <memory>
#include <cxxabi.h>
#include <chrono>
#include <exception>

extern "C" void rt_thread_start();
extern "C" const std::byte __tls_start[], __tls_data_end[], __tls_end[], __tls_align[];
struct rt_value { unsigned key; void* value; rt_value* next; };
struct rt_key { unsigned id; void (*destroy)(void*); rt_key* next; };
struct rt_storage {
    rt_context* context{};
    vm_area stack, ipc;
    cap_owner thread, sc;
    ev_watch stopped;
};
struct rt_record {
    rt_record* next{};
    std::atomic<unsigned> refs{2};
    rt_thread id{};
    rt_entry entry{};
    void* argument{};
    std::unique_ptr<rt_storage> storage;
    uint32_t watched{}, reaped{};
    status_t watch_status{};
    void* result{};
    bool internal{}, detached{}, joining{};
    std::atomic<bool> canceled{};
    std::atomic<bool> finished{}, published{};
};
static constinit rt_mutex registry_mutex, key_mutex;
static rt_record* records;
static rt_key* keys;
static unsigned next_key;
static uint64_t next_id{1};
static size_t users;
static constinit rt_mutex reaper_mutex;
static ev_notice* reaper_notice;
static uint32_t reaper_ready;
static std::atomic<rt_thread> exit_owner;
static thread_local rt_cleanup* dtors;
static thread_local rt_cleanup* cleanups;
static thread_local rt_cleanup* deferred;
static thread_local rt_value* values;

static void rt_release(rt_record* record) {
    if (record->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete record;
}
static void rt_unlink(rt_record* record) {
    auto** link = &records;
    while (*link != record) link = &(*link)->next;
    *link = record->next;
}
static auto rt_find(rt_thread id) -> rt_record* {
    for (auto* record = records; record; record = record->next) if (record->id == id) return record;
    return nullptr;
}
static void rt_destroy(cap_owner& cap) {
    if (!cap) return;
    const auto status = sys::object_destroy(cap.selector()).status;
    if (status != STATUS_OK) sys::exit(status);
    cap = {};
}
static void rt_stop(cap_t thread) {
    const auto stopped = sys::execution_stop(thread).status;
    if (stopped != STATUS_OK) sys::exit(stopped);
    const auto waited = sys::wait_one(thread, THREAD_STOPPED).status;
    if (waited != STATUS_OK) sys::exit(waited);
}
static void rt_unmap(vm_area& area) {
    auto memory = std::move(area.memory);
    const auto status = area.close();
    if (status != STATUS_OK) sys::exit(status);
    rt_destroy(memory);
}
static auto rt_register(rt_record&, word_t) -> status_t;
static void rt_reclaim(rt_record& record) {
    if (!record.canceled.load(std::memory_order_acquire)) {
        // Destroy invalidates external Observe grants: retire their observer first.
        const auto acknowledged = rt_register(record, rt_life_reap);
        if (acknowledged != STATUS_OK) sys::exit(acknowledged);
    }
    auto& storage = *record.storage;
    const auto status = storage.stopped.close(); if (status != STATUS_OK) sys::exit(status);
    if (storage.context) for (auto selector : {storage.context->events, storage.context->park}) if (selector) {
        cap_owner cap{selector}; rt_destroy(cap);
    }
    rt_unmap(storage.stack); rt_unmap(storage.ipc);
    rt_destroy(storage.sc); rt_destroy(storage.thread);
    record.storage.reset();
}
static auto rt_register(rt_record& record, word_t operation = rt_life_register) -> status_t {
    const word_t words[]{rt_life_version, operation, record.internal};
    const CapXfer cap{record.storage->thread.selector(), RIGHT_OBSERVE | RIGHT_INSPECT, CAP_COPY, 0};
    rpc_op call;
    auto status = call.begin(rt_startup().lifecycle, rt_startup().alloc(), 0, words, std::span{&cap, 1});
    if (status != STATUS_OK) return status;
    ch_packet reply;
    while ((status = call.poll(reply)) == STATUS_WOULD_BLOCK) {
        const auto waited = call.wait(); if (waited != STATUS_OK) return waited;
    }
    return status != STATUS_OK ? status : reply.word_count == 1 ? static_cast<status_t>(reply.words[0]) : STATUS_PEER_FAULT;
}
static void rt_signal_reaper() {
    if (reaper_notice) {
        const auto status = sys::notification_signal(reaper_notice->selector()).status;
        if (status != STATUS_OK) sys::exit(status);
    }
}
static auto rt_prepare(rt_entry entry, void* argument, const rt_thread_attr& attr, bool internal) -> std::expected<rt_record*, status_t> {
    const auto& env = rt_startup();
    rt_thread id;
    { std::lock_guard lock{registry_mutex};
      if (next_id == UINT64_MAX) return std::unexpected(STATUS_EXHAUSTED);
      id = ++next_id; }
    auto* record = new (std::nothrow) rt_record;
    if (!record) return std::unexpected(STATUS_NO_MEMORY);
    record->storage.reset(new (std::nothrow) rt_storage);
    if (!record->storage) { delete record; return std::unexpected(STATUS_NO_MEMORY); }
    record->id = id;
    cap_owner descriptor;
    auto& storage = *record->storage;
    record->entry = entry; record->argument = argument; record->internal = internal; record->detached = attr.detached;
    const auto build = [&]() -> status_t {
        const auto tls_size = size_t(__tls_end - __tls_start), alignment = reinterpret_cast<uintptr_t>(__tls_align);
        const auto guard = (std::max(size_t{4096}, attr.guard_size) + 4095) & ~size_t{4095};
        if (tls_size > SIZE_MAX - attr.stack_size - alignment - 4096) return STATUS_BAD_ARGS;
        const auto bytes = (attr.stack_size + tls_size + alignment + 4095) & ~size_t{4095};
        if (guard > SIZE_MAX - bytes) return STATUS_BAD_ARGS;
        auto status = storage.stack.open(rt_vm(), 0, guard + bytes);
        if (status != STATUS_OK) return status;
        const auto memory = sys::memory_create(env.alloc(), bytes, VM_READ | VM_WRITE);
        storage.stack.memory = cap_owner{memory.value};
        if (memory.status != STATUS_OK) return memory.status;
        status = storage.stack.attach(memory.value, 0, bytes, VM_READ | VM_WRITE, guard);
        if (status != STATUS_OK) return status;
        const auto tp = (storage.stack.address + storage.stack.size - tls_size) & ~(alignment - 1);
        ::memcpy(reinterpret_cast<void*>(tp), __tls_start, __tls_data_end - __tls_start);
        // Fresh anonymous backing supplies TLS BSS without eagerly touching it.
        auto ipc = vm_area::create(env.alloc(), rt_vm(), 0, 4096);
        if (!ipc) return ipc.error();
        storage.ipc = std::move(*ipc);
        const auto tls_offset = reinterpret_cast<uintptr_t>(&rt_local) - reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
        storage.context = reinterpret_cast<rt_context*>(tp + tls_offset);
        const auto park = sys::notification_create(env.alloc(), 1);
        if (park.status != STATUS_OK) return park.status;
        storage.context->park = park.value;
        status = sys::memory_populate(storage.ipc.memory.selector(), 0).status;
        if (status != STATUS_OK) return status;
        const auto desc = sys::memory_create(env.alloc(), 4096, VM_READ | VM_WRITE); descriptor = cap_owner{desc.value};
        if (desc.status != STATUS_OK) return desc.status;
        status = sys::domain_info(env.domain).status;
        if (status != STATUS_OK) return status;
        const auto cpus = reinterpret_cast<const DomainInfo*>(rt_current().ipc)->cpus;
        ThreadInit init{.version=THREAD_START_VERSION, .flags=0,
            .entry=reinterpret_cast<word_t>(&rt_thread_start), .stack=(tp - 16) & ~word_t{15},
            .arguments={reinterpret_cast<word_t>(record), tp},
            .ipc={.memory=storage.ipc.memory.selector(), .page=0, .address=storage.ipc.address, .pages=1}};
        status = mem_write(descriptor.selector(), 0, std::as_bytes(std::span{&init, 1}));
        if (status != STATUS_OK) return status;
        auto home = cpus;
        for (auto skip = (record->id - 1) % std::popcount(cpus); skip; --skip) home &= home - 1;
        const auto made = sys::thread_create(env.alloc(), env.vspace, env.cspace, descriptor.selector(), 0, std::countr_zero(home));
        storage.thread = cap_owner{made.value}; rt_destroy(descriptor);
        if (made.status != STATUS_OK) return made.status;
        const auto sc = sys::sc_create(env.alloc(), env.domain, 0, 0, 1024, cpus); storage.sc = cap_owner{sc.value};
        if (sc.status != STATUS_OK) return sc.status;
        return sys::sc_bind(storage.sc.selector(), storage.thread.selector()).status;
    };
    auto status = build();
    rt_destroy(descriptor);
    if (status == STATUS_OK) {
        std::lock_guard lock{registry_mutex};
        if (exit_owner.load(std::memory_order_acquire)) status = STATUS_CANCELED;
        else { record->next = records; records = record; if (!internal) ++users; }
    }
    if (status != STATUS_OK) {
        if (record->storage->thread) rt_stop(record->storage->thread.selector());
        record->canceled.store(true, std::memory_order_relaxed); rt_reclaim(*record);
        delete record; return std::unexpected(status);
    }
    return record;
}
static auto rt_reaper(void*) -> void* {
    auto& notice = *reaper_notice;
    __atomic_store_n(&reaper_ready, 1, __ATOMIC_RELEASE); rt_wake(&reaper_ready, true);
    for (;;) {
        (void)notice.take();
        uint64_t cursor{};
        for (;;) {
            rt_record* record{};
            {
                std::lock_guard lock{registry_mutex};
                for (auto* item = records; item; item = item->next)
                    if (!item->internal && item->storage && item->id > cursor && (!record || item->id < record->id)) record = item;
                if (record) record->refs.fetch_add(1, std::memory_order_relaxed);
            }
            if (!record) break;
            cursor = record->id;
            if (!__atomic_load_n(&record->watched, __ATOMIC_ACQUIRE)) {
                record->watch_status = record->storage->stopped.open(rt_events(), record->storage->thread.selector(), THREAD_STOPPED);
                __atomic_store_n(&record->watched, 1, __ATOMIC_RELEASE); rt_wake(&record->watched, true);
            }
            if ((record->watch_status == STATUS_OK || record->canceled.load(std::memory_order_acquire)) && !__atomic_load_n(&record->reaped, __ATOMIC_ACQUIRE)) {
                const auto facts = sys::source_info(record->storage->thread.selector());
                if (facts.status != STATUS_OK) sys::exit(facts.status);
                if (facts.value & THREAD_STOPPED) {
                    if (!record->finished.load(std::memory_order_acquire) && !record->canceled.load(std::memory_order_acquire)) sys::exit(STATUS_INTERNAL);
                    rt_reclaim(*record);
                    bool remove{}, last{};
                    {
                        std::lock_guard lock{registry_mutex};
                        last = !--users;
                        if (last) { rt_thread none{}; (void)exit_owner.compare_exchange_strong(none, rt_thread_self(), std::memory_order_acq_rel); }
                        __atomic_store_n(&record->reaped, 1, __ATOMIC_RELEASE);
                        if (record->detached) { rt_unlink(record); remove = true; }
                    }
                    rt_wake(&record->reaped, true);
                    if (remove) rt_release(record);
                    rt_release(record);
                    if (last) rt_exit(0);
                    continue;
                }
                if (const auto status = record->storage->stopped ? record->storage->stopped.arm().status : STATUS_OK; status != STATUS_OK) sys::exit(status);
            }
            rt_release(record);
        }
        if (const auto status = notice.arm(); status != STATUS_OK) sys::exit(status);
        const auto status = ev_wait(rt_events()).status;
        if (status != STATUS_OK) sys::exit(status);
    }
}
static auto rt_make_reaper() -> status_t {
    std::lock_guard lock{reaper_mutex};
    if (__atomic_load_n(&reaper_ready, __ATOMIC_ACQUIRE)) return STATUS_OK;
    auto prepared = rt_prepare(rt_reaper, nullptr, {}, true);
    if (!prepared) return prepared.error();
    auto& record = **prepared;
    auto* notice = new (std::nothrow) ev_notice;
    auto status = notice ? STATUS_OK : STATUS_NO_MEMORY;
    if (status == STATUS_OK) {
        auto port = sys::port_create(rt_startup().alloc(), 64, 16);
        record.storage->context->events = port.value;
        if (port.status == STATUS_PENDING) port.status = op_wait(port.value).status;
        status = port.status;
        if (status == STATUS_OK) status = notice->open(rt_startup().alloc(), port.value);
    }
    if (status == STATUS_OK) status = rt_register(record);
    if (status != STATUS_OK) {
        { std::lock_guard registry{registry_mutex}; rt_unlink(&record); }
        rt_stop(record.storage->thread.selector());
        delete notice;
        record.canceled.store(true, std::memory_order_relaxed); rt_reclaim(record);
        delete &record; return status;
    }
    reaper_notice = notice; // Execution owns this one lazily allocated doorbell.
    record.published.store(true, std::memory_order_release);
    const auto enabled = sys::execution_set(record.storage->thread.selector(), true).status;
    if (enabled != STATUS_OK) sys::exit(enabled);
    rt_release(&record);
    while (!__atomic_load_n(&reaper_ready, __ATOMIC_ACQUIRE)) (void)rt_wait32(&reaper_ready, 0);
    return STATUS_OK;
}
void rt_thread_init() {
    if (!rt_startup().lifecycle) return;
    const auto& initial = rt_initial();
    auto* record = new rt_record;
    record->storage = std::make_unique<rt_storage>();
    auto& storage = *record->storage;
    record->refs.store(1, std::memory_order_relaxed);
    record->id = 1; storage.context = &rt_current();
    storage.thread = cap_owner{initial.thread}; storage.sc = cap_owner{initial.sc};
    auto stack = vm_area::adopt(rt_vm(), cap_owner{initial.stack_memory}, initial.stack, initial.stack_size);
    if (!stack) sys::exit(stack.error());
    storage.stack = std::move(*stack);
    auto ipc = vm_area::adopt(rt_vm(), cap_owner{initial.ipc_memory}, rt_current().ipc, 4096);
    if (!ipc) sys::exit(ipc.error());
    storage.ipc = std::move(*ipc);
    records = record; users = 1; rt_current().thread = record;
}
extern "C" [[noreturn]] void rt_thread_boot(rt_record* record) {
    if (!record->published.load(std::memory_order_acquire)) sys::exit(STATUS_INTERNAL);
    auto& current = rt_current();
    current.ipc = record->storage->ipc.address; current.tid = record->id; current.thread = record; record->storage->context = &current;
    // Exit may win after creation's last check but before this first dispatch.
    rt_thread_exit(!record->internal && exit_owner.load(std::memory_order_acquire)
        ? nullptr : record->entry(record->argument));
}
auto rt_thread_self() noexcept -> rt_thread { return rt_current().tid; }
auto rt_thread_create(rt_entry entry, void* argument, const rt_thread_attr& attr) -> std::expected<rt_thread, status_t> {
    if (!entry || !attr.stack_size || attr.stack_size > SIZE_MAX / 2 || attr.guard_size > SIZE_MAX / 2) return std::unexpected(STATUS_BAD_ARGS);
    if (!rt_startup().lifecycle || exit_owner.load(std::memory_order_acquire)) return std::unexpected(STATUS_DENIED);
    if (!rt_current().park) {
        const auto park = sys::notification_create(rt_startup().alloc(), 1);
        if (park.status != STATUS_OK) return std::unexpected(park.status);
        rt_current().park = park.value;
    }
    if (const auto status = rt_make_reaper(); status != STATUS_OK) return std::unexpected(status);
    auto prepared = rt_prepare(entry, argument, attr, false);
    if (!prepared) return std::unexpected(prepared.error());
    auto* record = *prepared;
    rt_signal_reaper();
    while (!__atomic_load_n(&record->watched, __ATOMIC_ACQUIRE)) (void)rt_wait32(&record->watched, 0);
    auto status = record->watch_status;
    if (status == STATUS_OK) status = rt_register(*record);
    if (status != STATUS_OK) {
        record->canceled.store(true, std::memory_order_release);
        { std::lock_guard lock{registry_mutex}; record->detached = false; }
        rt_stop(record->storage->thread.selector());
        rt_signal_reaper();
        while (!__atomic_load_n(&record->reaped, __ATOMIC_ACQUIRE)) (void)rt_wait32(&record->reaped, 0);
        { std::lock_guard lock{registry_mutex}; rt_unlink(record); }
        rt_release(record); rt_release(record); return std::unexpected(status);
    }
    const auto id = record->id;
    if (exit_owner.load(std::memory_order_acquire)) rt_exit(0);
    record->published.store(true, std::memory_order_release);
    status = sys::execution_set(record->storage->thread.selector(), true).status;
    if (status != STATUS_OK) sys::exit(status);
    rt_release(record);
    return id;
}
auto rt_thread_join(rt_thread id) -> std::expected<void*, status_t> {
    if (id == rt_thread_self()) return std::unexpected(STATUS_DEADLOCK);
    rt_record* record;
    {
        std::lock_guard lock{registry_mutex}; record = rt_find(id);
        if (!record || record->internal) return std::unexpected(STATUS_NOT_FOUND);
        if (record->detached || record->joining) return std::unexpected(STATUS_BAD_ARGS);
        record->joining = true; record->refs.fetch_add(1, std::memory_order_relaxed);
    }
    while (!__atomic_load_n(&record->reaped, __ATOMIC_ACQUIRE)) (void)rt_wait32(&record->reaped, 0);
    auto* result = record->result;
    { std::lock_guard lock{registry_mutex}; rt_unlink(record); }
    rt_release(record); rt_release(record); return result;
}
auto rt_thread_detach(rt_thread id) -> status_t {
    rt_record* record; bool remove{};
    {
        std::lock_guard lock{registry_mutex}; record = rt_find(id);
        if (!record || record->internal) return STATUS_NOT_FOUND;
        if (record->detached || record->joining) return STATUS_BAD_ARGS;
        record->detached = true;
        if (__atomic_load_n(&record->reaped, __ATOMIC_ACQUIRE)) { rt_unlink(record); remove = true; }
    }
    if (remove) rt_release(record);
    return STATUS_OK;
}
extern "C" int __cxxabiv1::__cxa_thread_atexit(void (*function)(void*), void* argument, void*) noexcept {
    auto* entry = new rt_cleanup{function, argument, dtors}; dtors = entry; return 0;
}
void rt_cleanup_push(rt_cleanup* node, void (*function)(void*), void* argument) {
    *node = {function, argument, cleanups}; cleanups = node;
}
void rt_cleanup_pop(rt_cleanup* node, bool execute) {
    if (cleanups != node) sys::exit(STATUS_INTERNAL);
    cleanups = node->next; if (execute) node->function(node->argument);
}
void rt_cleanup_defer(rt_cleanup* node, void (*function)(void*), void* argument) {
    *node = {function, argument, deferred}; deferred = node;
}
void rt_thread_fini() {
    while (cleanups) rt_cleanup_pop(cleanups, true);
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
[[noreturn]] void rt_thread_exit(void* result) {
    auto* record = static_cast<rt_record*>(rt_current().thread);
    if (!record) rt_exit(0);
    rt_thread_fini(); record->result = result; record->finished.store(true, std::memory_order_release);
    if (!__atomic_load_n(&reaper_ready, __ATOMIC_ACQUIRE)) rt_exit(0);
    sys::exit(STATUS_OK);
}
[[noreturn]] void rt_exit(int status) {
    rt_thread expected{};
    if (!exit_owner.compare_exchange_strong(expected, rt_thread_self(), std::memory_order_acq_rel)
        && expected != rt_thread_self()) {
        for (;;) {
            if (rt_startup().lifecycle) (void)sys::wait_one(rt_startup().lifecycle, SOURCE_TERMINAL, 0, 0, 0, SOURCE_WRITE);
            else sys::yield();
        }
    }
    rt_thread_fini(); rt_finalize(); rt_thread_fini();
    if (rt_startup().lifecycle) {
        const word_t message[]{rt_life_version, rt_life_exit, word_t(status)};
        const auto sent = ch_send(rt_startup().lifecycle, message).status;
        if (sent != STATUS_OK) sys::exit(sent);
        sys::exit(STATUS_OK);
    }
    sys::exit(status);
}
auto rt_key_create(unsigned* id, void (*destroy)(void*)) -> int {
    auto* key = new (std::nothrow) rt_key{}; if (!key) return ENOMEM;
    { std::lock_guard lock{key_mutex}; if (next_key == UINT32_MAX) { delete key; return EAGAIN; }
      key->id = ++next_key; key->destroy = destroy; key->next = keys; keys = key; *id = key->id; }
    return 0;
}
auto rt_key_delete(unsigned id) -> int {
    rt_key* key;
    { std::lock_guard lock{key_mutex}; auto** link = &keys; while (*link && (*link)->id != id) link = &(*link)->next;
      if (!*link) return EINVAL;
      key = *link; *link = key->next; }
    delete key; return 0;
}
auto rt_key_get(unsigned id) -> void* {
    for (auto* entry = values; entry; entry = entry->next) if (entry->key == id) return entry->value;
    return nullptr;
}
auto rt_key_set(unsigned id, const void* value) -> int {
    { std::lock_guard lock{key_mutex}; bool valid{}; for (auto* key = keys; key; key = key->next) if (key->id == id) { valid = true; break; }
      if (!valid) return EINVAL; }
    for (auto* entry = values; entry; entry = entry->next) if (entry->key == id) { entry->value = const_cast<void*>(value); return 0; }
    auto* entry = new (std::nothrow) rt_value{id, const_cast<void*>(value), values};
    if (!entry) return ENOMEM;
    values = entry; return 0;
}

auto rt_errno(status_t status) noexcept -> int {
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
int rt_gthread_create(uint64_t* id, rt_entry entry, void* argument) {
    const auto made = rt_thread_create(entry, argument);
    if (!made) return rt_errno(made.error());
    *id = *made; return 0;
}
int rt_gthread_join(uint64_t id, void** value) {
    const auto result = rt_thread_join(id);
    if (!result) return rt_errno(result.error());
    if (value) *value = *result;
    return 0;
}
int rt_gthread_detach(uint64_t id) { return rt_errno(rt_thread_detach(id)); }
void rt_thread_yield() noexcept { sys::yield(); }
unsigned rt_cpu_count() noexcept { return rt_startup().cpus; }
uint64_t rt_clock_ns() noexcept {
    const auto frequency = sys::clock_frequency().value;
    const auto ns = static_cast<unsigned __int128>(sys::clock_now().value) * 1000000000 / frequency;
    return ns > INT64_MAX ? INT64_MAX : uint64_t(ns);
}
void rt_sleep_ns(uint64_t ns) {
    if (!ns) return;
    const auto now = sys::clock_now().value;
    const auto ticks = (static_cast<unsigned __int128>(ns) * sys::clock_frequency().value + 999999999) / 1000000000;
    const auto deadline = ticks > UINT64_MAX - now ? UINT64_MAX : now + uint64_t(ticks);
    uint32_t unchanged{};
    while (sys::clock_now().value < deadline) (void)rt_wait32(&unchanged, 0, deadline);
}
auto std::chrono::steady_clock::now() noexcept -> time_point { return time_point{duration{rt_clock_ns()}}; }
auto std::chrono::system_clock::now() noexcept -> time_point { sys::exit(STATUS_INVALID_OP); }
[[noreturn]] void std::terminate() noexcept { sys::exit(STATUS_INTERNAL); }
[[noreturn]] void std::__throw_system_error(int) { sys::exit(STATUS_INTERNAL); }
