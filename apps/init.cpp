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
    const auto queried = sys::quota_info(env.quota);
    if (queried.status != STATUS_OK) return queried.status;
    const auto budget = *reinterpret_cast<const QuotaInfo*>(rt_current().ipc);
    if (budget.memory_available < 8 * 1024 * 1024 || budget.caps_available < 256) return STATUS_NO_MEMORY;
    auto& child = owner.add("root", "/boot/mgr.elf",
        {.bytes=budget.memory_available - 4 * 1024 * 1024, .caps=budget.caps_available - 128,
         .kinds=OBJ_KINDS, .slots=1024, .pages=132, .quota_perms=RIGHT_CREATE | RIGHT_SPLIT | RIGHT_INSPECT});
    child.args = {"mgr"}; child.desired = child.restart = true; child.exports.push("*");
    const auto archive_idx = child.setup.attach(root.bundle, RIGHT_MAP | RIGHT_DUPLICATE);
    if (!archive_idx) return archive_idx.error();
    const auto firmware_idx = child.setup.attach(root.firmware, RIGHT_MAP | RIGHT_DUPLICATE);
    if (!firmware_idx) return firmware_idx.error();
    const auto phys_idx = child.setup.attach(root.physical, RIGHT_CREATE | RIGHT_CONTROL | RIGHT_DUPLICATE);
    if (!phys_idx) return phys_idx.error();
    child.setup.data.put("archive", ubus_buffer{*archive_idx, 0, root.bundle_bytes})
        .put("firmware", ubus_buffer{*firmware_idx, root.firmware_offset, root.firmware_bytes})
        .cap("physical", *phys_idx)
        .put("format", root.firmware_format).put("hart", root.boot_hart);
    for (;;) {
        status = owner.poll(); if (status != STATUS_OK) return status;
        status = ev_wait(rt_events(), owner.deadline()).status;
        if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
    }
}
