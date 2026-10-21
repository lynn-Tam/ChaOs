#include <mgr/mgr.hpp>

// Native root handoff and supervision use the same recursive ownership as any
// other component. Only this edge knows the boot archive and firmware source.
int main() {
    const auto& env = rt_startup();
    const auto& root = rt_root();
    proc_archive archive;
    auto status = archive.open(root.bundle, root.bundle_bytes);
    if (status != STATUS_OK) return status;
    mgr owner{[&](mgr_child& child) { return archive.image(child.image); }};
    status = owner.open(); if (status != STATUS_OK) return status;
    const auto queried = sys::account_info(env.pool);
    if (queried.status != STATUS_OK) return queried.status;
    const auto budget = *reinterpret_cast<const AccountInfo*>(env.ipc);
    if (budget.memory_available < 8 * 1024 * 1024 || budget.caps_available < 256) return STATUS_NO_MEMORY;
    auto& child = owner.add("root", "mgr",
        {.bytes=budget.memory_available - 4 * 1024 * 1024, .caps=budget.caps_available - 128,
         .kinds=OBJ_KINDS, .slots=1024, .pages=132, .pool_perms=RIGHT_CREATE | RIGHT_SPLIT | RIGHT_INSPECT});
    child.args = {"mgr"}; child.desired = child.restart = true; child.exports.push("*");
    child.setup.data.put("archive", ubus_buffer{*child.setup.attach(root.bundle, RIGHT_MAP | RIGHT_DUPLICATE), 0, root.bundle_bytes})
        .put("firmware", ubus_buffer{*child.setup.attach(root.firmware, RIGHT_MAP | RIGHT_DUPLICATE), root.firmware_offset, root.firmware_bytes})
        .cap("physical", *child.setup.attach(root.physical, RIGHT_CREATE | RIGHT_CONTROL | RIGHT_DUPLICATE))
        .put("format", root.firmware_format).put("hart", root.boot_hart);
    for (;;) {
        status = owner.poll(); if (status != STATUS_OK) return status;
        status = ev_wait(env.events, owner.deadline()).status;
        if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
    }
}
