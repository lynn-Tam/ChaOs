#include <test/test.hpp>

#include <arch/address_layout.hpp>
#include <arch/ipi.hpp>
#include <boot/cpu_topology.hpp>
#include <cpu/cpu_provisioner.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/utility.hpp>
#include <mm/pmm.hpp>
#include <mm/kernel_stack.hpp>
#include <mm/kernel_vspace.hpp>
#include <platform/memory_layout.hpp>
#include <object/object_store.hpp>
#include <mm/vspace_work.hpp>
#include <sched/context.hpp>
#include <sched/domain.hpp>
#include <sched/wake_queue.hpp>
#include <time/clock.hpp>

#include "arch/riscv64/mmu/sv39_builder.hpp"

namespace {

constexpr size_t cpu_test_pages = 256;
alignas(kernel::mm::page_size) uint8_t cpu_test_ram[cpu_test_pages * kernel::mm::page_size]{};
constinit libk::ManualLifetime<kernel::mm::Pmm> cpu_test_pmm{};
constinit libk::ManualLifetime<kernel::mm::DirectMap> cpu_test_direct{};
constinit libk::ManualLifetime<kernel::object::ObjectStore> cpu_test_objects{};
constinit libk::ManualLifetime<kernel::mm::VSpaceExecutor> cpu_test_vspace_work{};
constinit libk::ManualLifetime<kernel::time::Clock> cpu_test_clock{};
constinit libk::ManualLifetime<kernel::CpuRegistry> cpu_test_registry{};
constinit libk::ManualLifetime<kernel::mm::KernelVSpace> cpu_test_kernel{};

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
        const auto physical = platform::memory::linked_physical(kernel::mm::VirtAddr{
            reinterpret_cast<uintptr_t>(cpu_test_ram)});
        if (!physical) {
            return false;
        }
        const auto first = kernel::mm::Page::from_base(*physical);
        if (!first) {
            return false;
        }
        kernel::mm::RegionList map{};
        if (!map.try_emplace_back(kernel::mm::Region{
                kernel::mm::PageRange{*first, pages},
                kernel::mm::RegionKind::AvailableRam})) {
            return false;
        }
        const auto direct = kernel::mm::DirectMap::initialize_in(
            cpu_test_direct,
            map,
            kernel::mm::DirectMapLayout{
                .physical_base = kernel::mm::PhysAddr{
                    physical->raw()},
                .virtual_base = kernel::mm::VirtAddr{
                    reinterpret_cast<uintptr_t>(cpu_test_ram)},
                .window_size = sizeof(cpu_test_ram),
            });
        if (!direct) {
            return false;
        }
        if (!kernel::mm::Pmm::initialize_in(
                cpu_test_pmm, *cpu_test_direct, libk::move(map))) {
            return false;
        }
        auto& vspace_work = cpu_test_vspace_work.emplace();
        [[maybe_unused]] auto& objects =
            cpu_test_objects.emplace(*cpu_test_pmm, vspace_work);
        [[maybe_unused]] auto& clock = cpu_test_clock.emplace(10'000'000);
        return true;
    }

private:
    static auto reset() noexcept -> void {
        cpu_test_registry.reset();
        cpu_test_objects.reset();
        cpu_test_vspace_work.reset();
        cpu_test_clock.reset();
        cpu_test_kernel.reset();
        cpu_test_pmm.reset();
        cpu_test_direct.reset();
    }
};

