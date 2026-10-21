#include <start.hpp>
#include <vm.hpp>
#include <sys/syscall.hpp>
#include <climits>
#include <libk/assert.hpp>
#include <string.h>
#include <vector>
#include <cstdlib>

static rt_env startup;
static constinit vm_arena address_space;
auto rt_vm() noexcept -> vm_arena& { return address_space; }
extern "C" { char** environ; }
static std::vector<void (*)()>* crt_finalizers;
extern "C" int atexit(void (*function)()) noexcept {
    if (!crt_finalizers) crt_finalizers = new std::vector<void (*)()>;
    crt_finalizers->push_back(function);
    return 0;
}
auto rt_startup() noexcept -> const rt_env& { return startup; }
extern "C" void init_heap(CreateAuth auth, vm_arena& arena) noexcept;
extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();
extern int main(int, char**);

extern "C" [[noreturn]] void crt_start(word_t* stack) noexcept {
    if (*stack > INT_MAX) sys::exit(STATUS_BAD_ARGS);
    const auto argc = static_cast<int>(*stack++);
    auto** argv = reinterpret_cast<char**>(stack);
    if (argv[argc]) sys::exit(STATUS_BAD_ARGS);
    auto** envp = argv + argc + 1;
    environ = envp;
    while (*envp) ++envp;
    const auto* aux = reinterpret_cast<const AuxEntry*>(envp + 1);
    const std::byte* boot{};
    size_t boot_size{};
    for (; aux->type != AT_NULL; ++aux) {
        switch (aux->type) {
        case AT_PAGESZ: if (aux->value != 4096) sys::exit(STATUS_BAD_ARGS); break;
        case AT_SYS_POOL: startup.pool = aux->value; break;
        case AT_SYS_SCOPE: startup.scope = aux->value; break;
        case AT_SYS_DOMAIN: startup.domain = aux->value; break;
        case AT_SYS_VSPACE: startup.vspace = aux->value; break;
        case AT_SYS_CSPACE: startup.cspace = aux->value; break;
        case AT_SYS_EVENTS: startup.events = aux->value; break;
        case AT_SYS_CPUS: startup.cpus = aux->value; break;
        case AT_SYS_IPC: startup.ipc = aux->value; break;
        case AT_SYS_ENV: startup.environment = aux->value; break;
        case AT_SYS_BOOT: boot = reinterpret_cast<const std::byte*>(aux->value); break;
        case AT_SYS_BOOT_SIZE: boot_size = aux->value; break;
        }
    }
    if (!startup.pool || !startup.scope || !startup.vspace || !startup.cspace || !startup.events ||
        !startup.cpus || !startup.ipc || !startup.domain ||
        bool(boot) != bool(boot_size)) sys::exit(STATUS_BAD_ARGS);
    startup.boot = {boot, boot_size};
    address_space.space = startup.vspace;
    address_space.begin = 0x100000000;
    address_space.end = 0x2000000000;
    init_heap(startup.alloc(), address_space);
    for (auto ctor : std::span{__init_array_start, __init_array_end}) ctor();
    const auto status = main(argc, argv);
    if (crt_finalizers) {
        while (!crt_finalizers->empty()) {
            const auto function = crt_finalizers->back(); crt_finalizers->pop_back();
            function();
        }
        delete crt_finalizers;
    }
    sys::exit(status);
}

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
