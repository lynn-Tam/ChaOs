#include <proc.hpp>
#include <util.hpp>
#include <ubus/value.hpp>

// Kernel handoff and one supervised root. Deployment and hardware policy live
// below this boundary; closing the root Scope also retires its descendants.
int main() {
    const auto& env = rt_startup();
    const auto& root = rt_root();
    proc_archive archive;
    auto status = archive.open(root.bundle, root.bundle_bytes);
    if (status != STATUS_OK) sys::exit(status);
    const auto image = archive.image("system");
    if (!image) sys::exit(image.error());
    ubus_data firmware, config;
    firmware.put("size", root.firmware_bytes).put("offset", root.firmware_offset)
        .put("format", root.firmware_format).put("hart", root.boot_hart);
    config.put("firmware", firmware);
    const auto created = sys::memory_create(env.alloc(), 4096, VM_READ | VM_WRITE);
    if (created.status != STATUS_OK) return created.status;
    cap_owner metadata{created.value};
    status = mem_write(metadata.selector(), 0, config.bytes());
    if (status == STATUS_OK) status = sys::memory_seal(metadata.selector()).status;
    if (status != STATUS_OK) sys::exit(status);
    const std::array grants{
        proc_cap{.key={"archive", ObjMem}, .bytes=root.bundle_bytes, .cap=root.bundle, .limit=word_t{RIGHT_MAP | RIGHT_DUPLICATE}},
        proc_cap{.key={"firmware", ObjMem}, .bytes=root.firmware_bytes, .cap=root.firmware, .limit=word_t{RIGHT_MAP | RIGHT_DUPLICATE}},
        proc_cap{.key={"physical", ObjPhys}, .cap=root.physical, .limit=word_t{RIGHT_CREATE | RIGHT_CONTROL | RIGHT_DUPLICATE}},
        proc_cap{.key={"boot", ObjMem}, .bytes=config.bytes().size(), .cap=metadata.selector(), .limit=word_t{RIGHT_MAP | RIGHT_DUPLICATE}}
    };
    for (;;) {
        const auto queried = sys::account_info(env.pool);
        if (queried.status != STATUS_OK) return queried.status;
        AccountInfo budget; ::memcpy(&budget, reinterpret_cast<const void*>(env.ipc), sizeof(budget));
        if (budget.memory_available < 8 * 1024 * 1024 || budget.caps_available < 256) sys::exit(STATUS_NO_MEMORY);
        proc_task task;
        const std::array<std::string_view, 1> args{"system"};
        status = proc_spawn(task, {env.alloc(), static_cast<uint32_t>(env.cpus), env.domain}, *image,
            {.argv=args, .caps=grants,
             .resources={.bytes=budget.memory_available - 4 * 1024 * 1024, .caps=budget.caps_available - 128,
                .kinds=OBJ_KINDS, .slots=1024, .pages=132, .pool_perms=RIGHT_CREATE | RIGHT_SPLIT},
             .events=env.events});
        if (status == STATUS_OK) status = task.start();
        if (status != STATUS_OK) (void)task.stop(status);
        for (;;) {
            status = task.poll();
            if (status == STATUS_OK) { (void)task.collect(); break; }
            if (status != STATUS_WOULD_BLOCK && !proc_retryable(status)) sys::exit(status);
            status = ev_wait(env.events, task.polling() ? rt_after_ms(1).value_or(0) : 0).status;
            if (status != STATUS_OK && status != STATUS_TIMED_OUT) sys::exit(status);
        }
    }
}