[[nodiscard]] auto make_test_root() noexcept -> kernel::mm::KernelVSpace* {
    auto builder = arch::riscv64::Sv39Builder::create(*cpu_test_pmm);
    if (!builder) {
        return nullptr;
    }
    arch::KernelRoot root = libk::move(builder).value().finalize();
    if (!kernel::mm::KernelVSpace::adopt_in(
            cpu_test_kernel, *cpu_test_pmm, libk::move(root))) {
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
        be32(kernel::boot::fdt::FDT_BEGIN_NODE);
        cstring(name);
        align4();
    }

    auto end_node() noexcept -> void { be32(kernel::boot::fdt::FDT_END_NODE); }
    auto finish() noexcept -> void { be32(kernel::boot::fdt::FDT_END); }

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

    [[nodiscard]] auto view() const noexcept -> kernel::boot::fdt::FDT_View {
        if (!valid_) {
            return {};
        }
        kernel::boot::fdt::FDT_View result{};
        result.dt_struct = bytes_;
        result.dt_struct_size = size_;
        result.dt_strings = property_names;
        result.dt_strings_size = sizeof(property_names);
        return result;
    }

private:
    auto property_header(uint32_t name_offset, size_t length) noexcept -> void {
        be32(kernel::boot::fdt::FDT_PROP);
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
    size_t size_{};
    bool valid_{true};
};

constinit FdtStructureWriter fdt_writer{};

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

[[nodiscard]] auto finish_cpu_tree() noexcept -> kernel::boot::fdt::FDT_View {
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

    const auto summary = kernel::boot::parse_fdt_cpus_summary(
        view, kernel::CpuHardwareId{256});
    if (!summary
        || summary.value().count != 3
        || summary.value().boot_index != 1) {
        return false;
    }

    CpuStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }
    auto begun = kernel::CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        summary.value());
    if (!begun) {
        return false;
    }
    auto builder = libk::move(begun).value();
    if (!kernel::boot::populate_fdt_cpus(view, builder)
        || !builder.finish()) {
        return false;
    }

    const auto* cpu0 = cpu_test_registry->descriptor(kernel::CpuId{0});
    const auto* cpu1 = cpu_test_registry->descriptor(kernel::CpuId{1});
    const auto* cpu2 = cpu_test_registry->descriptor(kernel::CpuId{2});
    return cpu0 && cpu1 && cpu2
        && cpu0->hardware_id() == kernel::CpuHardwareId{0}
        && cpu0->availability() == kernel::CpuAvailability::Disabled
        && cpu1->hardware_id() == kernel::CpuHardwareId{256}
        && cpu1->availability() == kernel::CpuAvailability::Enabled
        && cpu2->hardware_id() == kernel::CpuHardwareId{1024}
        && cpu2->availability() == kernel::CpuAvailability::Failed
        && cpu0->state() == kernel::CpuState::Possible
        && cpu1->state() == kernel::CpuState::Present
        && cpu2->state() == kernel::CpuState::Failed
        && cpu2->failure()
        && *cpu2->failure() == kernel::CpuFailure::FirmwareReported;
}

bool test_malformed_cpu_nodes_are_rejected(const TestContext&) noexcept {
    fdt_writer.reset();
    fdt_writer.begin_node("");
    fdt_writer.end_node();
    fdt_writer.finish();
    const auto missing_cpus = kernel::boot::parse_fdt_cpus_summary(
        fdt_writer.view(), kernel::CpuHardwareId{0});
    if (missing_cpus
        || missing_cpus.error()
            != kernel::boot::CpuTopologyError::MissingCpusNode) {
        return false;
    }

    begin_cpu_tree();
    const auto zero_cpus = kernel::boot::parse_fdt_cpus_summary(
        finish_cpu_tree(), kernel::CpuHardwareId{0});
    if (zero_cpus
        || zero_cpus.error()
            != kernel::boot::CpuTopologyError::BootCpuMissing) {
        return false;
    }

    begin_cpu_tree(3, 0);
    add_cpu("cpu@0", 0);
    const auto bad_cells = kernel::boot::parse_fdt_cpus_summary(
        finish_cpu_tree(), kernel::CpuHardwareId{0});
    if (bad_cells
        || bad_cells.error()
            != kernel::boot::CpuTopologyError::InvalidAddressCells) {
        return false;
    }

    begin_cpu_tree(2, 1);
    add_cpu("cpu@0", 0);
    const auto bad_size_cells = kernel::boot::parse_fdt_cpus_summary(
        finish_cpu_tree(), kernel::CpuHardwareId{0});
    if (bad_size_cells
        || bad_size_cells.error()
            != kernel::boot::CpuTopologyError::InvalidSizeCells) {
        return false;
    }

    begin_cpu_tree();
    add_cpu("cpu@0", 0, nullptr, false);
    const auto missing_reg = kernel::boot::parse_fdt_cpus_summary(
        finish_cpu_tree(), kernel::CpuHardwareId{0});
    if (missing_reg
        || missing_reg.error()
            != kernel::boot::CpuTopologyError::MissingReg) {
        return false;
    }

    begin_cpu_tree();
    fdt_writer.begin_node("cpu@0");
    fdt_writer.reg64(0);
    fdt_writer.end_node();
    const auto missing_type = kernel::boot::parse_fdt_cpus_summary(
        finish_cpu_tree(), kernel::CpuHardwareId{0});
    if (missing_type
        || missing_type.error()
            != kernel::boot::CpuTopologyError::InvalidCpuNode) {
        return false;
    }

    begin_cpu_tree();
    fdt_writer.begin_node("cpu@0");
    fdt_writer.string_property(device_type_offset, "cpu");
    fdt_writer.reg64(0);
    fdt_writer.reg64(1);
    fdt_writer.end_node();
    const auto duplicate_reg = kernel::boot::parse_fdt_cpus_summary(
        finish_cpu_tree(), kernel::CpuHardwareId{0});
    if (duplicate_reg
        || duplicate_reg.error()
            != kernel::boot::CpuTopologyError::InvalidReg) {
        return false;
    }

    begin_cpu_tree();
    fdt_writer.begin_node("cpu@0");
    fdt_writer.string_property(device_type_offset, "cpu");
    fdt_writer.reg64_pair(0, 1);
    fdt_writer.end_node();
    const auto multi_tuple_reg = kernel::boot::parse_fdt_cpus_summary(
        finish_cpu_tree(), kernel::CpuHardwareId{0});
    if (multi_tuple_reg
        || multi_tuple_reg.error()
            != kernel::boot::CpuTopologyError::InvalidReg) {
        return false;
    }

    begin_cpu_tree();
    add_cpu("cpu@0", 0, "mystery");
    const auto bad_status = kernel::boot::parse_fdt_cpus_summary(
        finish_cpu_tree(), kernel::CpuHardwareId{0});
    return !bad_status
        && bad_status.error()
            == kernel::boot::CpuTopologyError::InvalidStatus;
}

