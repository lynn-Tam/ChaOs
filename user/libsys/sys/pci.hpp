#pragma once
#include <algorithm>
#include <libk/pci.hpp>
#include <sys/handle.hpp>
#include <sys/start.hpp>

namespace sys::pci {
inline constexpr boot::Import Config{"pci.cfg", 0x50434920, OBJECT_KIND_MEMORY};
inline constexpr boot::Import Window{"pci.mmio", 0x50434920, OBJECT_KIND_MEMORY};
inline constexpr boot::Import Host{"pci.host", 0x50434920, OBJECT_KIND_IO_HOST};
inline constexpr boot::Import Firmware{"firmware", 0x46445420, OBJECT_KIND_MEMORY};
inline void fence() noexcept { asm volatile("fence iorw, iorw" ::: "memory"); }

struct Binding { cap::OwnedCap cap; uint16_t rid{}; };

// Root owns discovery, BAR placement and driver selection. Configuration must
// remain frozen while a derived requester capability has a live DMA session.
struct Bus {
    MappedMemory cfg, desc;
    cap_t host{};
    uint64_t next{}, end{};
    static auto open(const boot::BootView& info, uintptr_t address) noexcept -> std::expected<Bus, status_t> {
        const auto* config = info.find(Config);
        const auto* window = info.find(Window);
        if (!config || !window || !info.selector(Host) || !config->bytes || config->bytes % 4096 ||
            config->bytes / 4096 > 65536 || !window->bytes ||
            window->phys > UINT64_MAX - window->bytes) return std::unexpected(STATUS_BAD_ARGS);
        const auto copy = cap_duplicate(config->handle, info.selector(BOOT_CSPACE), RIGHT_MAP);
        if (copy.status != STATUS_OK) return std::unexpected(copy.status);
        auto cfg = MappedMemory::map(info.selector(BOOT_VSPACE), cap::OwnedCap{{copy.value, 0}},
            address, config->bytes, VM_READ | VM_WRITE);
        if (!cfg) return std::unexpected(cfg.error());
        auto desc = MappedMemory::create(info.selector(BOOT_POOL), info.selector(BOOT_VSPACE),
            address + config->bytes, 4096);
        if (!desc) return std::unexpected(desc.error());
        return Bus{std::move(*cfg), std::move(*desc), info.selector(Host), window->phys, window->phys + window->bytes};
    }
    auto count(uint32_t id) const noexcept -> size_t {
        size_t count{};
        for (size_t rid = 0; rid < cfg.size / 4096; ++rid)
            count += ::pci::Cfg<fence>{cfg.address + rid * 4096}.read<uint32_t>(0) == id;
        return count;
    }
    auto find(uint32_t id, size_t ordinal) noexcept -> std::expected<Binding, status_t> {
        for (size_t rid = 0; rid < cfg.size / 4096; ++rid) {
            if (::pci::Cfg<fence>{cfg.address + rid * 4096}.read<uint32_t>(0) != id || ordinal--) continue;
            const CapView view{CAP_ATTENUATION_VERSION_CURRENT, OBJECT_KIND_IO_HOST,
                CAP_ATTENUATION_SIZE, RIGHT_CONNECT | RIGHT_DUPLICATE | RIGHT_DELEGATE | RIGHT_REVOKE,
                {rid, 1}};
            cap::encode(view, *reinterpret_cast<uint8_t (*)[CAP_ATTENUATION_SIZE]>(desc.address));
            const auto result = cap_typed_delegate(host, 0, desc.memory.selector(), 0);
            if (result.status != STATUS_OK) return std::unexpected(result.status);
            return Binding{cap::OwnedCap{{result.value, 0}}, static_cast<uint16_t>(rid)};
        }
        return std::unexpected(STATUS_NOT_FOUND);
    }
    auto configure(uint32_t id) noexcept -> status_t {
        for (size_t rid = 0; rid < cfg.size / 4096; ++rid) {
            const ::pci::Cfg<fence> fn{cfg.address + rid * 4096};
            if (fn.read<uint32_t>(0) != id) continue;
            fn.write<uint16_t>(4, 1 << 10);
            const auto bars = fn.bars();
            if (!bars) return STATUS_INVALID_OP;
            for (size_t i = 0; i < bars->size(); ++i) {
                const auto& bar = (*bars)[i];
                if (!bar.size) continue;
                const uint64_t bytes = std::max<uint64_t>(bar.size, 4096);
                if (next > end || bytes > end - next) return STATUS_NO_MEMORY;
                const uint64_t pa = (next + bytes - 1) & ~(bytes - 1);
                if (pa > end - bytes || (!bar.wide && pa > UINT32_MAX)) return STATUS_NO_MEMORY;
                const auto flags = fn.read<uint32_t>(0x10 + i * 4) & 15;
                fn.write<uint32_t>(0x10 + i * 4, static_cast<uint32_t>(pa) | flags);
                if (bar.wide) fn.write<uint32_t>(0x10 + 4 * ++i, static_cast<uint32_t>(pa >> 32));
                next = pa + bytes;
            }
            fn.write<uint16_t>(4, 2 | (1 << 10));
        }
        return STATUS_OK;
    }
};
} // namespace sys::pci
