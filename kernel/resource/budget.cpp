#include <resource/budget.hpp>

#include <libk/assert.hpp>
#include <base/types.hpp>
#include <limits>

namespace resource {

account::~account() noexcept { libk_assert(drained()); }

auto account::reserve(budget amount) noexcept -> bool {
    if (!available_.contains(amount)) return false;
    libk_assert(pending_ != std::numeric_limits<usize>::max());
    available_.memory -= amount.memory;
    available_.caps -= amount.caps;
    ++pending_;
    return true;
}

void account::commit() noexcept {
    libk_assert(pending_ != 0);
    --pending_;
}

void account::cancel(budget amount) noexcept {
    refund(amount);
    commit();
}

void account::refund(budget amount) noexcept {
    libk_assert(limit_.memory - available_.memory >= amount.memory);
    libk_assert(limit_.caps - available_.caps >= amount.caps);
    available_.memory += amount.memory;
    available_.caps += amount.caps;
}

} // namespace resource
