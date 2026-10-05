#include <expected>
#include <panic.hpp>
#include <trace.hpp>
#include <test/test.hpp>

#include <arch/ipi.hpp>
#include <boot/cpu_topology.hpp>
#include <cpu/setup.hpp>
#include <cpu/registry.hpp>
#include <cpu/runtime.hpp>
#include <libk/manual_lifetime.hpp>
#include <utility>
#include <mm/pmm.hpp>
#include <mm/kspace.hpp>
#include <boot/link.hpp>
#include <object/pool.hpp>
#include <object/group.hpp>
#include <sched/sc.hpp>
#include <sched/domain.hpp>
#include <sched/remote_queue.hpp>
#include <time/clock.hpp>

#include <mm/table.hpp>

namespace {

constexpr size_t cpu_test_pages = 512;
alignas(mm::page_size) uint8_t cpu_test_ram[cpu_test_pages * mm::page_size]{};
constinit libk::ManualLifetime<mm::Pmm> cpu_test_pmm{};
constinit libk::delegate<void() noexcept> cpu_test_notify{};
constinit libk::ManualLifetime<object::pool<Thread>> cpu_test_threads{};

constinit libk::ManualLifetime<time::Clock> cpu_test_clock{};
constinit libk::ManualLifetime<CpuRegistry> cpu_test_registry{};
constinit libk::ManualLifetime<mm::KSpace> cpu_test_kernel{};

void unused_idle_entry(void*) noexcept {}

class CpuStorageGuard final {
public:
    CpuStorageGuard() noexcept { reset(); }
    ~CpuStorageGuard() noexcept { reset(); }

    [[nodiscard]] auto initialize(size_t pages = cpu_test_pages) noexcept
        -> bool {
        if (pages == 0 || pages > cpu_test_pages) {
            return false;
        }
        const auto physical = kernel_phys(mm::Virt{
            reinterpret_cast<uintptr_t>(cpu_test_ram)});
        if (!physical) {
            return false;
        }
        const auto first = mm::Page::from_base(*physical);
        if (!first) {
            return false;
        }
        mm::RegionList map{};
        if (!map.try_emplace_back(mm::Region{
                mm::Pages{*first, pages},
                mm::Region::Kind::Ram})) {
            return false;
        }
        if (!mm::Pmm::initialize_in(
                cpu_test_pmm, std::move(map), mm::DirectMap::Layout{
                .physical_base = mm::Phys{
                    physical->raw()},
                .virtual_base = mm::Virt{
                    reinterpret_cast<uintptr_t>(cpu_test_ram)},
                .window_size = sizeof(cpu_test_ram),
            })) {
            return false;
        }
        (void)cpu_test_threads.emplace(*cpu_test_pmm, cpu_test_notify);
        [[maybe_unused]] auto& clock = cpu_test_clock.emplace(10'000'000);
        return true;
    }

private:
    static auto reset() noexcept -> void {
        cpu_test_registry.reset();
        cpu_test_threads.reset();
        cpu_test_clock.reset();
        cpu_test_kernel.reset();
        cpu_test_pmm.reset();
    }
};

[[nodiscard]] auto make_test_root() noexcept -> mm::KSpace* {
    auto builder = mm::PageTable::create(*cpu_test_pmm, mm::PageTable::Kind::Kernel);
    if (!builder) {
        return nullptr;
    }
    mm::PageTable root = std::move(builder).value();
    if (!mm::KSpace::adopt_in(
            cpu_test_kernel, *cpu_test_pmm, std::move(root))) {
        return nullptr;
    }
    return &*cpu_test_kernel;
}

constexpr uint32_t address_cells_offset = 0;
constexpr uint32_t size_cells_offset =
    address_cells_offset + sizeof("#address-cells");
constexpr uint32_t device_type_offset =
    size_cells_offset + sizeof("#size-cells");
constexpr uint32_t reg_offset =
    device_type_offset + sizeof("device_type");
constexpr uint32_t status_offset = reg_offset + sizeof("reg");
constexpr char property_names[] =
    "#address-cells\0"
    "#size-cells\0"
    "device_type\0"
    "reg\0"
    "status\0";

class FdtStructureWriter final {
public:
    auto reset() noexcept -> void {
        size_ = 0;
        valid_ = true;
    }

    auto begin_node(const char* name) noexcept -> void {
        be32(1U);
        cstring(name);
        align4();
    }

    auto end_node() noexcept -> void { be32(2U); }
    auto finish() noexcept -> void { be32(9U); }

    auto cell_property(uint32_t name_offset, uint32_t value) noexcept -> void {
        property_header(name_offset, sizeof(uint32_t));
        be32(value);
    }

