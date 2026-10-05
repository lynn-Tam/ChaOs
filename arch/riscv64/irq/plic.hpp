#pragma once
#include <irq/route.hpp>

// PLIC register protocol; the board supplies mapping and supervisor context.
class Plic final {
public:
    Plic() noexcept : routes_(irq::Routes::Enable::bind<&Plic::set>(*this)) {}
    void start(usize base, usize ctx) noexcept;
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
    irq::Routes routes_;
};
