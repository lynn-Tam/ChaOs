#include <expected>
#include <sched/domain.hpp>

#include <libk/assert.hpp>
#include <base/types.hpp>
#include <sched/sc.hpp>
#include <sync.hpp>

namespace sched {

Domain::Domain(usize cpu_count, u32 limit, u32 reserved) noexcept
    : count_(cpu_count), capacity_(limit - reserved) {
    libk_assert(count_ && count_ <= MaxCpus);
    libk_assert(reserved < limit && limit <= share_scale);
}

Domain::~Domain() noexcept {
    for (usize i = 0; i < count_; ++i) libk_assert(admitted_[i] == 0);
}

auto Domain::share_of(const Sc& sc) noexcept -> u32 {
    const auto c = sc.config();
    // Widen before scaling: every valid u64 budget/period ratio is representable.
    const auto n = static_cast<unsigned __int128>(c.budget.ticks()) * share_scale;
    return static_cast<u32>((n + c.period.ticks() - 1) / c.period.ticks());
}

auto Domain::admit(
    Sc& context,
    CpuId home_cpu) noexcept -> Result {
    const auto share = share_of(context);

    sync::Lock guard{lock_};
    if (!allows(home_cpu)) {
        return std::unexpected(Error::InvalidCpu);
    }
    if (context.domain_ != nullptr) {
        return std::unexpected(Error::Busy);
    }
    auto& used = admitted_[home_cpu.raw];
    if (share > capacity_ - used) return std::unexpected(Error::Quota);
    used += share;
    context.domain_ = this;
    context.home_cpu_ = home_cpu;
    return {};
}

auto Domain::unadmit(Sc& context) noexcept -> Result {
    const auto share = share_of(context);

    sync::Lock guard{lock_};
    if (context.domain_ != this) {
        return std::unexpected(Error::NotAdmitted);
    }
    if (context.active() || context.owner_) {
        return std::unexpected(Error::Busy);
    }
    auto& used = admitted_[context.home_cpu_.raw];
    libk_assert(used >= share);
    used -= share;
    context.domain_ = nullptr;
    context.home_cpu_ = {};
    return {};
}

auto Domain::allows(CpuId cpu) const noexcept -> bool {
    return cpu.raw < count_;
}

} // namespace sched