    auto string_property(uint32_t name_offset, const char* value) noexcept
        -> void {
        size_t length = 1;
        while (value[length - 1] != '\0') {
            ++length;
        }
        property_header(name_offset, length);
        for (size_t index = 0; index < length; ++index) {
            byte(static_cast<uint8_t>(value[index]));
        }
        align4();
    }

    auto reg64(uint64_t hardware_id) noexcept -> void {
        property_header(reg_offset, 2 * sizeof(uint32_t));
        be32(static_cast<uint32_t>(hardware_id >> 32));
        be32(static_cast<uint32_t>(hardware_id));
    }

    auto reg64_pair(uint64_t first, uint64_t second) noexcept -> void {
        property_header(reg_offset, 4 * sizeof(uint32_t));
        be32(static_cast<uint32_t>(first >> 32));
        be32(static_cast<uint32_t>(first));
        be32(static_cast<uint32_t>(second >> 32));
        be32(static_cast<uint32_t>(second));
    }

    [[nodiscard]] auto view() noexcept
        -> std::expected<Fdt, FdtError> {
        if (!valid_) {
            return std::unexpected(FdtError::InvalidStructure);
        }
        constexpr size_t header_size = 40;
        constexpr size_t reservations_size = 16;
        constexpr size_t structure_offset = header_size + reservations_size;
        const size_t strings_offset = structure_offset + size_;
        const size_t total = strings_offset + sizeof(property_names);
        if (total > sizeof(blob_)) {
            return std::unexpected(FdtError::InvalidStructure);
        }
        auto write32 = [this](size_t offset, uint32_t value) {
            blob_[offset] = static_cast<uint8_t>(value >> 24);
            blob_[offset + 1] = static_cast<uint8_t>(value >> 16);
            blob_[offset + 2] = static_cast<uint8_t>(value >> 8);
            blob_[offset + 3] = static_cast<uint8_t>(value);
        };
        write32(0, 0xd00dfeed);
        write32(4, static_cast<uint32_t>(total));
        write32(8, static_cast<uint32_t>(structure_offset));
        write32(12, static_cast<uint32_t>(strings_offset));
        write32(16, static_cast<uint32_t>(header_size));
        write32(20, 17);
        write32(24, 16);
        write32(28, 0);
        write32(32, sizeof(property_names));
        write32(36, static_cast<uint32_t>(size_));
        for (size_t index = header_size; index < structure_offset; ++index) {
            blob_[index] = 0;
        }
        for (size_t index = 0; index < size_; ++index) {
            blob_[structure_offset + index] = bytes_[index];
        }
        for (size_t index = 0; index < sizeof(property_names); ++index) {
            blob_[strings_offset + index] = property_names[index];
        }
        return Fdt::open(blob_);
    }

private:
    auto property_header(uint32_t name_offset, size_t length) noexcept -> void {
        be32(3U);
        be32(static_cast<uint32_t>(length));
        be32(name_offset);
    }

    auto byte(uint8_t value) noexcept -> void {
        if (size_ >= sizeof(bytes_)) {
            valid_ = false;
            return;
        }
        bytes_[size_++] = value;
    }

    auto be32(uint32_t value) noexcept -> void {
        byte(static_cast<uint8_t>(value >> 24));
        byte(static_cast<uint8_t>(value >> 16));
        byte(static_cast<uint8_t>(value >> 8));
        byte(static_cast<uint8_t>(value));
    }

    auto cstring(const char* value) noexcept -> void {
        do {
            byte(static_cast<uint8_t>(*value));
        } while (*value++ != '\0');
    }

    auto align4() noexcept -> void {
        while (size_ % sizeof(uint32_t) != 0) {
            byte(0);
        }
    }

