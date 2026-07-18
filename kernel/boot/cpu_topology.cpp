#include <boot/cpu_topology.hpp>

#include <libk/limits.hpp>
#include <libk/optional.hpp>

namespace kernel::boot {
namespace {

[[nodiscard]] auto as_string(kernel::boot::fdt::ByteSpan bytes) noexcept
    -> libk::optional<kernel::boot::fdt::StrView> {
    for (usize index = 0; index < bytes.size(); ++index) {
        if (bytes[index] == 0) {
            return kernel::boot::fdt::StrView{
                reinterpret_cast<const char*>(bytes.data()),
                index,
            };
        }
    }
    return libk::nullopt;
}

[[nodiscard]] auto read_single_cell(
    kernel::boot::fdt::ByteSpan bytes,
    uint32_t& value) noexcept -> bool {
    kernel::boot::fdt::ByteReader reader{bytes.data(), bytes.size()};
    return bytes.size() == sizeof(uint32_t)
        && reader.read_be32(value);
}

[[nodiscard]] auto parse_status(
    kernel::boot::fdt::ByteSpan bytes,
    CpuAvailability& availability) noexcept -> bool {
    const auto status = as_string(bytes);
    if (!status) {
        return false;
    }
    if (*status == "okay" || *status == "ok") {
        availability = CpuAvailability::Enabled;
        return true;
    }
    if (*status == "disabled") {
        availability = CpuAvailability::Disabled;
        return true;
    }
    if (*status == "fail" || status->starts_with("fail-")) {
        availability = CpuAvailability::Failed;
        return true;
    }
    return false;
}

template<typename Sink>
class CpuCollector final {
public:
    CpuCollector(Sink& sink, CpuTopologyError& error) noexcept
        : sink_(sink), error_(error) {
    }

    [[nodiscard]] auto begin_node(
        kernel::boot::fdt::StrView name,
        int depth) noexcept -> bool {
        if (depth == 1) {
            in_cpus_ = name == "cpus";
            if (in_cpus_) {
                if (saw_cpus_) {
                    return fail(CpuTopologyError::InvalidCpuNode);
                }
                saw_cpus_ = true;
            }
            return true;
        }
        if (depth == 2 && in_cpus_) {
            in_cpu_ = name == "cpu" || name.starts_with("cpu@");
            if (in_cpu_) {
                device_type_seen_ = false;
                device_type_cpu_ = false;
                reg_seen_ = false;
                reg_ = {};
                status_seen_ = false;
                availability_ = CpuAvailability::Enabled;
            }
        }
        return true;
    }

    [[nodiscard]] auto prop(
        kernel::boot::fdt::StrView name,
        kernel::boot::fdt::ByteSpan value,
        int depth) noexcept -> bool {
        if (depth == 1 && in_cpus_) {
            uint32_t cells{};
            if (name == "#address-cells") {
                if (address_cells_seen_
                    || !read_single_cell(value, cells)
                    || cells == 0
                    || cells > 2) {
                    return fail(CpuTopologyError::InvalidAddressCells);
                }
                address_cells_seen_ = true;
                address_cells_ = cells;
            } else if (name == "#size-cells") {
                if (size_cells_seen_
                    || !read_single_cell(value, cells)
                    || cells != 0) {
                    return fail(CpuTopologyError::InvalidSizeCells);
                }
                size_cells_seen_ = true;
            }
            return true;
        }
        if (depth != 2 || !in_cpu_) {
            return true;
        }
        if (name == "device_type") {
            if (device_type_seen_) {
                return fail(CpuTopologyError::InvalidCpuNode);
            }
            device_type_seen_ = true;
            const auto type = as_string(value);
            device_type_cpu_ = type && *type == "cpu";
            return device_type_cpu_
                || fail(CpuTopologyError::InvalidCpuNode);
        }
        if (name == "reg") {
            if (reg_seen_) {
                return fail(CpuTopologyError::InvalidReg);
            }
            reg_seen_ = true;
            reg_ = value;
            return true;
        }
        if (name == "status") {
            if (status_seen_ || !parse_status(value, availability_)) {
                return fail(CpuTopologyError::InvalidStatus);
            }
            status_seen_ = true;
        }
        return true;
    }