bool test_boot_hart_match_is_strict(const TestContext&) noexcept {
    begin_cpu_tree();
    add_cpu("cpu@0", 0, "disabled");
    const auto disabled = kernel::boot::parse_fdt_cpus_summary(
        finish_cpu_tree(), kernel::CpuHardwareId{0});
    if (disabled
        || disabled.error()
            != kernel::boot::CpuTopologyError::BootCpuUnavailable) {
        return false;
    }

    begin_cpu_tree();
    add_cpu("cpu@0", 0);
    const auto missing = kernel::boot::parse_fdt_cpus_summary(
        finish_cpu_tree(), kernel::CpuHardwareId{7});
    if (missing
        || missing.error()
            != kernel::boot::CpuTopologyError::BootCpuMissing) {
        return false;
    }

    begin_cpu_tree();
    add_cpu("cpu@0", 0);
    add_cpu("cpu@00", 0);
    const auto duplicate = kernel::boot::parse_fdt_cpus_summary(
        finish_cpu_tree(), kernel::CpuHardwareId{0});
    return !duplicate
        && duplicate.error()
            == kernel::boot::CpuTopologyError::DuplicateBootCpu;
}

bool test_builder_rejects_mismatch_and_duplicate_id(const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }
    auto begun = kernel::CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        kernel::CpuTopologySummary{2, 0});
    if (!begun) {
        return false;
    }
    auto builder = libk::move(begun).value();
    if (!builder.append(
            kernel::CpuHardwareId{0},
            kernel::CpuAvailability::Enabled)) {
        return false;
    }
    const auto duplicate = builder.append(
        kernel::CpuHardwareId{0},
        kernel::CpuAvailability::Disabled);
    if (duplicate
        || duplicate.error()
            != kernel::CpuRegistry::Error::DuplicateHardwareId) {
        return false;
    }
    const auto incomplete = builder.finish();
    return !incomplete
        && incomplete.error()
            == kernel::CpuRegistry::Error::InvalidTopology;
}

[[nodiscard]] auto registry_accepts_count(usize count) noexcept -> bool {
    CpuStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }
    auto begun = kernel::CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        kernel::CpuTopologySummary{count, 0});
    if (!begun) {
        return false;
    }
    auto builder = libk::move(begun).value();
    for (usize index = 0; index < count; ++index) {
        if (!builder.append(
                kernel::CpuHardwareId{index * 17},
                kernel::CpuAvailability::Enabled)) {
            return false;
        }
    }
    if (!builder.finish() || cpu_test_registry->count() != count) {
        return false;
    }
    const auto* last = cpu_test_registry->descriptor(kernel::CpuId{count - 1});
    return last != nullptr
        && last->hardware_id()
            == kernel::CpuHardwareId{(count - 1) * 17};
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
    const auto begun = kernel::CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        kernel::CpuTopologySummary{100000, 0});
    return !begun
        && begun.error()
            == kernel::CpuRegistry::Error::InvalidTopology;
}

