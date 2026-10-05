#pragma once

#include <base/types.hpp>
#include <cpu/types.hpp>
#include <expected>
#include <libk/noncopyable.hpp>
#include <sync.hpp>
#include <mm/pmm.hpp>

namespace sched {

class Sc;

class DomainCapacity final : private libk::noncopyable {
public:
    struct Record final {
        u32 limit{};
        u32 reserved{};
        u32 admitted{};
    };

    enum class Error : u8 {
        InvalidCpuCount,
        OutOfMemory,
    };

    using CreateResult = std::expected<DomainCapacity, Error>;

    [[nodiscard]] static auto create(
        mm::Pmm& pmm,
        usize cpu_count) noexcept -> CreateResult;

    DomainCapacity(DomainCapacity&& other) noexcept;
    auto operator=(DomainCapacity&& other) noexcept -> DomainCapacity&;
    ~DomainCapacity() noexcept;

    [[nodiscard]] auto size() const noexcept -> usize { return count_; }
    [[nodiscard]] auto at(CpuId id) noexcept -> Record*;
    [[nodiscard]] auto at(CpuId id) const noexcept -> const Record*;

private:
    struct Block;

    DomainCapacity(
        mm::PageGroup&& backing,
        Block* first,
        usize count) noexcept;
    void reset() noexcept;

    mm::PageGroup backing_;
    Block* first_{};
    usize count_{};
};

class Domain final : private libk::noncopyable_nonmovable {
public:
    static constexpr u32 share_scale = 1'000'000;

    enum class Error : u8 {
        InvalidCapacity,
        InvalidContext,
        InvalidCpu,
        CapacityExceeded,
        ArithmeticOverflow,
        Busy,
    };

    using Result = std::expected<void, Error>;

    Domain(
        DomainCapacity&& capacity,
        u32 limit,
        u32 reserved) noexcept;
    ~Domain() noexcept;

    [[nodiscard]] auto admit(
        Sc& context,
        CpuId home_cpu) noexcept -> Result;
    [[nodiscard]] auto unadmit(Sc& context) noexcept -> Result;

    [[nodiscard]] auto allows(CpuId cpu) const noexcept -> bool;
    [[nodiscard]] auto cpu_count() const noexcept -> usize {
        return capacity_.size();
    }

private:
    [[nodiscard]] static auto share_of(const Sc& context) noexcept
        -> std::expected<u32, Error>;

    mutable sync::Spin
        lock_{};
    DomainCapacity capacity_;
};

} // namespace sched
