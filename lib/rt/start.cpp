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

static rt_env startup;
constinit thread_local rt_context rt_local;
static rt_context initial_context;
static rt_bootstrap initial;
auto rt_initial() noexcept -> const rt_bootstrap& { return initial; }
extern "C" const std::byte __tls_start[], __tls_data_end[], __tls_end[], __tls_align[];
static constinit vm_arena address_space;
auto rt_vm() noexcept -> vm_arena& { return address_space; }
extern "C" { char** environ; }
struct crt_finalizer {
    void (*function)(void*); void (*plain)(); void* argument; void* dso; crt_finalizer* next;
};
static crt_finalizer* finalizers;
static constinit rt_mutex finalizer_mutex;
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
        (void)rt_wait32(state, value);
    }
}
extern "C" void __cxxabiv1::__cxa_guard_release(__guard* guard) noexcept {
    auto* state = reinterpret_cast<uint32_t*>(guard); __atomic_store_n(state, 1, __ATOMIC_RELEASE); rt_wake(state, true);
}
extern "C" void __cxxabiv1::__cxa_guard_abort(__guard* guard) noexcept {
    auto* state = reinterpret_cast<uint32_t*>(guard); __atomic_store_n(state, 0, __ATOMIC_RELEASE); rt_wake(state, true);
}
auto rt_startup() noexcept -> const rt_env& { return startup; }
extern "C" void init_heap(CreateAuth auth, vm_arena& arena) noexcept;
extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();
extern int main(int, char**);

extern "C" auto crt_boot(word_t* stack) noexcept -> uintptr_t {
    const auto initial_sp = reinterpret_cast<uintptr_t>(stack);
    if (*stack > INT_MAX) sys::exit(STATUS_BAD_ARGS);
    const auto argc = static_cast<int>(*stack++);
    auto** argv = reinterpret_cast<char**>(stack);
    if (argv[argc]) sys::exit(STATUS_BAD_ARGS);
    auto** envp = argv + argc + 1;
    while (*envp) ++envp;
    const auto* aux = reinterpret_cast<const AuxEntry*>(envp + 1);
    const std::byte* boot{};
    size_t boot_size{};
    for (; aux->type != AT_NULL; ++aux) {
        switch (aux->type) {
        case AT_PAGESZ: if (aux->value != 4096) sys::exit(STATUS_BAD_ARGS); break;
        case AT_SYS_QUOTA: startup.quota = aux->value; break;
        case AT_SYS_SCOPE: startup.scope = aux->value; break;
        case AT_SYS_DOMAIN: startup.domain = aux->value; break;
        case AT_SYS_VSPACE: startup.vspace = aux->value; break;
        case AT_SYS_CSPACE: startup.cspace = aux->value; break;
        case AT_SYS_EVENTS: initial_context.events = aux->value; break;
        case AT_SYS_CPUS: startup.cpus = aux->value; break;
        case AT_SYS_IPC: initial_context.ipc = aux->value; break;
        case AT_SYS_LIFECYCLE: startup.lifecycle = aux->value; break;
        case AT_SYS_THREAD: initial.thread = aux->value; break;
        case AT_SYS_SC: initial.sc = aux->value; break;
        case AT_SYS_STACK_MEM: initial.stack_memory = aux->value; break;
        case AT_SYS_IPC_MEM: initial.ipc_memory = aux->value; break;
        case AT_SYS_STACK: initial.stack = aux->value; break;
        case AT_SYS_STACK_SIZE: initial.stack_size = aux->value; break;
        case AT_SYS_ENV: startup.environment = aux->value; break;
        case AT_SYS_BOOT: boot = reinterpret_cast<const std::byte*>(aux->value); break;
        case AT_SYS_BOOT_SIZE: boot_size = aux->value; break;
        }
    }
    if (!startup.quota || !startup.scope || !startup.vspace || !startup.cspace || !initial_context.events ||
        !startup.cpus || !initial_context.ipc || !startup.domain ||
        bool(boot) != bool(boot_size)) sys::exit(STATUS_BAD_ARGS);
    startup.boot = {boot, boot_size};
    const auto size = size_t(__tls_end - __tls_start);
    const auto alignment = reinterpret_cast<uintptr_t>(__tls_align);
    const auto base = (initial_sp - size) & ~(alignment - 1);
    if (!initial.stack || base < initial.stack + 4096 || base > initial.stack + initial.stack_size) sys::exit(STATUS_BAD_ARGS);
    return base;
}
extern "C" void crt_tls_init() noexcept {
    auto* destination = static_cast<std::byte*>(__builtin_thread_pointer());
    ::memcpy(destination, __tls_start, __tls_data_end - __tls_start);
    ::memset(destination + (__tls_data_end - __tls_start), 0, __tls_end - __tls_data_end);
    rt_local = initial_context;
}
auto rt_events() noexcept -> cap_t {
    if (!rt_local.events) {
        auto created = sys::port_create(startup.alloc(), 64, 16);
        if (created.status == STATUS_PENDING) created.status = op_wait(created.value).status;
        if (created.status != STATUS_OK) sys::exit(created.status);
        rt_local.events = created.value;
    }
    return rt_local.events;
}
extern "C" [[noreturn]] void crt_start(word_t* stack) noexcept {
    const auto argc = static_cast<int>(*stack++);
    auto** source = reinterpret_cast<char**>(stack);
    auto** envp = source + argc + 1;
    size_t envc{}, bytes{};
    for (size_t i = 0; i < size_t(argc); ++i) bytes += ::strlen(source[i]) + 1;
    for (; envp[envc]; ++envc) bytes += ::strlen(envp[envc]) + 1;
    address_space.space = startup.vspace;
    address_space.begin = 0x100000000;
    address_space.end = 0x2000000000;
    init_heap(startup.alloc(), address_space);
    auto** argv = static_cast<char**>(::malloc((argc + envc + 2) * sizeof(char*) + bytes));
    if (!argv) sys::exit(STATUS_NO_MEMORY);
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
    rt_thread_init();
    for (auto ctor : std::span{__init_array_start, __init_array_end}) ctor();
    rt_exit(main(argc, argv));
}
void rt_finalize() { __cxxabiv1::__cxa_finalize(nullptr); }
extern "C" [[noreturn]] void exit(int status) noexcept { rt_exit(status); }

[[noreturn]] void libk::assert_fail(const libk::assert_info&) noexcept {
    sys::exit(STATUS_INTERNAL);
}
extern "C" [[noreturn]] void abort() noexcept { sys::exit(STATUS_INTERNAL); }
extern "C" [[noreturn]] void __assert_func(const char*, int, const char*, const char*) {
    sys::exit(STATUS_INTERNAL);
}

auto rt_root() noexcept -> const RootInfo& {
    const auto bytes = rt_startup().boot;
    if (bytes.size() != sizeof(RootInfo) || reinterpret_cast<uintptr_t>(bytes.data()) % alignof(RootInfo)) sys::exit(STATUS_BAD_ARGS);
    const auto& root = *reinterpret_cast<const RootInfo*>(bytes.data());
    if (root.version != ROOT_VERSION || root.bytes != sizeof(root)) sys::exit(STATUS_BAD_ARGS);
    return root;
}