    uint8_t bytes_[32768]{};
    alignas(8) uint8_t blob_[33024]{};
    size_t size_{};
    bool valid_{true};
};

constinit FdtStructureWriter fdt_writer{};
constinit CpuHandoff cpu_handoff_storage{};

[[nodiscard]] auto parse_cpu_tree(
    const std::expected<Fdt, FdtError>& view,
    CpuHwId boot_cpu) noexcept
    -> std::expected<CpuHandoff*,
        CpuTopologyError> {
    cpu_handoff_storage.cpus.clear();
    cpu_handoff_storage.boot_index = 0;
    if (!view) {
        return std::unexpected(CpuTopologyError::InvalidCpuNode);
    }
    auto parsed = parse_fdt_cpus(
        view.value(), boot_cpu, cpu_handoff_storage);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return (&cpu_handoff_storage);
}

auto begin_cpu_tree(
    uint32_t address_cells = 2,
    uint32_t size_cells = 0) noexcept -> void {
    fdt_writer.reset();
    fdt_writer.begin_node("");
    fdt_writer.begin_node("cpus");
    fdt_writer.cell_property(address_cells_offset, address_cells);
    fdt_writer.cell_property(size_cells_offset, size_cells);
}

auto add_cpu(
    const char* node_name,
    uint64_t hardware_id,
    const char* status = nullptr,
    bool include_reg = true) noexcept -> void {
    fdt_writer.begin_node(node_name);
    fdt_writer.string_property(device_type_offset, "cpu");
    if (include_reg) {
        fdt_writer.reg64(hardware_id);
    }
    if (status != nullptr) {
        fdt_writer.string_property(status_offset, status);
    }
    fdt_writer.end_node();
}

[[nodiscard]] auto finish_cpu_tree() noexcept -> std::expected<Fdt, FdtError> {
    fdt_writer.end_node();
    fdt_writer.end_node();
    fdt_writer.finish();
    return fdt_writer.view();
}

bool test_sparse_inventory_and_statuses(const TestContext&) noexcept {
    begin_cpu_tree();
    add_cpu("cpu@0", 0, "disabled");
    add_cpu("cpu@100", 256, "okay");
    add_cpu("cpu@400", 1024, "fail-selftest");
    const auto view = finish_cpu_tree();

    const auto summary = parse_cpu_tree(
        view, CpuHwId{256});
    if (!summary
        || summary.value()->cpus.size() != 3
        || summary.value()->boot_index != 1) {
        return false;
    }

    CpuStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }
    auto begun = CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        summary.value()->summary());
    if (!begun) {
        return false;
    }
    auto builder = std::move(begun).value();
    for (const BootCpu& cpu : summary.value()->cpus) {
        if (!builder.append(cpu.hardware_id, cpu.availability)) {
            return false;
        }
    }
    if (!builder.finish()) {
        return false;
    }

    const auto* cpu0 = cpu_test_registry->descriptor(CpuId{0});
    const auto* cpu1 = cpu_test_registry->descriptor(CpuId{1});
    const auto* cpu2 = cpu_test_registry->descriptor(CpuId{2});
    if (cpu0 == nullptr || cpu1 == nullptr || cpu2 == nullptr) {
        return false;
    }
    const auto failure = cpu2->failure();
    return cpu0->hardware_id() == CpuHwId{0}
        && cpu0->availability() == CpuAvail::Disabled
        && cpu1->hardware_id() == CpuHwId{256}
        && cpu1->availability() == CpuAvail::Enabled
        && cpu2->hardware_id() == CpuHwId{1024}
        && cpu2->availability() == CpuAvail::Failed
        && cpu0->state() == CpuState::Possible
        && cpu1->state() == CpuState::Present
        && cpu2->state() == CpuState::Failed
        && failure
        && *failure == CpuFailure::FirmwareReported;
}

