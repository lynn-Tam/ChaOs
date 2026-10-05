#pragma once

#include <base/types.hpp>

namespace resource {

// Capacity values have no dependency on how allocations are owned or closed.
struct budget final {
    u64 memory{};
    u64 caps{};

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return memory == 0 && caps == 0;
    }
    [[nodiscard]] constexpr auto contains(budget amount) const noexcept -> bool {
        return memory >= amount.memory && caps >= amount.caps;
    }
    friend constexpr auto operator==(budget, budget) noexcept -> bool = default;
};

enum class errc : u8 { invalid, closed, exhausted };

// Canonical capacity ledger. Its owner serializes all access and controls
// admission; committed charges remain outstanding until resources are reusable.
class account final {
public:
    explicit account(budget limit) noexcept : limit_(limit), available_(limit) {}
    account(const account&) = delete;
    auto operator=(const account&) -> account& = delete;
    ~account() noexcept;

    [[nodiscard]] auto limit() const noexcept -> budget { return limit_; }
    [[nodiscard]] auto available() const noexcept -> budget { return available_; }
    [[nodiscard]] auto pending() const noexcept -> usize { return pending_; }
    [[nodiscard]] auto drained() const noexcept -> bool {
        return pending_ == 0 && available_ == limit_;
    }
    [[nodiscard]] auto reserve(budget amount) noexcept -> bool;
    void commit() noexcept;
    void cancel(budget amount) noexcept;
    void refund(budget amount) noexcept;

private:
    budget limit_{};
    budget available_{};
    usize pending_{};
};

} // namespace resource
