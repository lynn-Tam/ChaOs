#include <cpu/cpu_registry.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_runtime.hpp>
#include <cpu/cpu_set.hpp>
#include <libk/bits.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/memory.hpp>
#include <libk/utility.hpp>
#include <mm/pmm.hpp>

namespace kernel {

struct CpuRegistry::CpuRecordBlock final {
    static constexpr usize header_size =
        sizeof(CpuRecordBlock*) + sizeof(usize);
    static constexpr usize capacity =
        (kernel::mm::page_size - header_size) / sizeof(CpuDescriptor);
    static constexpr usize padding_size =
        kernel::mm::page_size - header_size - (capacity * sizeof(CpuDescriptor));

    CpuRecordBlock* next{};
    usize first_logical_id{};
    CpuDescriptor records[capacity]{};
    byte padding[padding_size]{};
};

CpuRegistry::Builder::Builder(
    libk::ManualLifetime<CpuRegistry>& storage,
    CpuRegistry& registry) noexcept
    : storage_(&storage), registry_(&registry) {}

CpuRegistry::Builder::Builder(Builder&& other) noexcept
    : storage_(libk::exchange(other.storage_, nullptr)),
      registry_(libk::exchange(other.registry_, nullptr)) {}

CpuRegistry::Builder::~Builder() noexcept {
    if (storage_ != nullptr) {
        storage_->reset();
    }
}

auto CpuRegistry::Builder::append(
    CpuHardwareId hardware_id,
    CpuAvailability availability) noexcept -> Result {
    if (registry_ == nullptr) {
        return libk::unexpected(Error::AlreadyFinished);
    }
    return registry_->append_record(hardware_id, availability);
}

auto CpuRegistry::Builder::finish() noexcept -> Result {
    if (registry_ == nullptr) {
        return libk::unexpected(Error::AlreadyFinished);
    }
    auto result = registry_->finish_records();
    if (!result) {
        return result;
    }
    storage_ = nullptr;
    registry_ = nullptr;
    return libk::expected();
}

auto CpuRegistry::begin(
    libk::ManualLifetime<CpuRegistry>& storage,
    kernel::mm::Pmm& pmm,
    CpuTopologySummary summary) noexcept -> BeginResult {
    if (summary.count == 0
        || summary.count > max_cpu_count
        || summary.boot_index >= summary.count) {
        return libk::unexpected(Error::InvalidTopology);
    }

    CpuRegistry& registry = storage.emplace(
        ConstructionKey{}, pmm, summary);
    auto allocated = registry.allocate_blocks();
    if (!allocated) {
        const Error error = allocated.error();
        storage.reset();
        return libk::unexpected(error);
    }
    return libk::expected(Builder{storage, registry});
}

CpuRegistry::CpuRegistry(
    [[maybe_unused]] ConstructionKey key,
    kernel::mm::Pmm& pmm,
    CpuTopologySummary summary) noexcept
    : storage_pages_(pmm.make_page_group()),
      count_(summary.count),
      boot_index_(summary.boot_index) {}

CpuRegistry::~CpuRegistry() noexcept {
    for (usize index = 0; index < appended_; ++index) {
        CpuDescriptor* const cpu = record_at(index);
        if (cpu->runtime_ != nullptr) {
            libk::destroy_at(cpu->runtime_);
            cpu->runtime_ = nullptr;
        }
    }
    for (CpuRecordBlock* block = first_block_; block != nullptr;) {
        CpuRecordBlock* const next = block->next;
        libk::destroy_at(block);
        block = next;
    }
}

auto CpuRegistry::allocate_blocks() noexcept -> Result {
    static_assert(CpuRecordBlock::capacity != 0);
    static_assert(sizeof(CpuRecordBlock) == kernel::mm::page_size);
    const usize block_count =
        (count_ + CpuRecordBlock::capacity - 1) / CpuRecordBlock::capacity;
    for (usize index = 0; index < block_count; ++index) {
        void* const allocation = allocate_storage(
            sizeof(CpuRecordBlock), alignof(CpuRecordBlock));
        if (allocation == nullptr) {
            return libk::unexpected(Error::MetadataAllocation);
        }
        auto* const block = libk::construct_at(
            static_cast<CpuRecordBlock*>(allocation));
        block->first_logical_id = index * CpuRecordBlock::capacity;
        if (last_block_ == nullptr) {
            first_block_ = block;
        } else {
            last_block_->next = block;
        }
        last_block_ = block;
    }
    return libk::expected();
}

auto CpuRegistry::extend_storage() noexcept -> bool {
    auto extension = storage_pages_.extend();
    auto next = extension.allocate_page();
    if (!next) {
        return false;
    }

    const kernel::mm::Page page = next.value();
    extension.commit();
    storage_cursor_ = reinterpret_cast<usize>(storage_pages_.bytes(page));
    const auto limit = libk::checked_add(storage_cursor_, kernel::mm::page_size);
    KASSERT(limit);
    storage_limit_ = *limit;
    return true;
}

auto CpuRegistry::allocate_storage(
    usize size,
    usize alignment) noexcept -> void* {
    KASSERT(size != 0);
    KASSERT(size <= kernel::mm::page_size);
    KASSERT(libk::has_single_bit(alignment));

    auto allocate_from_current = [this, size, alignment]() noexcept -> void* {
        const auto aligned =
            libk::checked_align_up(storage_cursor_, alignment);
        if (!aligned) {
            return nullptr;
        }
        const auto end = libk::checked_add(aligned.value(), size);
        if (!end || end.value() > storage_limit_) {
            return nullptr;
        }
        storage_cursor_ = end.value();
        return reinterpret_cast<void*>(aligned.value());
    };

    if (void* const allocation = allocate_from_current()) {
        return allocation;
    }
    if (!extend_storage()) {
        return nullptr;
    }
    return allocate_from_current();
}

auto CpuRegistry::append_record(
    CpuHardwareId hardware_id,
    CpuAvailability availability) noexcept -> Result {
    if (finished_ || appended_ >= count_) {
        return libk::unexpected(Error::InvalidTopology);
    }
    if (find_record(hardware_id) != nullptr) {
        return libk::unexpected(Error::DuplicateHardwareId);
    }

    CpuDescriptor* const cpu = slot_at(appended_);
    KASSERT(cpu != nullptr);
    cpu->logical_id_ = CpuId{appended_};
    cpu->hardware_id_ = hardware_id;
    cpu->availability_ = availability;

    CpuState initial_state = CpuState::Possible;
    switch (availability) {
    case CpuAvailability::Enabled:
        initial_state = CpuState::Present;
        break;
    case CpuAvailability::Disabled:
        initial_state = CpuState::Possible;
        break;
    case CpuAvailability::Failed:
        cpu->failure_ = CpuFailure::FirmwareReported;
        initial_state = CpuState::Failed;
        break;
    }
    cpu->state_.store<libk::MemoryOrder::Relaxed>(initial_state);
    ++appended_;
    return libk::expected();
}

auto CpuRegistry::finish_records() noexcept -> Result {
    if (finished_ || appended_ != count_) {
        return libk::unexpected(Error::InvalidTopology);
    }
    const CpuDescriptor* const boot = record_at(boot_index_);
    if (boot == nullptr || boot->availability_ != CpuAvailability::Enabled) {
        return libk::unexpected(Error::InvalidTopology);
    }
    finished_ = true;
    return libk::expected();
}

auto CpuRegistry::slot_at(usize index) noexcept -> CpuDescriptor* {
    if (index >= count_) {
        return nullptr;
    }
    for (CpuRecordBlock* block = first_block_;
         block != nullptr;
         block = block->next) {
        if (index >= block->first_logical_id
            && index < block->first_logical_id + CpuRecordBlock::capacity) {
            return &block->records[index - block->first_logical_id];
        }
    }
    return nullptr;
}

auto CpuRegistry::slot_at(usize index) const noexcept
    -> const CpuDescriptor* {
    if (index >= count_) {
        return nullptr;
    }
    for (const CpuRecordBlock* block = first_block_;
         block != nullptr;
         block = block->next) {
        if (index >= block->first_logical_id
            && index < block->first_logical_id + CpuRecordBlock::capacity) {
            return &block->records[index - block->first_logical_id];
        }
    }
    return nullptr;
}

auto CpuRegistry::record_at(usize index) noexcept -> CpuDescriptor* {
    return index < appended_ ? slot_at(index) : nullptr;
}

auto CpuRegistry::record_at(usize index) const noexcept
    -> const CpuDescriptor* {
    return index < appended_ ? slot_at(index) : nullptr;
}

auto CpuRegistry::find_record(CpuHardwareId hardware_id) noexcept
    -> CpuDescriptor* {
    for (usize index = 0; index < appended_; ++index) {
        CpuDescriptor* const cpu = record_at(index);
        if (cpu->hardware_id_ == hardware_id) {
            return cpu;
        }
    }
    return nullptr;
}

auto CpuRegistry::find_record(CpuHardwareId hardware_id) const noexcept
    -> const CpuDescriptor* {
    for (usize index = 0; index < appended_; ++index) {
        const CpuDescriptor* const cpu = record_at(index);
        if (cpu->hardware_id_ == hardware_id) {
            return cpu;
        }
    }
    return nullptr;
}

auto CpuRegistry::mutable_descriptor(CpuId id) noexcept -> CpuDescriptor* {
    if (!finished_ || id.raw >= appended_) {
        return nullptr;
    }
    return record_at(id.raw);
}

auto CpuRegistry::descriptor(CpuId id) const noexcept
    -> const CpuDescriptor* {
    if (!finished_ || id.raw >= appended_) {
        return nullptr;
    }
    return record_at(id.raw);
}

auto CpuRegistry::runtime(CpuId id) noexcept -> CpuRuntime* {
    CpuDescriptor* const cpu = mutable_descriptor(id);
    if (cpu == nullptr) {
        return nullptr;
    }
    const CpuState observed =
        cpu->state_.load<libk::MemoryOrder::Acquire>();
    switch (observed) {
    case CpuState::Prepared:
    case CpuState::Starting:
    case CpuState::Online:
    case CpuState::Failed:
        return cpu->runtime_;
    case CpuState::Possible:
    case CpuState::Present:
        return nullptr;
    }
    __builtin_unreachable();
}

auto CpuRegistry::runtime(CpuId id) const noexcept -> const CpuRuntime* {
    return const_cast<CpuRegistry*>(this)->runtime(id);
}

auto CpuRegistry::runtime_by_hardware_id(
    CpuHardwareId hardware_id) noexcept -> CpuRuntime* {
    if (!finished_) {
        return nullptr;
    }
    CpuDescriptor* const cpu = find_record(hardware_id);
    return cpu == nullptr ? nullptr : runtime(cpu->logical_id_);
}

void CpuRegistry::fail_unpublished(
    CpuDescriptor& cpu,
    CpuFailure failure) noexcept {
    KASSERT(cpu.runtime_ == nullptr);
    KASSERT(cpu.state_.load<libk::MemoryOrder::Relaxed>()
        == CpuState::Present);
    cpu.failure_ = failure;
    cpu.state_.store<libk::MemoryOrder::Release>(CpuState::Failed);
}

auto CpuRegistry::reserve_runtime(CpuId id) noexcept
    -> RuntimeTargetResult {
    CpuDescriptor* const cpu = mutable_descriptor(id);
    if (cpu == nullptr
        || cpu->availability_ != CpuAvailability::Enabled
        || cpu->state_.load<libk::MemoryOrder::Relaxed>()
            != CpuState::Present
        || cpu->runtime_ != nullptr) {
        return libk::unexpected(RuntimeReserveError::InvalidState);
    }

    static_assert(sizeof(CpuRuntime) <= kernel::mm::page_size);
    void* const allocation =
        allocate_storage(sizeof(CpuRuntime), alignof(CpuRuntime));
    if (allocation == nullptr) {
        fail_unpublished(*cpu, CpuFailure::MetadataAllocation);
        return libk::unexpected(RuntimeReserveError::MetadataAllocation);
    }
    return libk::expected(RuntimeTarget{cpu, allocation});
}

void CpuRegistry::fail_runtime(
    RuntimeTarget target,
    CpuFailure failure) noexcept {
    KASSERT(target.descriptor != nullptr);
    KASSERT(target.storage != nullptr);
    KASSERT(failure != CpuFailure::None);
    fail_unpublished(*target.descriptor, failure);
}

void CpuRegistry::publish_runtime(
    RuntimeTarget target,
    CpuRuntime& runtime_value) noexcept {
    CpuDescriptor& cpu = *target.descriptor;
    KASSERT(target.storage == &runtime_value);
    KASSERT(cpu.runtime_ == nullptr);
    KASSERT(cpu.state_.load<libk::MemoryOrder::Relaxed>()
        == CpuState::Present);
    KASSERT(runtime_value.owner_registry == this);
    KASSERT(runtime_value.local.descriptor == &cpu);
    KASSERT(runtime_value.stacks.init);
    KASSERT(runtime_value.stacks.irq);
    KASSERT(runtime_value.stacks.emergency);
    KASSERT(runtime_value.idle_thread);
    KASSERT(runtime_value.dispatcher_storage);
    KASSERT(runtime_value.start_context.ready());

    cpu.runtime_ = &runtime_value;
    // Publishes the stable association and every fully initialized runtime
    // member to registry observers and the eventual start launcher.
    cpu.state_.store<libk::MemoryOrder::Release>(CpuState::Prepared);
}

auto CpuRegistry::begin_start(CpuId id) noexcept -> bool {
    CpuDescriptor* const cpu = mutable_descriptor(id);
    if (cpu == nullptr) {
        return false;
    }
    CpuState expected = CpuState::Prepared;
    return cpu->state_.compare_exchange_strong<
        libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(expected, CpuState::Starting);
}

auto CpuRegistry::fail_start(CpuId id, CpuFailure failure) noexcept -> bool {
    if (failure == CpuFailure::None) {
        return false;
    }
    CpuDescriptor* const cpu = mutable_descriptor(id);
    if (cpu == nullptr) {
        return false;
    }
    CpuState state =
        cpu->state_.load<libk::MemoryOrder::Acquire>();
    if (state != CpuState::Prepared && state != CpuState::Starting) {
        return false;
    }
    cpu->failure_ = failure;
    return cpu->state_.compare_exchange_strong<
        libk::MemoryOrder::Release,
        libk::MemoryOrder::Relaxed>(state, CpuState::Failed);
}

auto CpuRegistry::publish_online(CpuRuntime& runtime_value) noexcept -> bool {
    const CpuDescriptor* const identity = runtime_value.local.descriptor;
    if (identity == nullptr) {
        return false;
    }
    CpuDescriptor* const cpu = mutable_descriptor(identity->logical_id_);
    if (cpu == nullptr || cpu != identity || cpu->runtime_ != &runtime_value) {
        return false;
    }
    if (runtime_value.local.current_thread() != &runtime_value.idle()
        || runtime_value.idle().state() != Thread::State::Running
        || arch::active_stack(runtime_value.local.arch_state)
            != runtime_value.idle().home_stack_top()) {
        return false;
    }
    CpuState expected = CpuState::Starting;
    // Publishes the running idle Thread, active stack and local entry state.
    return cpu->state_.compare_exchange_strong<
        libk::MemoryOrder::Release,
        libk::MemoryOrder::Relaxed>(expected, CpuState::Online);
}

auto CpuRegistry::snapshot() const noexcept -> CpuSnapshot {
    CpuSnapshot result{};
    for (usize index = 0; index < appended_; ++index) {
        switch (record_at(index)->state()) {
        case CpuState::Possible:
            ++result.possible;
            break;
        case CpuState::Present:
            ++result.present;
            break;
        case CpuState::Prepared:
            ++result.prepared;
            break;
        case CpuState::Starting:
            ++result.starting;
            break;
        case CpuState::Online:
            ++result.online;
            break;
        case CpuState::Failed:
            ++result.failed;
            break;
        }
    }
    return result;
}

} // namespace kernel