bool test_malformed_cpu_nodes_are_rejected(const TestContext&) noexcept {
    fdt_writer.reset();
    fdt_writer.begin_node("");
    fdt_writer.end_node();
    fdt_writer.finish();
    const auto missing_cpus = parse_cpu_tree(
        fdt_writer.view(), CpuHwId{0});
    if (missing_cpus
        || missing_cpus.error()
            != CpuTopologyError::MissingCpusNode) {
        return false;
    }

    begin_cpu_tree();
    const auto zero_cpus = parse_cpu_tree(
        finish_cpu_tree(), CpuHwId{0});
    if (zero_cpus
        || zero_cpus.error()
            != CpuTopologyError::BootCpuMissing) {
        return false;
    }

    begin_cpu_tree(3, 0);
    add_cpu("cpu@0", 0);
    const auto bad_cells = parse_cpu_tree(
        finish_cpu_tree(), CpuHwId{0});
    if (bad_cells
        || bad_cells.error()
            != CpuTopologyError::InvalidAddressCells) {
        return false;
    }

    begin_cpu_tree(2, 1);
    add_cpu("cpu@0", 0);
    const auto bad_size_cells = parse_cpu_tree(
        finish_cpu_tree(), CpuHwId{0});
    if (bad_size_cells
        || bad_size_cells.error()
            != CpuTopologyError::InvalidSizeCells) {
        return false;
    }

    begin_cpu_tree();
    add_cpu("cpu@0", 0, nullptr, false);
    const auto missing_reg = parse_cpu_tree(
        finish_cpu_tree(), CpuHwId{0});
    if (missing_reg
        || missing_reg.error()
            != CpuTopologyError::MissingReg) {
        return false;
    }

    begin_cpu_tree();
    fdt_writer.begin_node("cpu@0");
    fdt_writer.reg64(0);
    fdt_writer.end_node();
    const auto missing_type = parse_cpu_tree(
        finish_cpu_tree(), CpuHwId{0});
    if (missing_type
        || missing_type.error()
            != CpuTopologyError::InvalidCpuNode) {
        return false;
    }

    begin_cpu_tree();
    fdt_writer.begin_node("cpu@0");
    fdt_writer.string_property(device_type_offset, "cpu");
    fdt_writer.reg64(0);
    fdt_writer.reg64(1);
    fdt_writer.end_node();
    const auto duplicate_reg = parse_cpu_tree(
        finish_cpu_tree(), CpuHwId{0});
    if (duplicate_reg
        || duplicate_reg.error()
            != CpuTopologyError::InvalidReg) {
        return false;
    }

    begin_cpu_tree();
    fdt_writer.begin_node("cpu@0");
    fdt_writer.string_property(device_type_offset, "cpu");
    fdt_writer.reg64_pair(0, 1);
    fdt_writer.end_node();
    const auto multi_tuple_reg = parse_cpu_tree(
        finish_cpu_tree(), CpuHwId{0});
    if (multi_tuple_reg
        || multi_tuple_reg.error()
            != CpuTopologyError::InvalidReg) {
        return false;
    }

    begin_cpu_tree();
    add_cpu("cpu@0", 0, "mystery");
    const auto bad_status = parse_cpu_tree(
        finish_cpu_tree(), CpuHwId{0});
    return !bad_status
        && bad_status.error()
            == CpuTopologyError::InvalidStatus;
}

bool test_boot_hart_match_is_strict(const TestContext&) noexcept {
    begin_cpu_tree();
    add_cpu("cpu@0", 0, "disabled");
    const auto disabled = parse_cpu_tree(
        finish_cpu_tree(), CpuHwId{0});
    if (disabled
        || disabled.error()
            != CpuTopologyError::BootCpuUnavailable) {
        return false;
    }

    begin_cpu_tree();
    add_cpu("cpu@0", 0);
    const auto missing = parse_cpu_tree(
        finish_cpu_tree(), CpuHwId{7});
    if (missing
        || missing.error()
            != CpuTopologyError::BootCpuMissing) {
        return false;
    }

    begin_cpu_tree();
    add_cpu("cpu@0", 0);
    add_cpu("cpu@00", 0);
    const auto duplicate = parse_cpu_tree(
        finish_cpu_tree(), CpuHwId{0});
    return !duplicate
        && duplicate.error()
            == CpuTopologyError::DuplicateBootCpu;
}

bool test_builder_rejects_mismatch_and_duplicate_id(const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }
    auto begun = CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        CpuTopo{2, 0});
    if (!begun) {
        return false;
    }
    auto builder = std::move(begun).value();
    if (!builder.append(
            CpuHwId{0},
            CpuAvail::Enabled)) {
        return false;
    }
    const auto duplicate = builder.append(
        CpuHwId{0},
        CpuAvail::Disabled);
    if (duplicate
        || duplicate.error()
            != CpuRegistry::Error::DuplicateHardwareId) {
        return false;
    }
    const auto incomplete = builder.finish();
    return !incomplete
        && incomplete.error()
            == CpuRegistry::Error::InvalidTopology;
}

[[nodiscard]] auto registry_accepts_count(usize count) noexcept -> bool {
    CpuStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }
    auto begun = CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        CpuTopo{count, 0});
    if (!begun) {
        return false;
    }
    auto builder = std::move(begun).value();
    for (usize index = 0; index < count; ++index) {
        if (!builder.append(
                CpuHwId{index * 17},
                CpuAvail::Enabled)) {
            return false;
        }
    }
    if (!builder.finish() || cpu_test_registry->count() != count) {
        return false;
    }
    const auto* last = cpu_test_registry->descriptor(CpuId{count - 1});
    return last != nullptr
        && last->hardware_id()
            == CpuHwId{(count - 1) * 17};
}

bool test_registry_crosses_legacy_array_thresholds(const TestContext&) noexcept {
    constexpr usize counts[] = {26, 27, 28, 29, 128, 129};
    for (const usize count : counts) {
        if (!registry_accepts_count(count)) {
            return false;
        }
    }
    return true;
}

