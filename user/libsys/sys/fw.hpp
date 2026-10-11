#pragma once
#include <libk/fdt.hpp>
#include <sys/start.hpp>

namespace sys {
// Root's read-only firmware mapping. Tree identities locate authorized boot
// resources; a matching description never creates a hardware capability.
struct Firmware {
    mapping memory;
    libk::fdt tree;
    boot::BootView boot;
    static auto open(const boot::BootView& boot, uintptr_t address) noexcept
        -> std::expected<Firmware, status_t> {
        const auto* fw = boot.record(BOOT_FIRMWARE);
        if (!fw || fw->firmware.format != FW_FDT || fw->bytes > SIZE_MAX - 8190)
            return std::unexpected(STATUS_BAD_ARGS);
        const auto offset = fw->phys % 4096;
        const auto bytes = (offset + fw->bytes + 4095) & ~uint64_t{4095};
        const auto cap = cap_duplicate(fw->handle, boot.selector(BOOT_CSPACE), RIGHT_MAP);
        if (cap.status != STATUS_OK) return std::unexpected(cap.status);
        auto memory = mapping::map(boot.selector(BOOT_VSPACE), cap_owner{cap.value},
            address, bytes, VM_READ);
        if (!memory) return std::unexpected(memory.error());
        const auto tree = libk::fdt::open(std::span{
            reinterpret_cast<const uint8_t*>(memory->address + offset), static_cast<size_t>(fw->bytes)});
        if (!tree) return std::unexpected(STATUS_BAD_ARGS);
        return Firmware{std::move(*memory), *tree, boot};
    }
    auto console(obj_kind_t kind) const noexcept -> cap_t {
        const auto chosen = tree.child(tree.root(), "chosen");
        const auto p = chosen ? tree.property(*chosen, "stdout-path") : std::nullopt;
        const auto path = p ? libk::fdt::first_string(*p) : std::nullopt;
        const auto node = path ? tree.path(*path) : std::nullopt;
        const auto* cap = node ? boot.resource(*node, kind) : nullptr;
        return cap ? cap->handle : 0;
    }
};
}
