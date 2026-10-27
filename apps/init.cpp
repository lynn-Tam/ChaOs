#include <libk/cpio.hpp>
#include <mgr/mgr.hpp>

// Native root handoff and supervision use the same recursive ownership as any
// other component. Only this edge knows the boot archive and firmware source.
int main() {
    const auto& env = _startup();
    const auto& root = _root();
    auto archive_memory = _vm_area::map(_vm(), root.bundle, 0, (root.bundle_bytes + 4095) & ~size_t{4095}, VM_READ);
    if (!archive_memory) return archive_memory.error();
    const auto archive = libk::cpio_view::parse({reinterpret_cast<const std::byte*>(archive_memory->address), root.bundle_bytes});
    if (!archive) return STATUS_BAD_ARGS;
    mgr owner{[&](mgr_child& child) -> std::expected<_proc_image, status_t> {
        auto name = std::string_view{child.program()};
        while (name.starts_with('/')) name.remove_prefix(1);
        const auto file = archive.find(name);
        if (!file) return std::unexpected(STATUS_NOT_FOUND);
        return _proc_image{.bytes=file->data};
    }};
    auto status = owner.open(); if (status != STATUS_OK) return status;
    const auto queried = _quota_info(env.quota);
    if (queried.status != STATUS_OK) return queried.status;
    const auto budget = *reinterpret_cast<const QuotaInfo*>(_ipc());
    if (budget.memory_available < 8 * 1024 * 1024 || budget.caps_available < 256) return STATUS_NO_MEMORY;
    auto recipe = mgr_recipe("root", "/boot/mgr.elf",
        {.bytes=budget.memory_available - 4 * 1024 * 1024, .caps=budget.caps_available - 128,
         .kinds=OBJ_KINDS, .slots=1024, .pages=132, .quota_perms=RIGHT_CREATE | RIGHT_SPLIT | RIGHT_INSPECT});
    ubus_data exports{ubus_type::Array}; exports.push("*");
    recipe.data.put("argv0", "mgr").put("start", true).put("restart", true).put("exports", exports);
    ubus_data payload;
    const auto archive_idx = recipe.attach(root.bundle, RIGHT_MAP | RIGHT_DUPLICATE);
    if (!archive_idx) return archive_idx.error();
    const auto firmware_idx = recipe.attach(root.firmware, RIGHT_MAP | RIGHT_DUPLICATE);
    if (!firmware_idx) return firmware_idx.error();
    const auto phys_idx = recipe.attach(root.physical, RIGHT_CREATE | RIGHT_CONTROL | RIGHT_INSPECT | RIGHT_DUPLICATE);
    if (!phys_idx) return phys_idx.error();
    payload.put("archive", ubus_buffer{*archive_idx, 0, root.bundle_bytes})
        .put("firmware", ubus_buffer{*firmware_idx, root.firmware_offset, root.firmware_bytes})
        .cap("physical", *phys_idx)
        .put("format", root.firmware_format).put("hart", root.boot_hart);
    recipe.data.put("setup", payload);
    const auto child = owner.configure(std::move(recipe)); if (!child) return child.error();
    for (;;) {
        status = owner.poll(); if (status != STATUS_OK) return status;
        status = _ev_wait(&_events(), owner.deadline()).status;
        if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
    }
}