bool test_registry_rejects_unbounded_logical_ids(const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize(32)) {
        return false;
    }
    const auto begun = CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        CpuTopo{100000, 0});
    return !begun
        && begun.error()
            == CpuRegistry::Error::InvalidTopology;
}

[[nodiscard]] auto prepare_failure_with_budget(
    bool warm_stack_pool,
    usize remaining_pages,
    CpuSetup::Error expected_error,
    CpuFailure expected_failure) noexcept -> bool {
    CpuStorageGuard storage{};
    if (!storage.initialize(384)) {
        return false;
    }
    auto begun = CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        CpuTopo{1, 0});
    if (!begun) {
        return false;
    }
    auto builder = std::move(begun).value();
    if (!builder.append(
            CpuHwId{0},
            CpuAvail::Enabled)
        || !builder.finish()) {
        return false;
    }

    auto* const activation = make_test_root();
    if (activation == nullptr) {
        return false;
    }
    if (warm_stack_pool) {
        auto first = mm::Stack::create(*activation);
        auto second = mm::Stack::create(*activation);
        auto third = mm::Stack::create(*activation);
        auto fourth = mm::Stack::create(*activation);
        if (!first || !second || !third || !fourth) {
            return false;
        }
    }

    auto withheld = cpu_test_pmm->group();
    if (cpu_test_pmm->free_page_count() < remaining_pages) {
        return false;
    }
    {
        auto pending = withheld.owner().group();
        while (cpu_test_pmm->free_page_count() > remaining_pages) {
            if (!pending.allocate()) {
                return false;
            }
        }
        withheld.append(std::move(pending));
    }

    CpuSetup provisioner{
        *cpu_test_registry,
        *cpu_test_pmm,
        *cpu_test_threads,
        *cpu_test_clock};
    const auto prepared = provisioner.prepare(
        CpuId{0},
        *activation,
        unused_idle_entry);
    const auto* const cpu =
        cpu_test_registry->descriptor(CpuId{0});
    return !prepared
        && prepared.error() == expected_error
        && cpu != nullptr
        && cpu_test_registry->runtime(CpuId{0}) == nullptr
        && cpu->state() == CpuState::Failed
        && cpu->failure()
        && *cpu->failure() == expected_failure
        && cpu_test_pmm->verify_invariants();
}

bool test_prepare_resource_exhaustion_is_unpublished(
    const TestContext&) noexcept {
    return prepare_failure_with_budget(
               false,
               1,
               CpuSetup::Error::StackAllocation,
               CpuFailure::StackAllocation)
        && prepare_failure_with_budget(
               true,
               1,
               CpuSetup::Error::MetadataAllocation,
               CpuFailure::MetadataAllocation)
        && prepare_failure_with_budget(
               true,
               2 + usize(trace::enabled()),
               CpuSetup::Error::ObjectAllocation,
               CpuFailure::ObjectAllocation);
}

bool test_secondary_prepare_failure_preserves_prepared_boot_cpu(
    const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize(384)) {
        return false;
    }
    auto begun = CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        CpuTopo{2, 0});
    if (!begun) {
        return false;
    }
    auto builder = std::move(begun).value();
    if (!builder.append(
            CpuHwId{0},
            CpuAvail::Enabled)
        || !builder.append(
            CpuHwId{1},
            CpuAvail::Enabled)
        || !builder.finish()) {
        return false;
    }
    const auto activation = make_test_root();
    CpuSetup provisioner{
        *cpu_test_registry,
        *cpu_test_pmm,
        *cpu_test_threads,
        *cpu_test_clock};
    if (!activation
        || !provisioner.prepare(
            CpuId{0},
            *activation,
            unused_idle_entry)) {
        return false;
    }
    CpuRuntime* const boot =
        cpu_test_registry->runtime(CpuId{0});
    auto withheld = cpu_test_pmm->group();
    {
        auto pending = withheld.owner().group();
        while (cpu_test_pmm->free_page_count() > 1) {
            if (!pending.allocate()) {
                return false;
            }
        }
        withheld.append(std::move(pending));
    }
    const auto secondary = provisioner.prepare(
        CpuId{1},
        *activation,
        unused_idle_entry);
    const auto* const boot_descriptor =
        cpu_test_registry->descriptor(CpuId{0});
    const auto* const secondary_descriptor =
        cpu_test_registry->descriptor(CpuId{1});
    return !secondary
        && secondary.error()
            == CpuSetup::Error::StackAllocation
        && boot != nullptr
        && cpu_test_registry->runtime(CpuId{0}) == boot
        && boot_descriptor->state() == CpuState::Prepared
        && secondary_descriptor->state() == CpuState::Failed
        && cpu_test_registry->runtime(CpuId{1}) == nullptr
        && cpu_test_pmm->verify_invariants();
}