[[nodiscard]] auto prepare_failure_with_budget(
    bool warm_stack_pool,
    usize remaining_pages,
    kernel::CpuProvisioner::Error expected_error,
    kernel::CpuFailure expected_failure) noexcept -> bool {
    CpuStorageGuard storage{};
    if (!storage.initialize(64)) {
        return false;
    }
    auto begun = kernel::CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        kernel::CpuTopologySummary{1, 0});
    if (!begun) {
        return false;
    }
    auto builder = libk::move(begun).value();
    if (!builder.append(
            kernel::CpuHardwareId{0},
            kernel::CpuAvailability::Enabled)
        || !builder.finish()) {
        return false;
    }

    auto* const activation = make_test_root();
    if (activation == nullptr) {
        return false;
    }
    if (warm_stack_pool) {
        auto first = kernel::KernelStack::create(*activation);
        auto second = kernel::KernelStack::create(*activation);
        auto third = kernel::KernelStack::create(*activation);
        auto fourth = kernel::KernelStack::create(*activation);
        if (!first || !second || !third || !fourth) {
            return false;
        }
    }

    auto withheld = cpu_test_pmm->make_page_group();
    if (cpu_test_pmm->free_page_count() < remaining_pages) {
        return false;
    }
    {
        auto extension = withheld.extend();
        while (cpu_test_pmm->free_page_count() > remaining_pages) {
            if (!extension.allocate_page()) {
                return false;
            }
        }
        extension.commit();
    }

    kernel::CpuProvisioner provisioner{
        *cpu_test_registry,
        *cpu_test_pmm,
        *cpu_test_objects,
        *cpu_test_clock};
    const auto prepared = provisioner.prepare(
        kernel::CpuId{0},
        *activation,
        unused_idle_entry);
    const auto* const cpu =
        cpu_test_registry->descriptor(kernel::CpuId{0});
    return !prepared
        && prepared.error() == expected_error
        && cpu != nullptr
        && cpu_test_registry->runtime(kernel::CpuId{0}) == nullptr
        && cpu->state() == kernel::CpuState::Failed
        && cpu->failure()
        && *cpu->failure() == expected_failure
        && cpu_test_pmm->verify_invariants();
}

bool test_prepare_resource_exhaustion_is_unpublished(
    const TestContext&) noexcept {
    return prepare_failure_with_budget(
               false,
               1,
               kernel::CpuProvisioner::Error::StackAllocation,
               kernel::CpuFailure::StackAllocation)
        && prepare_failure_with_budget(
               true,
               1,
               kernel::CpuProvisioner::Error::MetadataAllocation,
               kernel::CpuFailure::MetadataAllocation)
        && prepare_failure_with_budget(
               true,
               2,
               kernel::CpuProvisioner::Error::ObjectAllocation,
               kernel::CpuFailure::ObjectAllocation);
}

bool test_prepare_metadata_exhaustion_is_unpublished(const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize(3)) {
        return false;
    }
    auto begun = kernel::CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        kernel::CpuTopologySummary{1, 0});
    if (!begun) {
        return false;
    }
    auto builder = libk::move(begun).value();
    if (!builder.append(
            kernel::CpuHardwareId{0},
            kernel::CpuAvailability::Enabled)
        || !builder.finish()) {
        return false;
    }
    const auto activation = make_test_root();
    if (!activation) {
        return false;
    }
    kernel::CpuProvisioner provisioner{
        *cpu_test_registry,
        *cpu_test_pmm,
        *cpu_test_objects,
        *cpu_test_clock};
    const auto prepared = provisioner.prepare(
        kernel::CpuId{0},
        *activation,
        unused_idle_entry);
    const auto* cpu = cpu_test_registry->descriptor(kernel::CpuId{0});
    return !prepared
        && prepared.error()
            == kernel::CpuProvisioner::Error::MetadataAllocation
        && cpu != nullptr
        && cpu->state() == kernel::CpuState::Failed
        && cpu->failure()
        && *cpu->failure() == kernel::CpuFailure::MetadataAllocation
        && cpu_test_registry->runtime(kernel::CpuId{0}) == nullptr
        && cpu_test_pmm->verify_invariants();
}

