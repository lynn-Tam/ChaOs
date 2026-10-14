#include <start.hpp>
#include <sys/syscall.hpp>
#include <climits>
#include <libk/assert.hpp>
#include <string.h>

static rt_env startup;
auto rt_startup() noexcept -> const rt_env& { return startup; }
extern "C" void init_heap(cap_t pool, cap_t vspace) noexcept;
extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();
extern int main(int, char**);

extern "C" [[noreturn]] void crt_start(word_t* stack) noexcept {
    if (*stack > INT_MAX) sys::exit(STATUS_BAD_ARGS);
    const auto argc = static_cast<int>(*stack++);
    auto** argv = reinterpret_cast<char**>(stack);
    if (argv[argc]) sys::exit(STATUS_BAD_ARGS);
    auto** envp = argv + argc + 1;
    while (*envp) ++envp;
    const auto* aux = reinterpret_cast<const AuxEntry*>(envp + 1);
    const std::byte* bindings{};
    const std::byte* boot{};
    size_t binding_size{}, boot_size{};
    for (; aux->type != AT_NULL; ++aux) {
        switch (aux->type) {
        case AT_PAGESZ: if (aux->value != 4096) sys::exit(STATUS_BAD_ARGS); break;
        case AT_SYS_POOL: startup.pool = aux->value; break;
        case AT_SYS_VSPACE: startup.vspace = aux->value; break;
        case AT_SYS_CSPACE: startup.cspace = aux->value; break;
        case AT_SYS_EVENTS: startup.events = aux->value; break;
        case AT_SYS_CPUS: startup.cpus = aux->value; break;
        case AT_SYS_IPC: startup.ipc = aux->value; break;
        case AT_SYS_BINDINGS: bindings = reinterpret_cast<const std::byte*>(aux->value); break;
        case AT_SYS_BINDINGS_SIZE: binding_size = aux->value; break;
        case AT_SYS_BOOT: boot = reinterpret_cast<const std::byte*>(aux->value); break;
        case AT_SYS_BOOT_SIZE: boot_size = aux->value; break;
        }
    }
    if (!startup.pool || !startup.vspace || !startup.cspace || !startup.events ||
        !startup.cpus || !startup.ipc || bool(bindings) != bool(binding_size) ||
        bool(boot) != bool(boot_size)) sys::exit(STATUS_BAD_ARGS);
    startup.bindings = {bindings, binding_size};
    startup.boot = {boot, boot_size};
    if (bindings) {
        if (reinterpret_cast<uintptr_t>(bindings) % alignof(rt_binding) || binding_size < sizeof(rt_bindings))
            sys::exit(STATUS_BAD_ARGS);
        const auto& h = *reinterpret_cast<const rt_bindings*>(bindings);
        if (h.version != rt_bindings_version || h.reserved || h.size != binding_size ||
            h.count > (binding_size - sizeof(h)) / sizeof(rt_binding)) sys::exit(STATUS_BAD_ARGS);
        const auto* entries = reinterpret_cast<const rt_binding*>(&h + 1);
        const auto strings = sizeof(h) + h.count * sizeof(rt_binding);
        for (size_t i = 0; i < h.count; ++i) {
            const auto& b = entries[i];
            if (!b.cap || !b.kind || b.kind >= ObjCount || b.reserved || !b.major ||
                !b.name_size || b.name < strings || b.name > binding_size ||
                b.name_size > binding_size - b.name || (b.kind != ObjMem && b.bytes))
                sys::exit(STATUS_BAD_ARGS);
            const std::string_view name{reinterpret_cast<const char*>(bindings) + b.name, b.name_size};
            if (name.find('\0') != std::string_view::npos) sys::exit(STATUS_BAD_ARGS);
            for (size_t j = 0; j < i; ++j)
                if (name == std::string_view{reinterpret_cast<const char*>(bindings) + entries[j].name,
                                             entries[j].name_size}) sys::exit(STATUS_BAD_ARGS);
        }
    }
    init_heap(startup.pool, startup.vspace);
    for (auto ctor : std::span{__init_array_start, __init_array_end}) ctor();
    sys::exit(main(argc, argv));
}

[[noreturn]] void libk::assert_fail(const libk::assert_info&) noexcept {
    sys::exit(STATUS_INTERNAL);
}
extern "C" [[noreturn]] void abort() noexcept { sys::exit(STATUS_INTERNAL); }
extern "C" [[noreturn]] void __assert_func(const char*, int, const char*, const char*) {
    sys::exit(STATUS_INTERNAL);
}
