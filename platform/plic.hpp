#pragma once
#include <irq/route.hpp>
#include <cpu.hpp>

// PLIC register protocol; the board supplies mapping and supervisor context.
class Plic final {
public:
    Plic() noexcept : routes_(irq::Routes::Enable::bind<&Plic::set>(*this)) {}
    void start(usize base, usize ctx, u32 count) noexcept;
    void dispatch() noexcept { routes_.dispatch(irq::Routes::Take::bind<&Plic::take>(*this)); }
    auto routes() noexcept -> irq::Routes& { return routes_; }
private:
    void set(u32 id, bool enabled) noexcept;
    auto take() noexcept -> u32;
    auto word(usize offset) const noexcept -> volatile u32* {
        return reinterpret_cast<volatile u32*>(base_ + offset);
    }
    usize base_{};
    usize ctx_{};
    u32 count_{};
    irq::Routes routes_;
};


inline void Plic::start(usize base, usize ctx, u32 count) noexcept {
    auto setup = [&]() noexcept {
        libk_assert(base_ == 0);
        base_ = base;
        ctx_ = ctx;
        count_ = count;
        // Register geometry is the PLIC's 1024-bit enable bank, not a software
        // route capacity. Firmware enables cannot leak into this context.
        for (usize i = 0; i <= count_ / 32; ++i) *word(0x2000 + ctx_ * 0x80 + i * 4) = 0;
        *word(0x200000 + ctx_ * 0x1000) = 0;
        arch::io_fence();
    };
    routes_.refresh(irq::Routes::Setup::bind(setup));
}

inline void Plic::set(u32 id, bool enabled) noexcept {
    libk_assert(id != 0 && id <= count_);
    if (base_ == 0) return;
    arch::io_fence();
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
    arch::io_fence();
}

inline auto Plic::take() noexcept -> u32 {
    arch::io_fence();
    auto* claim = word(0x200000 + ctx_ * 0x1000 + 4);
    const u32 id = *claim;
    // PLIC may ignore completion after disable. Finish the claim while
    // enabled, before Routes masks delivery. Other controllers implement
    // their own protocol here; this is not the user's completion ack.
    if (id != 0) *claim = id;
    arch::io_fence();
    return id;
}
