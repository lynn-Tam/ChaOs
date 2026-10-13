#pragma once
#include <start.hpp>
#include <libk/fdt.hpp>
#include <start.hpp>
#include <cap.hpp>

inline auto fw_root(const rt_env& env) noexcept -> const RootInfo& {
    if (env.boot.size() < sizeof(RootInfo) ||
        reinterpret_cast<uintptr_t>(env.boot.data()) % alignof(RootInfo)) sys::exit(STATUS_BAD_ARGS);
    const auto& root = *reinterpret_cast<const RootInfo*>(env.boot.data());
    if (root.version != ROOT_VERSION || root.bytes != env.boot.size() ||
        root.resources_offset < sizeof(root) || root.resources_offset > root.bytes ||
        root.resources_count > (root.bytes - root.resources_offset) / sizeof(RootResource))
        sys::exit(STATUS_BAD_ARGS);
    return root;
}
// Only root's discovery code interprets hardware grants, never the runtime.
struct fw_view {
    vm_area memory;
    libk::fdt tree;
    const RootInfo* root;
    auto resource(uint32_t node, obj_kind_t kind, uint32_t index = 0) const noexcept -> const RootResource* {
        const auto* entries = reinterpret_cast<const RootResource*>(
            reinterpret_cast<const uint8_t*>(root) + root->resources_offset);
        for (const auto& entry : std::span{entries, root->resources_count})
            if (entry.node == node && entry.kind == kind && entry.index == index) return &entry;
        return nullptr;
    }
    static auto open(const rt_env& env, uintptr_t address) noexcept -> std::expected<fw_view, status_t> {
        const auto& root = fw_root(env);
        if (!root.firmware || root.firmware_format != FW_FDT || root.firmware_bytes > SIZE_MAX - 8190)
            return std::unexpected(STATUS_BAD_ARGS);
        const auto offset = root.firmware_offset;
        const auto bytes = (offset + root.firmware_bytes + 4095) & ~uint64_t{4095};
        const auto cap = sys::cap_duplicate(root.firmware, env.cspace, RIGHT_MAP);
        if (cap.status != STATUS_OK) return std::unexpected(cap.status);
        auto memory = vm_area::map(env.vspace, cap_owner{cap.value}, address, bytes, VM_READ);
        if (!memory) return std::unexpected(memory.error());
        const auto tree = libk::fdt::open(std::span{
            reinterpret_cast<const uint8_t*>(memory->address + offset), static_cast<size_t>(root.firmware_bytes)});
        if (!tree) return std::unexpected(STATUS_BAD_ARGS);
        return fw_view{std::move(*memory), *tree, &root};
    }
    auto console(obj_kind_t kind) const noexcept -> cap_t {
        const auto chosen = tree.child(tree.root(), "chosen");
        const auto p = chosen ? tree.property(*chosen, "stdout-path") : std::nullopt;
        const auto path = p ? libk::fdt::first_string(*p) : std::nullopt;
        const auto node = path ? tree.path(*path) : std::nullopt;
        const auto* cap = node ? resource(*node, kind) : nullptr;
        return cap ? cap->handle : 0;
    }
};
