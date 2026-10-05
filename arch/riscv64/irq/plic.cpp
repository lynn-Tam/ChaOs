#include "plic.hpp"

void Plic::start(usize base, usize ctx) noexcept {
    auto setup = [&]() noexcept {
        libk_assert(base_ == 0);
        base_ = base;
        ctx_ = ctx;
        // Register geometry is the PLIC's 1024-bit enable bank, not a software
        // route capacity. Firmware enables cannot leak into this context.
        for (usize i = 0; i < 32; ++i) *word(0x2000 + ctx_ * 0x80 + i * 4) = 0;
        *word(0x200000 + ctx_ * 0x1000) = 0;
        asm volatile("fence iorw, iorw" ::: "memory");
    };
    routes_.refresh(irq::Routes::Setup::bind(setup));
}

void Plic::set(u32 id, bool enabled) noexcept {
    libk_assert(id != 0 && id < 1024);
    if (base_ == 0) return;
    asm volatile("fence iorw, iorw" ::: "memory");
    auto* priority = word(id * 4);
    if (*priority == 0) *priority = 1;
    auto* bank = word(0x2000 + ctx_ * 0x80 + (id / 32) * 4);
    if (enabled) {
        *bank |= u32{1} << (id % 32);
        // Refresh QEMU's output when an event arrived while disabled.
        *priority = *priority;
    } else {
        *bank &= ~(u32{1} << (id % 32));
    }
    asm volatile("fence iorw, iorw" ::: "memory");
}

auto Plic::take() noexcept -> u32 {
    asm volatile("fence iorw, iorw" ::: "memory");
    auto* claim = word(0x200000 + ctx_ * 0x1000 + 4);
    const u32 id = *claim;
    // PLIC may ignore completion after disable. Finish the claim while
    // enabled, before Routes masks delivery. Other controllers implement
    // their own protocol here; this is not the user's completion ack.
    if (id != 0) *claim = id;
    asm volatile("fence iorw, iorw" ::: "memory");
    return id;
}