bool test_secondary_prepare_failure_preserves_prepared_boot_cpu(
    const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize(96)) {
        return false;
    }
    auto begun = kernel::CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        kernel::CpuTopologySummary{2, 0});
    if (!begun) {
        return false;
    }
    auto builder = libk::move(begun).value();
    if (!builder.append(
            kernel::CpuHardwareId{0},
            kernel::CpuAvailability::Enabled)
        || !builder.append(
            kernel::CpuHardwareId{1},
            kernel::CpuAvailability::Enabled)
        || !builder.finish()) {
        return false;
    }
    const auto activation = make_test_root();
    kernel::CpuProvisioner provisioner{
        *cpu_test_registry,
        *cpu_test_pmm,
        *cpu_test_objects,
        *cpu_test_clock};
    if (!activation
        || !provisioner.prepare(
            kernel::CpuId{0},
            *activation,
            unused_idle_entry)) {
        return false;
    }
    kernel::CpuRuntime* const boot =
        cpu_test_registry->runtime(kernel::CpuId{0});
    auto withheld = cpu_test_pmm->make_page_group();
    {
        auto extension = withheld.extend();
        while (cpu_test_pmm->free_page_count() > 1) {
            if (!extension.allocate_page()) {
                return false;
            }
        }
        extension.commit();
    }
    const auto secondary = provisioner.prepare(
        kernel::CpuId{1},
        *activation,
        unused_idle_entry);
    const auto* const boot_descriptor =
        cpu_test_registry->descriptor(kernel::CpuId{0});
    const auto* const secondary_descriptor =
        cpu_test_registry->descriptor(kernel::CpuId{1});
    return !secondary
        && secondary.error()
            == kernel::CpuProvisioner::Error::StackAllocation
        && boot != nullptr
        && cpu_test_registry->runtime(kernel::CpuId{0}) == boot
        && boot_descriptor->state() == kernel::CpuState::Prepared
        && secondary_descriptor->state() == kernel::CpuState::Failed
        && cpu_test_registry->runtime(kernel::CpuId{1}) == nullptr
        && cpu_test_pmm->verify_invariants();
}

bool test_prepare_publishes_descriptor_borrow(const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize(64)) {
        return false;
    }
    auto begun = kernel::CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        kernel::CpuTopologySummary{1, 0});
    if (!begun) {
        return false;
    }
    auto builder = libk::move(begun).value();
    if (!builder.append(
            kernel::CpuHardwareId{42},
            kernel::CpuAvailability::Enabled)
        || !builder.finish()) {
        return false;
    }
    const auto activation = make_test_root();
    if (!activation) {
        return false;
    }
    {
        kernel::CpuProvisioner provisioner{
            *cpu_test_registry,
            *cpu_test_pmm,
            *cpu_test_objects,
            *cpu_test_clock};
        if (!provisioner.prepare(
            kernel::CpuId{0},
            *activation,
            unused_idle_entry)) {
            return false;
        }
    }

    const auto* descriptor =
        cpu_test_registry->descriptor(kernel::CpuId{0});
    const auto* runtime = cpu_test_registry->runtime(kernel::CpuId{0});
    return descriptor != nullptr
        && descriptor->state() == kernel::CpuState::Prepared
        && runtime != nullptr
        && runtime->owner_registry == &*cpu_test_registry
        && runtime->local.descriptor == descriptor
        && runtime->local.current_thread() == nullptr
        && arch::active_stack(runtime->local.arch_state) == 0
        && runtime->diagnostics != nullptr
        && arch::panic_slot(runtime->local.arch_state)
            == &runtime->diagnostics->panic
        && arch::emergency_stack(runtime->local.arch_state)
            == runtime->stacks.emergency->top()
        && runtime->idle().state() == kernel::Thread::State::Prepared
        && runtime->idle().home_stack_top() != 0
        && (runtime->idle().home_stack_top() & 0xfU) == 0
        && runtime->start_context.ready()
        && cpu_test_registry->runtime_by_hardware_id(
            kernel::CpuHardwareId{42}) == runtime;
}

