#include <utility>
#include <test/test.hpp>
#include <libk/manual_lifetime.hpp>
#include <sync.hpp>
#include <cpu.hpp>

namespace {

auto irq_move(const TestContext&) noexcept -> bool {
    const bool enabled = arch::interrupts_enabled();
    {
        sync::Irq a;
        if (arch::interrupts_enabled()) return false;
        sync::Irq b{std::move(a)};
        if (a.active() || !b.active()) return false;
    }
    return arch::interrupts_enabled() == enabled;
}

auto lock_ownership(const TestContext&) noexcept -> bool {
    const bool enabled = arch::interrupts_enabled();
    sync::Spin a, b;
    {
        sync::Pair pair{a, b};
        if (!a.held() || !b.held()) return false;
        sync::TryLock attempt{a};
        if (attempt.owns_lock()) return false;
    }
    {
        sync::Pair same{a, a};
        if (!a.held()) return false;
    }
    {
        sync::Lock token{a, sync::try_lock};
        if (!token.owns_lock()) return false;
        sync::Lock moved{std::move(token)};
        if (token.owns_lock() || !moved.owns_lock()) return false;
        moved.restore();
        if (a.held()) return false;
    }
    sync::assert_unlocked();
    return !a.held() && !b.held() && arch::interrupts_enabled() == enabled;
}

} // namespace

void register_sync_tests(TestRegistry& registry) noexcept {
    (void)registry.add("sync", "IRQ move restores once", irq_move);
    (void)registry.add("sync", "ordered/try/moved locks own once", lock_ownership);
}
