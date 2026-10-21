#include <vm.hpp>
#pragma once
#include <start.hpp>
#include <libk/fdt.hpp>
#include <cap.hpp>
#include <uapi/irq.h>

struct fw_view {
    vm_area memory;
    libk::fdt tree;
    static auto open(const rt_env& env, cap_t firmware, size_t firmware_bytes, uint32_t firmware_offset,
                     uint32_t firmware_format, uintptr_t address = 0) noexcept -> std::expected<fw_view, status_t> {
        if (!firmware || firmware_format != FW_FDT || firmware_bytes > SIZE_MAX - 8190)
            return std::unexpected(STATUS_BAD_ARGS);
        const auto offset = firmware_offset;
        const auto bytes = (offset + firmware_bytes + 4095) & ~uint64_t{4095};
        const auto cap = sys::cap_duplicate(firmware, env.cspace, RIGHT_MAP);
        if (cap.status != STATUS_OK) return std::unexpected(cap.status);
        auto memory = vm_area::map(rt_vm(), cap_owner{cap.value}, address, bytes, VM_READ);
        if (!memory) return std::unexpected(memory.error());
        const auto tree = libk::fdt::open(std::span{
            reinterpret_cast<const uint8_t*>(memory->address + offset), size_t(firmware_bytes)});
        if (!tree) return std::unexpected(STATUS_BAD_ARGS);
        return fw_view{std::move(*memory), *tree};
    }
    static auto open(const rt_env& env, uintptr_t address = 0) noexcept -> std::expected<fw_view, status_t> {
        const auto& root = rt_root();
        return open(env, root.firmware, root.firmware_bytes, root.firmware_offset, root.firmware_format, address);
    }
    auto irq(const rt_env& env, cap_t physical, uint64_t boot_hart) const noexcept -> std::expected<cap_owner, status_t> {
        const auto plic = tree.find("sifive,plic-1.0.0");
        if (!plic) return std::unexpected(STATUS_NOT_FOUND);
        const auto pr = tree.reg(*plic);
        const auto plic_id = tree.number(*plic, "phandle"), count = tree.number(*plic, "riscv,ndev");
        const auto sources = tree.property(*plic, "interrupts-extended");
        if (!pr || !plic_id || !count || !sources ||
            tree.number(*plic, "#interrupt-cells") != 1 || tree.number(*plic, "#address-cells") != 0)
            return std::unexpected(STATUS_BAD_ARGS);
        std::optional<uint64_t> context;
        libk::byte_reader r{sources->data(), sources->size()};
        for (uint64_t index = 0; r.remaining(); ++index) {
            uint32_t ref, interrupt;
            if (!r.read_be(ref) || !r.read_be(interrupt)) return std::unexpected(STATUS_BAD_ARGS);
            const auto intc = tree.phandle(ref);
            const auto cpu = intc ? tree.parent(*intc) : std::nullopt;
            const auto reg = cpu ? tree.property(*cpu, "reg") : std::nullopt;
            if (!intc || !reg || !tree.matches(*intc, "riscv,cpu-intc") ||
                tree.number(*intc, "#interrupt-cells") != 1 || reg->empty() || reg->size() % 4)
                return std::unexpected(STATUS_BAD_ARGS);
            libk::byte_reader cr{reg->data(), reg->size()};
            uint64_t hart;
            if (!libk::fdt::read_cells(cr, reg->size() / 4, hart)) return std::unexpected(STATUS_BAD_ARGS);
            if (hart == boot_hart && interrupt == 9) {
                if (context) return std::unexpected(STATUS_BAD_ARGS);
                context = index;
            }
        }
        auto config = vm_area::create(env.alloc(), rt_vm(), 0, 4096);
        if (!config) return std::unexpected(config.error());
        if (!context) return std::unexpected(STATUS_BAD_ARGS);
        const auto* data = std::construct_at(reinterpret_cast<IrqConfig*>(config->address),
            IRQ_PLIC, *count, pr->address, pr->size, *context, 0U, 0U);
        (void)data;
        const auto control = sys::irq_controller_create(env.alloc(), physical,
                                                       config->memory.selector());
        if (control.status != STATUS_OK) return std::unexpected(control.status);
        return cap_owner{control.value};
    }
    auto irq(const rt_env& env) const noexcept -> std::expected<cap_owner, status_t> {
        return irq(env, rt_root().physical, rt_root().boot_hart);
    }
};

// Root's explicit console composition; the IRQ controller is independently owned.
struct fw_console {
    cap_owner memory, irq;
    size_t offset{};
    static auto open(const rt_env& env, const libk::fdt& tree, cap_t controller, cap_t physical) noexcept
        -> std::expected<fw_console, status_t> {
        const auto chosen = tree.child(tree.root(), "chosen");
        const auto property = chosen ? tree.property(*chosen, "stdout-path") : std::nullopt;
        const auto path = property ? libk::fdt::first_string(*property) : std::nullopt;
        const auto uart = path ? tree.path(*path) : std::nullopt;
        if (!uart) return std::unexpected(STATUS_NOT_FOUND);
        const auto ur = tree.reg(*uart);
        if (!ur || !tree.matches(*uart, "ns16550a") || tree.cell_count(*uart, "reg-shift", 0) ||
            tree.cell_count(*uart, "reg-io-width", 1) != 1) return std::unexpected(STATUS_BAD_ARGS);
        std::optional<uint32_t> parent;
        for (auto n = uart; n && !parent; n = tree.parent(*n)) parent = tree.number(*n, "interrupt-parent");
        const auto plic = tree.find("sifive,plic-1.0.0");
        const auto plic_id = plic ? tree.number(*plic, "phandle") : std::nullopt;
        const auto interrupts = tree.property(*uart, "interrupts");
        uint32_t source;
        if (parent != plic_id || !interrupts || !libk::fdt::read_u32(*interrupts, source))
            return std::unexpected(STATUS_BAD_ARGS);
        fw_console result;
        const auto irq = sys::irq_create(env.alloc(), controller, source);
        if (irq.status != STATUS_OK) return std::unexpected(irq.status);
        result.irq = cap_owner{irq.value};
        result.offset = ur->address % 4096;
        if (ur->size > SIZE_MAX - result.offset - 4095) return std::unexpected(STATUS_BAD_ARGS);
        const auto bytes = (result.offset + ur->size + 4095) & ~size_t{4095};
        const auto mem = sys::memory_create_physical(env.alloc(), physical,
                                      ur->address - result.offset, bytes, VM_READ | VM_WRITE);
        if (mem.status != STATUS_OK) return std::unexpected(mem.status);
        result.memory = cap_owner{mem.value};
        return result;
    }
    static auto open(const rt_env& env, const libk::fdt& tree, cap_t controller) noexcept
        -> std::expected<fw_console, status_t> { return open(env, tree, controller, rt_root().physical); }

};