bool test_lifecycle_start_failure_and_snapshot_use_canonical_states(
    const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize(96)) {
        return false;
    }
    auto begun = kernel::CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        kernel::CpuTopologySummary{2, 0});
    if (!begun) {
        return false;
    }
    auto builder = libk::move(begun).value();
    if (!builder.append(
            kernel::CpuHardwareId{4},
            kernel::CpuAvailability::Enabled)
        || !builder.append(
            kernel::CpuHardwareId{19},
            kernel::CpuAvailability::Enabled)
        || !builder.finish()) {
        return false;
    }
    const auto activation = make_test_root();
    kernel::CpuProvisioner provisioner{
        *cpu_test_registry,
        *cpu_test_pmm,
        *cpu_test_objects,
        *cpu_test_clock};
    if (!activation
        || !provisioner.prepare(
            kernel::CpuId{0},
            *activation,
            unused_idle_entry)
        || !provisioner.prepare(
            kernel::CpuId{1},
            *activation,
            unused_idle_entry)) {
        return false;
    }

    auto* const first = cpu_test_registry->runtime(kernel::CpuId{0});
    auto* const second = cpu_test_registry->runtime(kernel::CpuId{1});
    if (first == nullptr || second == nullptr || first == second
        || &first->local == &second->local
        || first->stacks.init->top() == second->stacks.init->top()
        || &first->idle() == &second->idle()
        || first->idle().home_stack_top() == second->idle().home_stack_top()
        || &first->start_context == &second->start_context) {
        return false;
    }

    if (!cpu_test_registry->begin_start(kernel::CpuId{0})
        || cpu_test_registry->begin_start(kernel::CpuId{0})
        || !cpu_test_registry->fail_start(
            kernel::CpuId{1},
            kernel::CpuFailure::HsmUnavailable)
        || cpu_test_registry->publish_online(*first)) {
        return false;
    }

    const auto* const failed =
        cpu_test_registry->descriptor(kernel::CpuId{1});
    const kernel::CpuSnapshot snapshot = cpu_test_registry->snapshot();
    return failed != nullptr
        && failed->state() == kernel::CpuState::Failed
        && failed->failure()
        && *failed->failure() == kernel::CpuFailure::HsmUnavailable
        && cpu_test_registry->runtime(kernel::CpuId{1}) == second
        && snapshot.starting == 1
        && snapshot.failed == 1
        && snapshot.possible == 0
        && snapshot.present == 0
        && snapshot.prepared == 0
        && snapshot.online == 0;
}

bool test_shootdown_ack_controls_retirement(const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize(96)) {
        return false;
    }
    auto begun = kernel::CpuRegistry::begin(
        cpu_test_registry,
        *cpu_test_pmm,
        kernel::CpuTopologySummary{2, 0});
    if (!begun) {
        return false;
    }
    auto builder = libk::move(begun).value();
    if (!builder.append(
            kernel::CpuHardwareId{0},
            kernel::CpuAvailability::Enabled)
        || !builder.append(
            kernel::CpuHardwareId{4096},
            kernel::CpuAvailability::Enabled)
        || !builder.finish()) {
        return false;
    }
    const auto root = make_test_root();
    if (!root) {
        return false;
    }
    kernel::mm::TranslationState& translation = root->coherence();
    kernel::CpuProvisioner provisioner{
        *cpu_test_registry,
        *cpu_test_pmm,
        *cpu_test_objects,
        *cpu_test_clock};
    if (!provisioner.prepare(
            kernel::CpuId{0}, *root, unused_idle_entry)
        || !provisioner.prepare(
            kernel::CpuId{1}, *root, unused_idle_entry)) {
        return false;
    }
    kernel::CpuRuntime* const remote =
        cpu_test_registry->runtime(kernel::CpuId{1});
    auto page_result = cpu_test_pmm->allocate_page();
    if (remote == nullptr || !page_result) {
        return false;
    }
    const kernel::mm::Page page = page_result.value().page();
    kernel::mm::RetireBatch retired{*cpu_test_pmm};
    if (!retired.adopt(libk::move(page_result).value())) {
        return false;
    }

    static_cast<void>(translation.enter(kernel::CpuId{0}));
    static_cast<void>(translation.enter(kernel::CpuId{1}));
    kernel::mm::ShootdownTicket ticket{};
    auto mutation = translation.begin();
    if (!mutation) {
        translation.leave(kernel::CpuId{1});
        translation.leave(kernel::CpuId{0});
        return false;
    }
    auto plan = kernel::mm::ShootdownPlan::prepare(
        *cpu_test_registry,
        kernel::CpuId{0},
        mutation.value().targets());
    if (!plan) {
        mutation.value().abort();
        translation.leave(kernel::CpuId{1});
        translation.leave(kernel::CpuId{0});
        return false;
    }
    arch::inject_ipi_failures_for_test(2);
    const auto issued = mutation.value().commit(
        libk::move(plan).value(), ticket, &retired);
    const auto retained_retry = kernel::mm::retry_shootdowns(
        *cpu_test_registry, ticket);
    arch::inject_ipi_failures_for_test(0);
    const auto held_state = cpu_test_pmm->state_of(page);
    const bool held = issued != kernel::mm::ShootdownStatus::Complete
        && retained_retry == kernel::mm::ShootdownRetry::TransportFailure
        && !ticket.complete()
        && translation.pending_tickets() == 1
        && translation.pending_retires() == 1
        && ticket.acknowledged(kernel::CpuId{0})
        && !ticket.acknowledged(kernel::CpuId{1})
        && !retired.release()
        && held_state
        && held_state.value() == kernel::mm::PageState::Allocated;

    kernel::mm::drain_shootdowns(*remote);
    const bool completed = ticket.complete()
        && ticket.acknowledged(kernel::CpuId{1})
        && translation.pending_tickets() == 0
        && translation.pending_retires() == 1
        && retired.release()
        && translation.pending_retires() == 0;
    const auto released_state = cpu_test_pmm->state_of(page);
    translation.leave(kernel::CpuId{1});
    translation.leave(kernel::CpuId{0});
    return held
        && completed
        && released_state
        && released_state.value() == kernel::mm::PageState::Free
        && cpu_test_pmm->verify_invariants();
}