    [[nodiscard]] auto end_node(int depth) noexcept -> bool {
        if (depth == 2 && in_cpu_) {
            if (!device_type_seen_ || !device_type_cpu_) {
                return fail(CpuTopologyError::InvalidCpuNode);
            }
            if (!reg_seen_) {
                return fail(CpuTopologyError::MissingReg);
            }
            if (!emit_reg()) {
                return false;
            }
            in_cpu_ = false;
        } else if (depth == 1 && in_cpus_) {
            in_cpus_ = false;
        }
        return true;
    }

    [[nodiscard]] auto finish() noexcept -> bool {
        if (!saw_cpus_) {
            return fail(CpuTopologyError::MissingCpusNode);
        }
        if (!address_cells_seen_) {
            return fail(CpuTopologyError::InvalidAddressCells);
        }
        if (!size_cells_seen_) {
            return fail(CpuTopologyError::InvalidSizeCells);
        }
        return true;
    }

private:
    [[nodiscard]] auto fail(CpuTopologyError error) noexcept -> bool {
        error_ = error;
        return false;
    }

    [[nodiscard]] auto emit_reg() noexcept -> bool {
        if (!address_cells_seen_ || !size_cells_seen_) {
            return fail(!address_cells_seen_
                ? CpuTopologyError::InvalidAddressCells
                : CpuTopologyError::InvalidSizeCells);
        }
        const usize tuple_size = address_cells_ * sizeof(uint32_t);
        // A direct child of /cpus describes exactly one CPU identity.  A
        // multi-tuple reg would make the node count and descriptor count
        // disagree and would leave status applying ambiguously to several
        // hardware IDs.
        if (reg_.size() != tuple_size) {
            return fail(CpuTopologyError::InvalidReg);
        }

        kernel::boot::fdt::ByteReader reader{reg_.data(), reg_.size()};
        uint64_t raw_id{};
        for (uint32_t index = 0; index < address_cells_; ++index) {
            uint32_t cell{};
            if (!reader.read_be32(cell)) {
                return fail(CpuTopologyError::InvalidReg);
            }
            raw_id = raw_id << 32 | cell;
        }
        if (raw_id > libk::numeric_limits<usize>::max()
            || !sink_(
                CpuHardwareId{static_cast<usize>(raw_id)},
                availability_)) {
            return fail(CpuTopologyError::CapacityExceeded);
        }
        return true;
    }

    Sink& sink_;
    CpuTopologyError& error_;
    bool saw_cpus_{};
    bool in_cpus_{};
    bool in_cpu_{};
    bool address_cells_seen_{};
    bool size_cells_seen_{};
    uint32_t address_cells_{};
    bool device_type_seen_{};
    bool device_type_cpu_{};
    bool reg_seen_{};
    kernel::boot::fdt::ByteSpan reg_{};
    bool status_seen_{};
    CpuAvailability availability_{CpuAvailability::Enabled};
};

template<typename Sink>
[[nodiscard]] auto walk_cpus(
    const kernel::boot::fdt::FDT_View& view,
    Sink& sink) noexcept -> libk::Expected<void, CpuTopologyError> {
    CpuTopologyError error{CpuTopologyError::InvalidCpuNode};
    CpuCollector<Sink> collector{sink, error};
    if (!kernel::boot::fdt::walk_view(view, collector) || !collector.finish()) {
        return libk::unexpected(error);
    }
    return libk::expected();
}

} // namespace

auto parse_fdt_cpus(
    const kernel::boot::fdt::FDT_View& view,
    CpuHardwareId boot_hardware_id,
    CpuHandoff& destination) noexcept
    -> libk::Expected<void, CpuTopologyError> {
    destination.cpus.clear();
    destination.boot_index = 0;
    usize boot_matches{};
    bool boot_unavailable{};

    auto sink = [&](CpuHardwareId hardware_id, CpuAvailability availability) {
        if (hardware_id == boot_hardware_id) {
            ++boot_matches;
            destination.boot_index = destination.cpus.size();
            boot_unavailable = availability != CpuAvailability::Enabled;
        }
        return destination.cpus.try_push_back(BootCpu{
            .hardware_id = hardware_id,
            .availability = availability,
        });
    };
    auto walked = walk_cpus(view, sink);
    if (!walked) {
        return libk::unexpected(walked.error());
    }
    if (destination.cpus.empty() || boot_matches == 0) {
        return libk::unexpected(CpuTopologyError::BootCpuMissing);
    }
    if (boot_matches != 1) {
        return libk::unexpected(CpuTopologyError::DuplicateBootCpu);
    }
    for (usize first = 0; first < destination.cpus.size(); ++first) {
        for (usize second = first + 1;
             second < destination.cpus.size();
             ++second) {
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