bool test_prepare_publishes_descriptor_borrow(const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize(384)) {
        return false;
    }
    auto begun = CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        CpuTopo{1, 0});
    if (!begun) {
        return false;
    }
    auto builder = std::move(begun).value();
    if (!builder.append(
            CpuHwId{42},
            CpuAvail::Enabled)
        || !builder.finish()) {
        return false;
    }
    const auto activation = make_test_root();
    if (!activation) {
        return false;
    }
    {
        CpuSetup provisioner{
            *cpu_test_registry,
            *cpu_test_pmm,
            *cpu_test_threads,
            *cpu_test_clock};
        if (!provisioner.prepare(
            CpuId{0},
            *activation,
            unused_idle_entry)) {
            return false;
        }
    }

    const auto* descriptor =
        cpu_test_registry->descriptor(CpuId{0});
    const auto* runtime = cpu_test_registry->runtime(CpuId{0});
    return descriptor != nullptr
        && descriptor->state() == CpuState::Prepared
        && runtime != nullptr
        && runtime->owner_registry == &*cpu_test_registry
        && runtime->local.descriptor == descriptor
        && runtime->local.current_thread() == nullptr
        && arch::active_stack(runtime->local.arch_state) == 0
        && runtime->panic != nullptr
        && arch::panic_slot(runtime->local.arch_state)
            == runtime->panic
        && arch::emergency_stack(runtime->local.arch_state)
            == runtime->emergency_stack->top()
        && runtime->idle().state() == Thread::State::Prepared
        && runtime->idle().home_stack_top() != 0
        && (runtime->idle().home_stack_top() & 0xfU) == 0
        && runtime->start_context.ready()
        && cpu_test_registry->runtime_by_hardware_id(
            CpuHwId{42}) == runtime;
}

bool test_lifecycle_start_failure_and_snapshot_use_canonical_states(
    const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize(384)) {
        return false;
    }
    auto begun = CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        CpuTopo{2, 0});
    if (!begun) {
        return false;
    }
    auto builder = std::move(begun).value();
    if (!builder.append(
            CpuHwId{4},
            CpuAvail::Enabled)
        || !builder.append(
            CpuHwId{19},
            CpuAvail::Enabled)
        || !builder.finish()) {
        return false;
    }
    const auto activation = make_test_root();
    CpuSetup provisioner{
        *cpu_test_registry,
        *cpu_test_pmm,
        *cpu_test_threads,
        *cpu_test_clock};
    if (!activation
        || !provisioner.prepare(
            CpuId{0},
            *activation,
            unused_idle_entry)
        || !provisioner.prepare(
            CpuId{1},
            *activation,
            unused_idle_entry)) {
        return false;
    }

    auto* const first = cpu_test_registry->runtime(CpuId{0});
    auto* const second = cpu_test_registry->runtime(CpuId{1});
    if (first == nullptr || second == nullptr || first == second
        || &first->local == &second->local
        || first->init_stack->top() == second->init_stack->top()
        || &first->idle() == &second->idle()
        || first->idle().home_stack_top() == second->idle().home_stack_top()
        || &first->start_context == &second->start_context) {
        return false;
    }

    if (!cpu_test_registry->begin_start(CpuId{0})
        || cpu_test_registry->begin_start(CpuId{0})
        || !cpu_test_registry->fail_start(
            CpuId{1},
            CpuFailure::HsmUnavailable)
        || cpu_test_registry->publish_online(*first)) {
        return false;
    }

    const auto* const failed =
        cpu_test_registry->descriptor(CpuId{1});
    const CpuSnapshot snapshot = cpu_test_registry->snapshot();
    return failed != nullptr
        && failed->state() == CpuState::Failed
        && failed->failure()
        && *failed->failure() == CpuFailure::HsmUnavailable
        && cpu_test_registry->runtime(CpuId{1}) == second
        && snapshot.starting == 1
        && snapshot.failed == 1
        && snapshot.possible == 0
        && snapshot.present == 0
        && snapshot.prepared == 0
        && snapshot.online == 0;
}

