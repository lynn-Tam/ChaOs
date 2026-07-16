#include <sched/domain.hpp>

#include <core/debug.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/memory.hpp>
#include <libk/utility.hpp>
#include <sched/context.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::sched {

struct DomainCapacity::Block final {
    static constexpr usize header_size = sizeof(Block*) + sizeof(usize);
    static constexpr usize capacity =
        (kernel::mm::page_size - header_size) / sizeof(Record);

    Block* next{};
    usize first_cpu{};
    Record records[capacity]{};
};

auto DomainCapacity::create(kernel::mm::Pmm& pmm, usize cpu_count) noexcept
    -> CreateResult {
    static_assert(Block::capacity != 0);
    static_assert(sizeof(Block) <= kernel::mm::page_size);
    if (cpu_count == 0) {
        return libk::unexpected(Error::InvalidCpuCount);
    }

    kernel::mm::OwnedPageGroup backing = pmm.make_page_group();
    Block* first{};
    Block* last{};
    const usize block_count =
        (cpu_count + Block::capacity - 1) / Block::capacity;
    for (usize index = 0; index < block_count; ++index) {
        auto extension = backing.extend();
        auto allocation = extension.allocate_page();
        if (!allocation) {
            for (Block* block = first; block != nullptr;) {
                Block* const next = block->next;
                libk::destroy_at(block);
                block = next;
            }
            return libk::unexpected(Error::OutOfMemory);
        }
        auto* const block = libk::construct_at(
            reinterpret_cast<Block*>(backing.bytes(allocation.value())));
        block->first_cpu = index * Block::capacity;
        if (last == nullptr) {
            first = block;
        } else {
            last->next = block;
        }
        last = block;
        extension.commit();
    }
    return libk::expected(DomainCapacity{
        libk::move(backing), first, cpu_count});
}

DomainCapacity::DomainCapacity(
    kernel::mm::OwnedPageGroup&& backing,
    Block* first,
    usize count) noexcept
    : backing_(libk::move(backing)), first_(first), count_(count) {}

DomainCapacity::DomainCapacity(DomainCapacity&& other) noexcept
    : backing_(libk::move(other.backing_)),
      first_(libk::exchange(other.first_, nullptr)),
      count_(libk::exchange(other.count_, 0)) {}

auto DomainCapacity::operator=(DomainCapacity&& other) noexcept
    -> DomainCapacity& {
    if (this != &other) {
        reset();
        backing_ = libk::move(other.backing_);
        first_ = libk::exchange(other.first_, nullptr);
        count_ = libk::exchange(other.count_, 0);
    }
    return *this;
}

DomainCapacity::~DomainCapacity() noexcept {
    reset();
}

void DomainCapacity::reset() noexcept {
    for (Block* block = first_; block != nullptr;) {
        Block* const next = block->next;
        libk::destroy_at(block);
        block = next;
    }
    first_ = nullptr;
    count_ = 0;
    backing_.reset();
}

auto DomainCapacity::at(CpuId id) noexcept -> Record* {
    if (id.raw >= count_) {
        return nullptr;
    }
    for (Block* block = first_; block != nullptr; block = block->next) {
        if (id.raw >= block->first_cpu
            && id.raw < block->first_cpu + Block::capacity) {
            return &block->records[id.raw - block->first_cpu];
        }
    }
    return nullptr;
}

auto DomainCapacity::at(CpuId id) const noexcept -> const Record* {
    return const_cast<DomainCapacity*>(this)->at(id);
}

SchedulingDomain::SchedulingDomain(
    DomainCapacity&& capacity,
    u32 limit,
    u32 reserved) noexcept
    : capacity_(libk::move(capacity)) {
    KASSERT(capacity_.size() != 0);
    KASSERT(limit != 0 && limit <= share_scale);
    KASSERT(reserved < limit);
    for (usize index = 0; index < capacity_.size(); ++index) {
        auto* const record = capacity_.at(CpuId{index});
        KASSERT(record != nullptr);
        record->limit = limit;
        record->reserved = reserved;
        record->admitted = 0;
    }
}

SchedulingDomain::~SchedulingDomain() noexcept {
    for (usize index = 0; index < capacity_.size(); ++index) {
        KASSERT(capacity_.at(CpuId{index})->admitted == 0);
    }
}

auto SchedulingDomain::share_of(const SchedulingContext& context) noexcept
    -> libk::Expected<u32, Error> {
    const auto config = context.config();
    const auto scaled = libk::checked_multiply(
        config.budget.ticks(), static_cast<u64>(share_scale));
    if (!scaled) {
        return libk::unexpected(Error::ArithmeticOverflow);
    }
    const u64 quotient = *scaled / config.period.ticks();
    const u64 remainder = *scaled % config.period.ticks();
    const u64 rounded = quotient + (remainder != 0 ? 1 : 0);
    if (rounded == 0 || rounded > share_scale) {
        return libk::unexpected(Error::InvalidContext);
    }
    return libk::expected(static_cast<u32>(rounded));
}

auto SchedulingDomain::admit(
    SchedulingContext& context,
    CpuId home_cpu) noexcept -> Result {
    if (!SchedulingContext::valid_config(context.config())) {
        return libk::unexpected(Error::InvalidContext);
    }
    const auto share = share_of(context);
    if (!share) {
        return libk::unexpected(share.error());
    }

    kernel::sync::IrqLockGuard guard{lock_};
    auto* const record = capacity_.at(home_cpu);
    if (record == nullptr) {
        return libk::unexpected(Error::InvalidCpu);
    }
    if (context.domain_ != nullptr) {
        return libk::unexpected(Error::Busy);
    }
    const u64 total = static_cast<u64>(record->reserved)
        + record->admitted + share.value();
    if (total > record->limit) {
        return libk::unexpected(Error::CapacityExceeded);
    }

    record->admitted += share.value();
    context.domain_ = this;
    context.home_cpu_ = home_cpu;
    return libk::expected();
}

auto SchedulingDomain::unadmit(SchedulingContext& context) noexcept -> Result {
    const auto share = share_of(context);
    if (!share) {
        return libk::unexpected(share.error());
    }

    kernel::sync::IrqLockGuard guard{lock_};
    if (context.domain_ != this) {
        return libk::unexpected(Error::InvalidContext);
    }
    if (context.active_cpu_ || context.binding_) {
        return libk::unexpected(Error::Busy);
    }
    auto* const record = capacity_.at(context.home_cpu_);
    KASSERT(record != nullptr);
    KASSERT(record->admitted >= share.value());
    record->admitted -= share.value();
    context.domain_ = nullptr;
    context.home_cpu_ = {};
    return libk::expected();
}

auto SchedulingDomain::allows(CpuId cpu) const noexcept -> bool {
    // limit/reserved and table shape are immutable after construction. The
    // admitted aggregate is intentionally irrelevant to dispatch validation.
    const auto* const record = capacity_.at(cpu);
    return record != nullptr && record->limit > record->reserved;
}

} // namespace kernel::sched
