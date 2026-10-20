#include <proc.hpp>
#include <lib/fs/fs.hpp>
#include <util.hpp>

static constexpr rt_key proof{"proof", ObjChannel}, ready{"ready", ObjNotification}, crash{"crash", ObjNotification};

int main(int argc, char**) {
    const auto& env = rt_startup();
    if (argc == 2) {
        const std::array<word_t, 1> message{42};
        auto status = ch_send(env.selector(proof), message).status;
        if (status == STATUS_OK) status = sys::notification_signal(env.selector(ready)).status;
        if (status != STATUS_OK) sys::exit(status);
        for (;;) (void)ev_wait(env.events);
    }
    ubus_conn bus, files;
    auto status = bus.bind(env.selector(ubus_directory));
    if (status == STATUS_OK) status = files.connect(bus, "exec", "myos.files", 1, 1);
    if (status != STATUS_OK) sys::exit(status);
    auto source = fs_map(files, "/boot/supervisor.elf", VM_READ | VM_EXECUTE);
    if (!source) sys::exit(source.error());
    auto memory = vm_area::map(rt_vm(), std::move(source->memory), 0,
        (source->size + 4095) & ~size_t{4095}, VM_READ);
    if (!memory) sys::exit(memory.error());
    ev_notice signal;
    if ((status = signal.open(env.alloc(), env.events)) != STATUS_OK) sys::exit(status);
    cap_owner sender{env.selector(proof)};
    const std::array grants{
        proc_cap{.key=proof, .cap=&sender},
        proc_cap{.key=ready, .cap=signal.selector(), .limit=word_t{RIGHT_SIGNAL}}
    };
    const std::array<std::string_view, 2> args{"supervisor", "child"};
    proc_task child;
    status = proc_spawn(child, {env.alloc(), static_cast<uint32_t>(env.cpus), env.domain},
        {.bytes={reinterpret_cast<const std::byte*>(memory->address), source->size},
         .memory=memory->memory.selector(), .offset=source->offset, .perms=VM_READ | VM_EXECUTE},
        {.argv=args, .caps=grants, .resources={.bytes=1024 * 1024, .caps=64, .slots=64, .pages=12}, .events=env.events});
    if (status == STATUS_OK) status = child.start();
    if (status != STATUS_OK) sys::exit(status);
    const auto deadline = *rt_after_ms(2000);
    while (signal.take().status == STATUS_WOULD_BLOCK) {
        if ((status = signal.arm()) == STATUS_OK) status = ev_wait(env.events, deadline).status;
        if (status != STATUS_OK) sys::exit(status);
    }
    if ((status = sys::wait_one(env.selector(crash), 1, 0, deadline).status) != STATUS_OK) sys::exit(status);
    // Abrupt supervisor failure: intentionally skip every child destructor and
    // leave a live descendant. Its owner must close and drain the whole subtree.
    sys::exit(STATUS_INTERNAL);
}
