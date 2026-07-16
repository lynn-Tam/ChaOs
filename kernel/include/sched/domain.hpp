#pragma once

#include <core/types.hpp>
#include <cpu/topology.hpp>
#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <mm/pmm.hpp>

namespace kernel::sched {

class SchedulingContext;

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

    using CreateResult = libk::Expected<DomainCapacity, Error>;

    [[nodiscard]] static auto create(
        kernel::mm::Pmm& pmm,
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
        kernel::mm::OwnedPageGroup&& backing,
        Block* first,
        usize count) noexcept;
    void reset() noexcept;

    kernel::mm::OwnedPageGroup backing_;
    Block* first_{};
    usize count_{};
};

class SchedulingDomain final : private libk::noncopyable_nonmovable {
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

    using Result = libk::Expected<void, Error>;

    SchedulingDomain(
        DomainCapacity&& capacity,
        u32 limit,
        u32 reserved) noexcept;
    ~SchedulingDomain() noexcept;

    [[nodiscard]] auto admit(
        SchedulingContext& context,
        CpuId home_cpu) noexcept -> Result;
    [[nodiscard]] auto unadmit(SchedulingContext& context) noexcept -> Result;

    [[nodiscard]] auto allows(CpuId cpu) const noexcept -> bool;
    [[nodiscard]] auto cpu_count() const noexcept -> usize {
        return capacity_.size();
    }

private:
    [[nodiscard]] static auto share_of(const SchedulingContext& context) noexcept
        -> libk::Expected<u32, Error>;

    mutable libk::TicketSpinLock lock_{};
    DomainCapacity capacity_;
};

} // namespace kernel::sched
