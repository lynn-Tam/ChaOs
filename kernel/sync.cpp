#include <sync.hpp>

#include <cpu.hpp>
#include <cpu/local.hpp>

namespace sync {

static auto cpu() noexcept -> CpuLocal* {
    return (arch::local() ? arch::local()->owner : nullptr);
}

// Early boot runs on one CPU before CpuLocal publication. It still has a
// distinct nonzero owner, so a locked mutex never looks unowned.
static auto identity() noexcept -> usize {
    return cpu() ? reinterpret_cast<usize>(cpu()) : 1;
}

void Spin::lock() noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(!held());
    mutex_.lock();
    owner_.store<libk::MemoryOrder::Relaxed>(identity());
    if (auto* c = cpu()) ++c->locks;
}

auto Spin::try_lock() noexcept -> bool {
    libk_assert(!arch::interrupts_enabled());
    if (!mutex_.try_lock()) return false;
    owner_.store<libk::MemoryOrder::Relaxed>(identity());
    if (auto* c = cpu()) ++c->locks;
    return true;
}

void Spin::unlock() noexcept {
    libk_assert(held());
    if (auto* c = cpu()) {
        libk_assert(c->locks != 0);
        --c->locks;
    }
    owner_.store<libk::MemoryOrder::Relaxed>(0);
    mutex_.unlock();
}

auto Spin::held() const noexcept -> bool {
    return owner_.load<libk::MemoryOrder::Relaxed>() == identity();
}

void assert_unlocked() noexcept {
    if (auto* c = cpu()) libk_assert(c->locks == 0);
}

} // namespace sync