bool test_object_ref_generation_and_pin_reclaim(
    const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize(16)) {
        return false;
    }
    auto kernel_vspace = make_test_root();
    if (kernel_vspace == nullptr) {
        return false;
    }
    {
        auto warm = kernel::KernelStack::create(*kernel_vspace);
        if (!warm) {
            return false;
        }
    }
    const usize free_before = cpu_test_pmm->free_page_count();
    auto stack = kernel::KernelStack::create(*kernel_vspace);
    if (!stack) {
        return false;
    }
    auto pending = cpu_test_objects->create_thread(
        libk::move(stack).value(),
        kernel::ExecutionBinding::kernel(*kernel_vspace),
        kernel::Thread::KernelStart{unused_idle_entry, nullptr});
    if (!pending) {
        return false;
    }
    auto owner = libk::move(pending).value().publish();
    const kernel::object::ObjectId stale = owner.id();
    auto extra = owner.clone();
    auto ref_result = owner.ref();
    auto cold_pin = cpu_test_objects->pin_thread(stale);
    if (!extra || !ref_result || !cold_pin) {
        return false;
    }
    auto ref = libk::move(ref_result).value();
    auto ref_clone = ref.clone();
    auto ref_pin = ref.pin<kernel::Thread>();
    auto wrong_pin = ref.pin<kernel::sched::SchedulingContext>();
    if (ref.id() != stale
        || ref.kind() != kernel::object::ObjectKind::Thread
        || !ref_clone
        || !ref_pin
        || wrong_pin
        || wrong_pin.error() != kernel::object::ObjectError::WrongKind
        || !owner.retire()
        || cpu_test_objects->hold_thread(stale)
        || cpu_test_objects->pin_thread(stale)
        || ref.clone()
        || ref.pin<kernel::Thread>()) {
        return false;
    }
    auto extra_hold = libk::move(extra).value();
    auto structural_ref = libk::move(ref_clone).value();
    auto active_cold_pin = libk::move(cold_pin).value();
    auto active_ref_pin = libk::move(ref_pin).value();
    usize notifications{};
    auto notify = [&notifications]() noexcept {
        ++notifications;
    };
    cpu_test_objects->bind_reclaim_notifier(
        kernel::object::ObjectStore::ReclaimNotifier::bind(notify));

    owner.reset();
    extra_hold.reset();
    ref.reset();
    structural_ref.reset();
    cpu_test_objects->drain_reclaim();
    if (notifications != 0
        || active_cold_pin.get().state() != kernel::Thread::State::Prepared
        || active_ref_pin.get().state() != kernel::Thread::State::Prepared) {
        cpu_test_objects->unbind_reclaim_notifier();
        return false;
    }
    active_cold_pin.reset();
    cpu_test_objects->drain_reclaim();
    if (notifications != 0) {
        cpu_test_objects->unbind_reclaim_notifier();
        return false;
    }
    active_ref_pin.reset();
    cpu_test_objects->drain_reclaim();

    const bool reclaimed = notifications == 1
        && !cpu_test_objects->hold_thread(stale)
        && !cpu_test_objects->pin_thread(stale)
        && cpu_test_pmm->free_page_count() == free_before
        && cpu_test_pmm->verify_invariants();
    cpu_test_objects->unbind_reclaim_notifier();
    return reclaimed;
}