bool test_shootdown_ack_controls_retirement(const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize(384)) {
        return false;
    }
    auto begun = CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        CpuTopo{2, 0});
    if (!begun) {
        return false;
    }
    auto builder = std::move(begun).value();
    if (!builder.append(
            CpuHwId{0},
            CpuAvail::Enabled)
        || !builder.append(
            CpuHwId{4096},
            CpuAvail::Enabled)
        || !builder.finish()) {
        return false;
    }
    const auto root = make_test_root();
    if (!root) {
        return false;
    }
    mm::Tlb& translation = root->tlb();
    CpuSetup provisioner{
        *cpu_test_registry,
        *cpu_test_pmm,
        *cpu_test_threads,
        *cpu_test_clock};
    if (!provisioner.prepare(
            CpuId{0}, *root, unused_idle_entry)
        || !provisioner.prepare(
            CpuId{1}, *root, unused_idle_entry)) {
        return false;
    }
    CpuRuntime* const remote =
        cpu_test_registry->runtime(CpuId{1});
    auto page_result = cpu_test_pmm->allocate_page();
    if (remote == nullptr || !page_result) {
        return false;
    }
    const mm::Page page = page_result.value().page();
    mm::Flush retired{*cpu_test_pmm};
    resource::Charge refund{};
    if (!retired.adopt(std::move(page_result).value())) {
        return false;
    }

    static_cast<void>(translation.enter(CpuId{0}));
    static_cast<void>(translation.enter(CpuId{1}));
    auto edit = translation.begin();
    arch::inject_ipi_failures_for_test(2);
    const bool complete = edit.commit(retired, &*cpu_test_registry, CpuId{0});
    const bool retried = retired.kick(*cpu_test_registry);
    arch::inject_ipi_failures_for_test(0);
    const auto held_state = cpu_test_pmm->state_of(page);
    const bool held = !complete && !retried && !retired.complete()
        && retired.acknowledged(CpuId{0}) && !retired.acknowledged(CpuId{1})
        && !retired.release(refund) && held_state
        && held_state.value() == mm::PageState::Allocated;
    mm::drain_tlb(remote->local.descriptor->logical_id());
    const bool completed = retired.complete() && retired.acknowledged(CpuId{1})
        && retired.release(refund);
    refund.reset();
    const auto released_state = cpu_test_pmm->state_of(page);
    translation.leave(CpuId{1});
    translation.leave(CpuId{0});
    return held
        && completed
        && released_state
        && released_state.value() == mm::PageState::Free
        && cpu_test_pmm->verify_invariants();
}

bool test_object_ref_generation_and_reclaim(
    const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize(320)) {
        return false;
    }
    auto kernel_vspace = make_test_root();
    if (kernel_vspace == nullptr) {
        return false;
    }
    {
        auto warm = mm::Stack::create(*kernel_vspace);
        if (!warm) {
            return false;
        }
    }
    const usize free_before = cpu_test_pmm->free_page_count();
    auto stack = mm::Stack::create(*kernel_vspace);
    if (!stack) {
        return false;
    }
    auto pending = cpu_test_threads->create(
        std::move(stack).value(),
        Env::kernel(*kernel_vspace),
        Thread::KernelStart{unused_idle_entry, nullptr});
    if (!pending) {
        return false;
    }
    auto owner = std::move(pending).value().publish();
    const object::ObjectId stale = owner.id();
    auto extra = owner.clone();
    auto ref_result = owner.erase();
    auto lookup = cpu_test_threads->lookup(stale);
    if (!extra || !ref_result || !lookup) {
        return false;
    }
    auto ref = std::move(ref_result).value();
    auto ref_clone = ref.clone();
    auto typed = ref.as<Thread>();
    auto wrong_type = ref.as<sched::Sc>();
    if (ref.id() != stale
        || ref.kind() != object::ObjectKind::Thread
        || !ref_clone
        || !typed
        || wrong_type
        || wrong_type.error() != object::error::wrong_type
        || !owner.retire()
        || cpu_test_threads->lookup(stale)
        || ref.clone()
        || ref.as<Thread>()) {
        return false;
    }
    auto extra_ref = std::move(extra).value();
    auto structural_ref = std::move(ref_clone).value();
    auto first = std::move(lookup).value();
    auto last = std::move(typed).value();
    usize notifications{};
    auto notify = [&notifications]() noexcept { ++notifications; };
    cpu_test_notify = decltype(cpu_test_notify)::bind(notify);

    owner.reset();
    extra_ref.reset();
    ref.reset();
    structural_ref.reset();
    cpu_test_threads->drain_reclaim();
    if (notifications != 0
        || first.get().state() != Thread::State::Prepared
        || last.get().state() != Thread::State::Prepared) {
        cpu_test_notify.reset();
        return false;
    }
    first.reset();
    cpu_test_threads->drain_reclaim();
    if (notifications != 0) {
        cpu_test_notify.reset();
        return false;
    }
    object::ref<> terminal = std::move(last);
    const bool transferred = !last && terminal.id() == stale;
    terminal.reset();
    cpu_test_threads->drain_reclaim();

    const bool reclaimed = transferred && notifications == 1
        && !cpu_test_threads->lookup(stale)
        && cpu_test_pmm->free_page_count() == free_before
        && cpu_test_pmm->verify_invariants();
    cpu_test_notify.reset();
    return reclaimed;
}

