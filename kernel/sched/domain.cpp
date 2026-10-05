#include <expected>
#include <sched/domain.hpp>

#include <libk/assert.hpp>
#include <base/types.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/memory.hpp>
#include <utility>
#include <sched/sc.hpp>
#include <sync.hpp>

namespace sched {

struct DomainCapacity::Block final {
    static constexpr usize header_size = sizeof(Block*) + sizeof(usize);
    static constexpr usize capacity =
        (mm::page_size - header_size) / sizeof(Record);

    Block* next{};
    usize first_cpu{};
    Record records[capacity]{};
};

auto DomainCapacity::create(mm::Pmm& pmm, usize cpu_count) noexcept
    -> CreateResult {
    static_assert(Block::capacity != 0);
    static_assert(sizeof(Block) <= mm::page_size);
    if (cpu_count == 0) {
        return std::unexpected(Error::InvalidCpuCount);
    }

    mm::PageGroup backing = pmm.group();
    Block* first{};
    Block* last{};
    const usize block_count =
        (cpu_count + Block::capacity - 1) / Block::capacity;
    for (usize index = 0; index < block_count; ++index) {
        auto allocation = backing.allocate();
        if (!allocation) {
            for (Block* block = first; block != nullptr;) {
                Block* const next = block->next;
                libk::destroy_at(block);
                block = next;
            }
            return std::unexpected(Error::OutOfMemory);
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
    }
    return (DomainCapacity{
        std::move(backing), first, cpu_count});
}

DomainCapacity::DomainCapacity(
    mm::PageGroup&& backing,
    Block* first,
    usize count) noexcept
    : backing_(std::move(backing)), first_(first), count_(count) {}

DomainCapacity::DomainCapacity(DomainCapacity&& other) noexcept
    : backing_(std::move(other.backing_)),
      first_(std::exchange(other.first_, nullptr)),
      count_(std::exchange(other.count_, 0)) {}

auto DomainCapacity::operator=(DomainCapacity&& other) noexcept
    -> DomainCapacity& {
    if (this != &other) {
        reset();
        backing_ = std::move(other.backing_);
        first_ = std::exchange(other.first_, nullptr);
        count_ = std::exchange(other.count_, 0);
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

Domain::Domain(
    DomainCapacity&& capacity,
    u32 limit,
    u32 reserved) noexcept
    : capacity_(std::move(capacity)) {
    libk_assert(capacity_.size() != 0);
    libk_assert(limit != 0 && limit <= share_scale);
    libk_assert(reserved < limit);
    for (usize index = 0; index < capacity_.size(); ++index) {
        auto* const record = capacity_.at(CpuId{index});
        libk_assert(record != nullptr);
        record->limit = limit;
        record->reserved = reserved;
        record->admitted = 0;
    }
}

Domain::~Domain() noexcept {
    for (usize index = 0; index < capacity_.size(); ++index) {
        libk_assert(capacity_.at(CpuId{index})->admitted == 0);
    }
}

auto Domain::share_of(const Sc& context) noexcept
    -> std::expected<u32, Error> {
    const auto config = context.config();
    const auto scaled = libk::checked_multiply(
        config.budget.ticks(), static_cast<u64>(share_scale));
    if (!scaled) {
        return std::unexpected(Error::ArithmeticOverflow);
    }
    const u64 quotient = *scaled / config.period.ticks();
    const u64 remainder = *scaled % config.period.ticks();
    const u64 rounded = quotient + (remainder != 0 ? 1 : 0);
    if (rounded == 0 || rounded > share_scale) {
        return std::unexpected(Error::InvalidContext);
    }
    return (static_cast<u32>(rounded));
}

auto Domain::admit(
    Sc& context,
    CpuId home_cpu) noexcept -> Result {
    if (!Sc::valid_config(context.config())) {
        return std::unexpected(Error::InvalidContext);
    }
    const auto share = share_of(context);
    if (!share) {
        return std::unexpected(share.error());
    }

    sync::Lock guard{lock_};
    auto* const record = capacity_.at(home_cpu);
    if (record == nullptr) {
        return std::unexpected(Error::InvalidCpu);
    }
    if (context.domain_ != nullptr) {
        return std::unexpected(Error::Busy);
    }
    const u64 total = static_cast<u64>(record->reserved)
        + record->admitted + share.value();
    if (total > record->limit) {
        return std::unexpected(Error::CapacityExceeded);
    }

    record->admitted += share.value();
    context.domain_ = this;
    context.home_cpu_ = home_cpu;
    return {};
}

auto Domain::unadmit(Sc& context) noexcept -> Result {
    const auto share = share_of(context);
    if (!share) {
        return std::unexpected(share.error());
    }

    sync::Lock guard{lock_};
    if (context.domain_ != this) {
        return std::unexpected(Error::InvalidContext);
    }
    if (context.active() || context.owner_) {
        return std::unexpected(Error::Busy);
    }
    auto* const record = capacity_.at(context.home_cpu_);
    libk_assert(record != nullptr);
    libk_assert(record->admitted >= share.value());
    record->admitted -= share.value();
    context.domain_ = nullptr;
    context.home_cpu_ = {};
    return {};
}

auto Domain::allows(CpuId cpu) const noexcept -> bool {
    // limit/reserved and table shape are immutable after construction. The
    // admitted aggregate is intentionally irrelevant to dispatch validation.
    const auto* const record = capacity_.at(cpu);
    return record != nullptr && record->limit > record->reserved;
}

} // namespace sched