bool test_wake_queue_coalesces_without_losing_membership(
    const TestContext&) noexcept {
    CpuStorageGuard storage{};
    if (!storage.initialize(24)) {
        return false;
    }
    auto capacity = kernel::sched::DomainCapacity::create(*cpu_test_pmm, 1);
    if (!capacity) {
        return false;
    }
    kernel::sched::SchedulingDomain domain{
        libk::move(capacity).value(),
        kernel::sched::SchedulingDomain::share_scale,
        100'000};
    auto kernel_vspace = make_test_root();
    if (kernel_vspace == nullptr) {
        return false;
    }
    auto stack = kernel::KernelStack::create(*kernel_vspace);
    if (!stack) {
        return false;
    }
    auto pending = cpu_test_objects->create_thread(
        libk::move(stack).value(),
        kernel::ExecutionBinding::kernel(*kernel_vspace),
        kernel::Thread::KernelStart{unused_idle_entry, nullptr});
    if (!pending) {
        return false;
    }
    auto thread = libk::move(pending).value().publish();
    const auto urgency = kernel::sched::Urgency::make(1);
    if (!urgency) {
        return false;
    }
    kernel::sched::SchedulingContext context{
        kernel::sched::SchedulingContext::Config{
            .budget = kernel::time::Duration::from_ticks(1),
            .period = kernel::time::Duration::from_ticks(2),
            .urgency = *urgency,
        },
        kernel::time::Instant::from_ticks(0)};
    auto target = thread.clone();
    if (!target || !domain.admit(context, kernel::CpuId{0})
        || !context.bind(libk::move(target).value(), kernel::CpuId{0})) {
        return false;
    }

    kernel::sched::WakeQueue queue{};
    kernel::sched::Binding& binding = *context.binding();
    const auto first = queue.post(binding);
    const auto first_signal = queue.claim_transport();
    const auto duplicate = queue.post(binding);
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
    kernel::sched::Binding* const taken = queue.take();
    const auto during_delivery = queue.post(binding);
    const auto during_signal = queue.claim_transport();
    queue.complete(binding);
    const bool drained = queue.take() == nullptr;
    const auto after_drain = queue.post(binding);
    const auto after_drain_signal = queue.claim_transport();
    kernel::sched::Binding* const final = queue.take();
    queue.complete(binding);
    const bool final_drained = queue.take() == nullptr;

    const bool protocol = first.accepted && first_signal
        && duplicate.accepted && !duplicate_signal
        && retry_signal
        && retry_signal->generation != first_signal->generation
        && !stale_retry
        && taken == &binding
        && during_delivery.accepted && !during_signal
        && drained && after_drain.accepted && after_drain_signal
        && final == &binding && final_drained;
    if (!context.unbind() || !domain.unadmit(context)
        || !thread.retire()) {
        return false;
    }
    thread.reset();
    cpu_test_objects->drain_reclaim();
    return protocol && cpu_test_pmm->verify_invariants();
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
    (void)registry.add("cpu-topology", "runtime metadata exhaustion leaves association unpublished", test_prepare_metadata_exhaustion_is_unpublished);
    (void)registry.add("cpu-topology", "secondary prepare failure preserves the prepared boot CPU", test_secondary_prepare_failure_preserves_prepared_boot_cpu);
    (void)registry.add("cpu-topology", "prepare publishes one descriptor-backed CpuRuntime", test_prepare_publishes_descriptor_borrow);
    (void)registry.add("cpu-topology", "lifecycle publication and snapshots derive from canonical states", test_lifecycle_start_failure_and_snapshot_use_canonical_states);
    (void)registry.add("cpu-topology", "shootdown acknowledgement controls detached-page retirement", test_shootdown_ack_controls_retirement);
    (void)registry.add("cpu-topology", "ObjectRef and typed pins share canonical reclaim state", test_object_ref_generation_and_pin_reclaim);
    (void)registry.add("cpu-topology", "WakeQueue retains failed kicks without stale-generation loss", test_wake_queue_coalesces_without_losing_membership);
}