bool test_remote_queue_coalesces_without_losing_membership(
    const TestContext&) noexcept {
    usize owner{};
    sched::RemoteRequest request{
        sched::RemoteKind::Wake, &owner};
    sched::RemoteQueue queue{CpuId{0}};
    const auto first_post = queue.post(request);
    const auto first_signal = queue.claim_transport();
    const auto coalesced = queue.post(request);
    const auto duplicate_signal = queue.claim_transport();
    if (!first_signal) {
        return false;
    }
    queue.transport_failed(*first_signal);
    const auto retry_signal = queue.claim_transport();
    if (!retry_signal) {
        return false;
    }
    // A late error from the old transport generation must not demote the
    // replacement signal that now owns delivery.
    queue.transport_failed(*first_signal);
    const auto stale_retry = queue.claim_transport();
    sched::RemoteRequest* const taken = queue.take();
    const auto claimed_cancel = queue.cancel(request);
    const auto second_post = queue.post(request);
    const auto during_signal = queue.claim_transport();
    queue.complete(request);
    const bool drained = queue.take() == nullptr;
    const auto third_post = queue.post(request);
    const auto after_drain_signal = queue.claim_transport();
    sched::RemoteRequest* const final = queue.take();
    queue.complete(request);
    const bool final_drained = queue.take() == nullptr;
    const auto fourth_post = queue.post(request);
    const auto queued_cancel = queue.cancel(request);
    const auto canceled_again = queue.cancel(request);
    const bool canceled_drained = queue.size() == 0;

    const bool protocol =
        first_post == sched::RemotePost::Inserted
        && coalesced == sched::RemotePost::Coalesced
        && second_post == sched::RemotePost::Coalesced
        && third_post == sched::RemotePost::Inserted
        && fourth_post == sched::RemotePost::Inserted
        && first_signal && !duplicate_signal
        && retry_signal
        && retry_signal->generation != first_signal->generation
        && !stale_retry
        && taken == &request
        && claimed_cancel == sched::RemoteCancel::AlreadyClaimed
        && !during_signal
        && drained && after_drain_signal
        && final == &request && final_drained
        && queued_cancel == sched::RemoteCancel::CanceledQueued
        && canceled_again == sched::RemoteCancel::NotPending
        && canceled_drained;
    return protocol;
}

} // namespace

void register_cpu_topology_tests(TestRegistry& registry) noexcept {
    (void)registry.add("cpu-topology", "sparse IDs and firmware statuses populate canonical descriptors", test_sparse_inventory_and_statuses);
    (void)registry.add("cpu-topology", "malformed CPU nodes are rejected", test_malformed_cpu_nodes_are_rejected);
    (void)registry.add("cpu-topology", "boot hart matching requires one enabled entry", test_boot_hart_match_is_strict);
    (void)registry.add("cpu-topology", "builder rejects duplicate IDs and incomplete population", test_builder_rejects_mismatch_and_duplicate_id);
    (void)registry.add("cpu-topology", "record blocks cross legacy continuous-array thresholds", test_registry_crosses_legacy_array_thresholds);
    (void)registry.add("cpu-topology", "logical CPU namespace has one explicit bound", test_registry_rejects_unbounded_logical_ids);
    (void)registry.add("cpu-topology", "each stack/object allocation failure leaves runtime unpublished", test_prepare_resource_exhaustion_is_unpublished);
    (void)registry.add("cpu-topology", "secondary prepare failure preserves the prepared boot CPU", test_secondary_prepare_failure_preserves_prepared_boot_cpu);
    (void)registry.add("cpu-topology", "prepare publishes one descriptor-backed CpuRuntime", test_prepare_publishes_descriptor_borrow);
    (void)registry.add("cpu-topology", "lifecycle publication and snapshots derive from canonical states", test_lifecycle_start_failure_and_snapshot_use_canonical_states);
    (void)registry.add("cpu-topology", "shootdown acknowledgement controls detached-page retirement", test_shootdown_ack_controls_retirement);
    (void)registry.add("cpu-topology", "typed references retain retiring objects until final release", test_object_ref_generation_and_reclaim);
    (void)registry.add("cpu-topology", "RemoteQueue retains failed kicks without stale-generation loss", test_remote_queue_coalesces_without_losing_membership);
}
