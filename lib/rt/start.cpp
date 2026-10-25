#include "ctx.hpp"
#include <start.hpp>
#include <vm.hpp>
#include <ipc.hpp>
#include <thread.hpp>
#include <sys/syscall.hpp>
#include <climits>
#include <libk/assert.hpp>
#include <string.h>
#include <cxxabi.h>
#include <mutex>
#include <new>
#include <cstdlib>

static _env startup;
constinit thread_local _context _local;
static _context initial_context;
static _bootstrap initial;
auto _initial() noexcept -> const _bootstrap& { return initial; }
extern "C" const std::byte __tls_start[], __tls_data_end[], __tls_end[], __tls_align[];
static constinit _vm_space address_space;
auto _vm() noexcept -> _vm_space& { return address_space; }
extern "C" { char** environ; }
struct crt_finalizer {
    void (*function)(void*); void (*plain)(); void* argument; void* dso; crt_finalizer* next;
};
static crt_finalizer* finalizers;
static constinit _mutex finalizer_mutex;
void* __dso_handle = &__dso_handle;
static auto crt_register(void (*function)(void*), void (*plain)(), void* argument, void* dso) -> int {
    auto* entry = new (std::nothrow) crt_finalizer{function, plain, argument, dso, nullptr};
    if (!entry) return -1;
    std::lock_guard lock{finalizer_mutex}; entry->next = finalizers; finalizers = entry; return 0;
}
extern "C" int atexit(void (*function)()) noexcept { return crt_register(nullptr, function, nullptr, nullptr); }
extern "C" int __cxxabiv1::__cxa_atexit(void (*function)(void*), void* argument, void* dso) noexcept {
    return crt_register(function, nullptr, argument, dso);
}
extern "C" void __cxxabiv1::__cxa_finalize(void* dso) {
    for (;;) {
        crt_finalizer* entry;
        {
            std::lock_guard lock{finalizer_mutex}; auto** link = &finalizers;
            while (*link && dso && (*link)->dso != dso) link = &(*link)->next;
            if (!*link) return;
            entry = *link; *link = entry->next;
        }
        if (entry->function) entry->function(entry->argument); else entry->plain();
        delete entry;
    }
}
extern "C" int __cxxabiv1::__cxa_guard_acquire(__guard* guard) {
    auto* state = reinterpret_cast<uint32_t*>(guard);
    for (;;) {
        auto value = __atomic_load_n(state, __ATOMIC_ACQUIRE);
        if (value & 1) return 0;
        if (!(value & 256) && __atomic_compare_exchange_n(state, &value, 256, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
        (void)_wait32(state, value);
    }
}
extern "C" void __cxxabiv1::__cxa_guard_release(__guard* guard) noexcept {
    auto* state = reinterpret_cast<uint32_t*>(guard); __atomic_store_n(state, 1, __ATOMIC_RELEASE); _wake(state, true);
}
extern "C" void __cxxabiv1::__cxa_guard_abort(__guard* guard) noexcept {
    auto* state = reinterpret_cast<uint32_t*>(guard); __atomic_store_n(state, 0, __ATOMIC_RELEASE); _wake(state, true);
}
auto _ipc() noexcept -> word_t { return _local.ipc; }
auto _startup() noexcept -> const _env& { return startup; }
extern "C" void init_heap(CreateAuth auth, _vm_space& arena) noexcept;
extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();
extern int main(int, char**);

extern "C" auto crt_boot(word_t* stack) noexcept -> uintptr_t {
    const auto initial_sp = reinterpret_cast<uintptr_t>(stack);
    if (*stack > INT_MAX) _thread_exit(STATUS_BAD_ARGS);
    const auto argc = static_cast<int>(*stack++);
    auto** argv = reinterpret_cast<char**>(stack);
    if (argv[argc]) _thread_exit(STATUS_BAD_ARGS);
    auto** envp = argv + argc + 1;
    while (*envp) ++envp;
    const auto* aux = reinterpret_cast<const AuxEntry*>(envp + 1);
    const std::byte* boot{};
    size_t boot_size{};
    word_t abi{};
    bool page_size{};
    for (; aux->type != AT_NULL; ++aux) {
        switch (aux->type) {
        case AT_SYS_ABI: abi = aux->value; break;
        case AT_SYS_STACK_REGION: initial.stack_region = aux->value; break;
        case AT_SYS_IPC_REGION: initial.ipc_region = aux->value; break;
        case AT_SYS_GUARD_SIZE: initial.guard_size = aux->value; break;
        case AT_PAGESZ: page_size = true; if (aux->value != 4096) _thread_exit(STATUS_BAD_ARGS); break;
        case AT_SYS_QUOTA: startup.quota = aux->value; break;
        case AT_SYS_SCOPE: startup.scope = aux->value; break;
        case AT_SYS_DOMAIN: startup.domain = aux->value; break;
        case AT_SYS_VSPACE: startup.vspace = aux->value; break;
        case AT_SYS_CSPACE: startup.cspace = aux->value; break;
        case AT_SYS_EVENTS: initial_context.event_cap = aux->value; break;
        case AT_SYS_CPUS: startup.cpus = aux->value; break;
        case AT_SYS_IPC: initial_context.ipc = aux->value; break;
        case AT_SYS_LIFECYCLE: startup.lifecycle = aux->value; break;
        case AT_SYS_THREAD: initial.thread = aux->value; break;
        case AT_SYS_SC: initial.sc = aux->value; break;
        case AT_SYS_STACK_MEM: initial.stack_memory = aux->value; break;
        case AT_SYS_IPC_MEM: initial.ipc_memory = aux->value; break;
        case AT_SYS_STACK: initial.stack = aux->value; break;
        case AT_SYS_STACK_SIZE: initial.stack_size = aux->value; break;
        case AT_SYS_BOOTSTRAP: startup.bootstrap = aux->value; break;
        case AT_SYS_BOOT: boot = reinterpret_cast<const std::byte*>(aux->value); break;
        case AT_SYS_BOOT_SIZE: boot_size = aux->value; break;
        }
    }
    if (!page_size || abi != NATIVE_ABI_VERSION || !initial.stack_region || !initial.ipc_region ||
        !initial.stack_memory || !initial.ipc_memory || !initial.thread || !initial.sc ||
        !initial.stack || initial.stack > UINTPTR_MAX - initial.stack_size ||
        !initial.guard_size || initial.guard_size % 4096 || initial.guard_size >= initial.stack_size ||
        !startup.quota || !startup.scope || !startup.vspace || !startup.cspace || !initial_context.event_cap ||
        !startup.cpus || !initial_context.ipc || !startup.domain ||
        bool(boot) != bool(boot_size)) _thread_exit(STATUS_BAD_ARGS);
    if (_cap_info(startup.vspace).status != STATUS_OK) _thread_exit(STATUS_BAD_ARGS);
    CapInfo root; ::memcpy(&root, reinterpret_cast<const void*>(initial_context.ipc), sizeof(root));
    const auto region = [&](cap_t cap, word_t address, word_t bytes) noexcept {
        if (_cap_info(cap).status != STATUS_OK) return false;
        CapInfo info; ::memcpy(&info, reinterpret_cast<const void*>(initial_context.ipc), sizeof(info));
        const auto& v = info.view;
        return info.object_id == root.object_id && v.version == CAP_ATTENUATION_VERSION_CURRENT && v.kind == ObjVSpace
            && v.words[0] == address && v.words[1] == bytes && v.words[3]
            && (v.rights & (RIGHT_MAP | RIGHT_UNMAP)) == (RIGHT_MAP | RIGHT_UNMAP)
            && !(v.rights & (RIGHT_MANAGE | RIGHT_CONTROL | RIGHT_OBSERVE | RIGHT_DESTROY));
    };
    if (!region(initial.stack_region, initial.stack, initial.stack_size)
        || !region(initial.ipc_region, initial_context.ipc, 4096)) _thread_exit(STATUS_BAD_ARGS);
    startup.boot = {boot, boot_size};
    const auto frequency = _clock_frequency();
    if (frequency.status != STATUS_OK || !frequency.value) _thread_exit(STATUS_BAD_ARGS);
    startup.frequency = frequency.value;
    const auto size = size_t(__tls_end - __tls_start);
    const auto alignment = reinterpret_cast<uintptr_t>(__tls_align);
    const auto base = (initial_sp - size) & ~(alignment - 1);
    if (!initial.stack || base < initial.stack + initial.guard_size || base > initial.stack + initial.stack_size) _thread_exit(STATUS_BAD_ARGS);
    return base;
}
extern "C" void crt_tls_init() noexcept {
    auto* destination = static_cast<std::byte*>(__builtin_thread_pointer());
    ::memcpy(destination, __tls_start, __tls_data_end - __tls_start);
    ::memset(destination + (__tls_data_end - __tls_start), 0, __tls_end - __tls_data_end);
    _local = initial_context;
}
auto _events() noexcept -> _ev_port& {
    if (!_local.events) {
        auto* port = new (std::nothrow) _ev_port{_cap_owner{std::exchange(_local.event_cap, 0)}};
        if (!port) _thread_exit(STATUS_NO_MEMORY);
        if (!port->selector()) if (auto s = port->open(startup.alloc()); s != STATUS_OK) _thread_exit(s);
        _local.events = port;
    }
    return *_local.events;
}
extern "C" [[noreturn]] void crt_start(word_t* stack) noexcept {
    const auto argc = static_cast<int>(*stack++);
    auto** source = reinterpret_cast<char**>(stack);
    auto** envp = source + argc + 1;
    size_t envc{}, bytes{};
    for (size_t i = 0; i < size_t(argc); ++i) bytes += ::strlen(source[i]) + 1;
    for (; envp[envc]; ++envc) bytes += ::strlen(envp[envc]) + 1;
    address_space.space = startup.vspace;
    init_heap(startup.alloc(), address_space);
    auto** argv = static_cast<char**>(::malloc((argc + envc + 2) * sizeof(char*) + bytes));
    if (!argv) _thread_exit(STATUS_NO_MEMORY);
    auto* text = reinterpret_cast<char*>(argv + argc + envc + 2);
    const auto copy = [&](char** input, char** output, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            const auto size = ::strlen(input[i]) + 1; output[i] = text;
            ::memcpy(text, input[i], size); text += size;
        }
        output[count] = nullptr;
    };
    environ = argv + argc + 1;
    copy(source, argv, argc); copy(envp, environ, envc);
    _thread_init();
    for (auto ctor : std::span{__init_array_start, __init_array_end}) ctor();
    _exit(main(argc, argv));
}
void _finalize() { __cxxabiv1::__cxa_finalize(nullptr); }
extern "C" [[noreturn]] void exit(int status) noexcept { _exit(status); }

[[noreturn]] void libk::assert_fail(const libk::assert_info&) noexcept {
    _thread_exit(STATUS_INTERNAL);
}
extern "C" [[noreturn]] void abort() noexcept { _thread_exit(STATUS_INTERNAL); }
extern "C" [[noreturn]] void __assert_func(const char*, int, const char*, const char*) {
    _thread_exit(STATUS_INTERNAL);
}

auto _root() noexcept -> const RootInfo& {
    const auto bytes = _startup().boot;
    if (bytes.size() != sizeof(RootInfo) || reinterpret_cast<uintptr_t>(bytes.data()) % alignof(RootInfo)) _thread_exit(STATUS_BAD_ARGS);
    const auto& root = *reinterpret_cast<const RootInfo*>(bytes.data());
    if (root.version != ROOT_VERSION || root.bytes != sizeof(root)) _thread_exit(STATUS_BAD_ARGS);
    return root;
}
