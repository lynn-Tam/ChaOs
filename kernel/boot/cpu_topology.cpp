#include <boot/cpu_topology.hpp>

#include <libk/byte_reader.hpp>
#include <libk/limits.hpp>

namespace kernel::boot {
namespace {

[[nodiscard]] auto read_id(libk::ByteSpan bytes, uint32_t cells,
                           uint64_t& id) noexcept -> bool {
    if (bytes.size() != cells * sizeof(uint32_t)) {
        return false;
    }
    libk::ByteReader reader{bytes.data(), bytes.size()};
    id = 0;
    for (uint32_t index = 0; index < cells; ++index) {
        uint32_t cell{};
        if (!reader.read_be32(cell)) {
            return false;
        }
        id = id << 32 | cell;
    }
    return true;
}

[[nodiscard]] auto parse_status(libk::ByteSpan bytes,
                                CpuAvailability& availability) noexcept -> bool {
    const auto status = Fdt::first_string(bytes);
    if (!status) {
        return false;
    }
    if (*status == "okay" || *status == "ok") {
        availability = CpuAvailability::Enabled;
    } else if (*status == "disabled") {
        availability = CpuAvailability::Disabled;
    } else if (*status == "fail" || status->starts_with("fail-")) {
        availability = CpuAvailability::Failed;
    } else {
        return false;
    }
    return true;
}

} // namespace

auto parse_fdt_cpus(const Fdt& tree, CpuHardwareId boot_hardware_id,
                    CpuHandoff& destination) noexcept
    -> libk::Expected<void, CpuTopologyError> {
    destination.cpus.clear();
    destination.boot_index = 0;
    const auto cpus = tree.child(tree.root(), "cpus");
    if (!cpus) {
        return libk::unexpected(CpuTopologyError::MissingCpusNode);
    }
    for (auto node = tree.next_sibling(*cpus); node;
         node = tree.next_sibling(*node)) {
        if (tree.node_name(*node) == "cpus") {
            return libk::unexpected(CpuTopologyError::InvalidCpuNode);
        }
    }
    uint32_t address_cells{};
    const auto address = tree.property(*cpus, "#address-cells");
    if (tree.property_count(*cpus, "#address-cells") != 1
        || !address || !Fdt::read_u32(*address, address_cells)
        || address_cells == 0 || address_cells > 2) {
        return libk::unexpected(CpuTopologyError::InvalidAddressCells);
    }
    uint32_t size_cells{};
    const auto size = tree.property(*cpus, "#size-cells");
    if (tree.property_count(*cpus, "#size-cells") != 1
        || !size || !Fdt::read_u32(*size, size_cells)
        || size_cells != 0) {
        return libk::unexpected(CpuTopologyError::InvalidSizeCells);
    }
    size_t boot_matches = 0;
    bool boot_unavailable = false;
    for (auto node = tree.first_child(*cpus); node;
         node = tree.next_sibling(*node)) {
        const auto name = tree.node_name(*node);
        if (name != "cpu" && !name.starts_with("cpu@")) {
            continue;
        }
        const auto type = tree.property(*node, "device_type");
        const auto type_name = type ? Fdt::first_string(*type) : libk::nullopt;
        if (tree.property_count(*node, "device_type") != 1
            || !type_name || *type_name != "cpu") {
            return libk::unexpected(CpuTopologyError::InvalidCpuNode);
        }
        const auto reg = tree.property(*node, "reg");
        if (!reg) {
            return libk::unexpected(CpuTopologyError::MissingReg);
        }
        uint64_t raw_id{};
        if (tree.property_count(*node, "reg") != 1
            || !read_id(*reg, address_cells, raw_id)) {
            return libk::unexpected(CpuTopologyError::InvalidReg);
        }
        CpuAvailability availability = CpuAvailability::Enabled;
        const auto status = tree.property(*node, "status");
        if (tree.property_count(*node, "status") > 1
            || (status && !parse_status(*status, availability))) {
            return libk::unexpected(CpuTopologyError::InvalidStatus);
        }
        if (raw_id > libk::numeric_limits<usize>::max()) {
            return libk::unexpected(CpuTopologyError::InvalidReg);
        }
        const CpuHardwareId id{static_cast<usize>(raw_id)};
        if (id == boot_hardware_id) {
            ++boot_matches;
            destination.boot_index = destination.cpus.size();
            boot_unavailable = availability != CpuAvailability::Enabled;
        }
        if (!destination.cpus.try_push_back(BootCpu{
                .hardware_id = id, .availability = availability})) {
            return libk::unexpected(CpuTopologyError::CapacityExceeded);
        }
    }
    if (destination.cpus.empty() || boot_matches == 0) {
        return libk::unexpected(CpuTopologyError::BootCpuMissing);
    }
    if (boot_matches != 1) {
        return libk::unexpected(CpuTopologyError::DuplicateBootCpu);
    }
    for (size_t first = 0; first < destination.cpus.size(); ++first) {
        for (size_t second = first + 1; second < destination.cpus.size(); ++second) {
            if (destination.cpus[first].hardware_id
                == destination.cpus[second].hardware_id) {
                return libk::unexpected(CpuTopologyError::DuplicateCpu);
            }
        }
    }
    if (boot_unavailable) {
        return libk::unexpected(CpuTopologyError::BootCpuUnavailable);
    }
    return libk::expected();
}

} // namespace kernel::boot
