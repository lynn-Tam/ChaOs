#include <sync.hpp>

#include <cpu.hpp>
#include <cpu/cpu.hpp>

namespace sync {

// Early boot runs on one CPU before Cpu publication. It still has a
// distinct nonzero owner, so a locked mutex never looks unowned.
static auto identity() noexcept -> usize {
    return Cpu::local() ? reinterpret_cast<usize>(Cpu::local()) : 1;
}

void Spin::lock() noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(!held());
    mutex_.lock();
    owner_.store(identity(), std::memory_order_relaxed);
    if (auto* c = Cpu::local()) ++c->locks;
}

auto Spin::try_lock() noexcept -> bool {
    libk_assert(!arch::interrupts_enabled());
    if (!mutex_.try_lock()) return false;
    owner_.store(identity(), std::memory_order_relaxed);
    if (auto* c = Cpu::local()) ++c->locks;
    return true;
}

void Spin::unlock() noexcept {
    libk_assert(held());
    if (auto* c = Cpu::local()) {
        libk_assert(c->locks != 0);
        --c->locks;
    }
    owner_.store(0, std::memory_order_relaxed);
    mutex_.unlock();
}

auto Spin::held() const noexcept -> bool {
    return owner_.load(std::memory_order_relaxed) == identity();
}

void assert_unlocked() noexcept {
    if (auto* c = Cpu::local()) libk_assert(c->locks == 0);
}

} // namespace sync
