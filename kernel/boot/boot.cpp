#include <boot/boot_info.hpp>
#include <boot/cpu_topology.hpp>
#include <boot/firmware/devicetree/fdt.hpp>
#include <boot/timebase.hpp>

#include <diag/console.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/limits.hpp>
#include <libk/optional.hpp>
#include <libk/utility.hpp>
#include <mm/boot_map.hpp>
#include <core/kernel_image.hpp>

namespace {

[[nodiscard]] auto as_string(kernel::boot::fdt::ByteSpan bytes) noexcept -> libk::optional<kernel::boot::fdt::StrView> {
    for (size_t index = 0; index < bytes.size(); ++index) {
        if (bytes[index] == 0) {
            return kernel::boot::fdt::StrView{
                reinterpret_cast<const char*>(bytes.data()),
                index,
            };
        }
    }
    return libk::nullopt;
}

[[nodiscard]] auto before_colon(kernel::boot::fdt::StrView string) noexcept -> kernel::boot::fdt::StrView {
    for (size_t index = 0; index < string.size(); ++index) {
        if (string[index] == ':') {
            return kernel::boot::fdt::StrView{string.data(), index};
        }
    }
    return string;
}

struct Reg {
    uint64_t address{};
    uint64_t size{};

    [[nodiscard]] auto contained_pages() const noexcept
        -> libk::optional<kernel::mm::PageRange> {
        if (address > libk::numeric_limits<uintptr_t>::max()
            || size > libk::numeric_limits<size_t>::max()) {
            return libk::nullopt;
        }
        return kernel::mm::PageRange::contained_bytes(
            kernel::mm::PhysAddr{static_cast<uintptr_t>(address)},
            static_cast<size_t>(size));
    }

    [[nodiscard]] auto covering_pages() const noexcept
        -> libk::optional<kernel::mm::PageRange> {
        if (address > libk::numeric_limits<uintptr_t>::max()
            || size > libk::numeric_limits<size_t>::max()) {
            return libk::nullopt;
        }
        return kernel::mm::PageRange::covering_bytes(
            kernel::mm::PhysAddr{static_cast<uintptr_t>(address)},
            static_cast<size_t>(size));
    }
};

class RegFormat {
public:
    template<typename Visitor>
    [[nodiscard]] auto visit(kernel::boot::fdt::ByteSpan bytes, Visitor&& visitor) const noexcept -> bool {
        if (!valid()) {
            return false;
        }
        const size_t tuple_size = (address_cells_ + size_cells_) * sizeof(uint32_t);
        if (bytes.empty() || bytes.size() % tuple_size != 0) {
            return false;
        }

        kernel::boot::fdt::ByteReader reader{bytes.data(), bytes.size()};
        auto read = [&reader](uint32_t cells, uint64_t& value) {
            value = 0;
            for (uint32_t index = 0; index < cells; ++index) {
                uint32_t cell{};
                if (!reader.read_be32(cell)) {
                    return false;
                }
                value = value << 32 | cell;
            }
            return true;
        };

        while (reader.remaining() != 0) {
            Reg reg{};
            if (!read(address_cells_, reg.address)
                || !read(size_cells_, reg.size)
                || !visitor(reg)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] auto set_address_cells(kernel::boot::fdt::ByteSpan bytes) noexcept -> bool {
        return set(bytes, address_cells_);
    }

    [[nodiscard]] auto set_size_cells(kernel::boot::fdt::ByteSpan bytes) noexcept -> bool {
        return set(bytes, size_cells_);
    }

    [[nodiscard]] auto read_address(
        kernel::boot::fdt::ByteSpan bytes,
        uint64_t& value) const noexcept -> bool {
        if (!valid() || bytes.size() != address_cells_ * sizeof(uint32_t)) {
            return false;
        }
        kernel::boot::fdt::ByteReader reader{bytes.data(), bytes.size()};
        value = 0;
        for (uint32_t index = 0; index < address_cells_; ++index) {
            uint32_t cell{};
            if (!reader.read_be32(cell)) {
                return false;
            }
            value = value << 32 | cell;
        }
        return true;
    }

private:
    [[nodiscard]] auto valid() const noexcept -> bool {
        return address_cells_ > 0 && address_cells_ <= 2
            && size_cells_ > 0 && size_cells_ <= 2;
    }

    [[nodiscard]] static auto set(kernel::boot::fdt::ByteSpan bytes, uint32_t& cells) noexcept -> bool {
        kernel::boot::fdt::ByteReader reader{bytes.data(), bytes.size()};
        uint32_t value{};
        if (bytes.size() != sizeof(uint32_t)
            || !reader.read_be32(value)
            || value == 0
            || value > 2) {
            return false;
        }
        cells = value;
        return true;
    }

    uint32_t address_cells_{2};
    uint32_t size_cells_{2};
};

struct Alias {
    kernel::boot::fdt::StrView name{};
    kernel::boot::fdt::StrView path{};
};

class BootCollector {
public:
    explicit BootCollector(kernel::mm::BootMapBuilder& memory) noexcept
        : memory_(memory) {}

    [[nodiscard]] auto begin_node(kernel::boot::fdt::StrView name, int depth) noexcept -> bool {
        if (depth == 1) {
            if (name == "aliases") {
                scope_ = Scope::Aliases;
            } else if (name == "chosen") {
                scope_ = Scope::Chosen;
            } else if (name == "memory" || name.starts_with("memory@")) {
                scope_ = Scope::Memory;
            } else if (name == "reserved-memory") {
                scope_ = Scope::ReservedMemory;
                reserved_format_ = root_format_;
            } else {
                scope_ = Scope::Other;
            }
        } else if (depth == 2) {
            in_reserved_child_ = scope_ == Scope::ReservedMemory;
        }
        return true;
    }

    [[nodiscard]] auto prop(kernel::boot::fdt::StrView name, kernel::boot::fdt::ByteSpan value, int depth) noexcept -> bool {
        if (depth == 0 && name == "#address-cells") {
            return root_format_.set_address_cells(value);
        }
        if (depth == 0 && name == "#size-cells") {
            return root_format_.set_size_cells(value);
        }
        if (depth == 1 && scope_ == Scope::Aliases) {
            const auto path = as_string(value);
            return path && aliases_.try_emplace_back(Alias{name, *path});
        }
        if (depth == 1 && scope_ == Scope::Chosen && name == "stdout-path") {
            const auto path = as_string(value);
            if (!path) {
                return false;
            }
            stdout_path_ = before_colon(*path);
            return true;
        }
        if (depth == 1 && scope_ == Scope::Chosen
            && name == "linux,initrd-start") {
            uint64_t address{};
            if (!root_format_.read_address(value, address)) {
                return false;
            }
            initrd_start_ = address;
            return true;
        }
        if (depth == 1 && scope_ == Scope::Chosen
            && name == "linux,initrd-end") {
            uint64_t address{};
            if (!root_format_.read_address(value, address)) {
                return false;
            }
            initrd_end_ = address;
            return true;
        }
        if (depth == 1 && scope_ == Scope::Memory && name == "reg") {
            return root_format_.visit(value, [this](Reg reg) {
                const auto range = reg.contained_pages();
                return range && static_cast<bool>(memory_.add_ram(*range));
            });
        }
        if (depth == 1 && scope_ == Scope::ReservedMemory) {
            if (name == "#address-cells") {
                return reserved_format_.set_address_cells(value);
            }
            if (name == "#size-cells") {
                return reserved_format_.set_size_cells(value);
            }
        }
        if (depth == 2 && in_reserved_child_ && name == "reg") {
            return reserved_format_.visit(value, [this](Reg reg) {
                const auto range = reg.covering_pages();
                return range && static_cast<bool>(memory_.reserve(
                    *range,
                    kernel::mm::RegionKind::FirmwareReserved));
            });
        }
        return true;
    }

    [[nodiscard]] auto end_node(int depth) noexcept -> bool {
        if (depth == 2) {
            in_reserved_child_ = false;
        } else if (depth == 1) {
            scope_ = Scope::Other;
        }
        return true;
    }

    [[nodiscard]] auto stdout_path() const noexcept -> libk::optional<kernel::boot::fdt::StrView> {
        if (!stdout_path_ || stdout_path_->empty()) {
            return libk::nullopt;
        }
        if ((*stdout_path_)[0] == '/') {
            return stdout_path_;
        }
        for (const auto& alias : aliases_) {
            if (alias.name == *stdout_path_) {
                return alias.path;
            }
        }
        return libk::nullopt;
    }

    [[nodiscard]] auto module() const noexcept
        -> libk::Expected<
            libk::optional<kernel::boot::BootModule>,
            kernel::boot::BootInfoError> {
        if (!initrd_start_ && !initrd_end_) {
            return libk::expected(
                libk::optional<kernel::boot::BootModule>{});
        }
        if (!initrd_start_ || !initrd_end_
            || *initrd_start_ >= *initrd_end_
            || *initrd_start_ > libk::numeric_limits<usize>::max()
            || *initrd_end_ - *initrd_start_
                > libk::numeric_limits<usize>::max()) {
            return libk::unexpected(
                kernel::boot::BootInfoError::InvalidModuleRange);
        }
        const kernel::mm::PhysAddr physical{
            static_cast<usize>(*initrd_start_)};
        const usize size = static_cast<usize>(*initrd_end_ - *initrd_start_);
        const auto pages = kernel::mm::PageRange::covering_bytes(
            physical, size);
        if (!pages) {
            return libk::unexpected(
                kernel::boot::BootInfoError::InvalidModuleRange);
        }
        return libk::expected(libk::optional<kernel::boot::BootModule>{
            kernel::boot::BootModule{
                .physical = physical,
                .size = size,
                .pages = *pages,
                .kind = kernel::boot::BootModuleKind::Bundle,
            }});
    }

private:
    enum class Scope : uint8_t {
        Other,
        Aliases,
        Chosen,
        Memory,
        ReservedMemory,
    };

    kernel::mm::BootMapBuilder& memory_;
    RegFormat root_format_{};
    RegFormat reserved_format_{};
    Scope scope_{Scope::Other};
    bool in_reserved_child_{};
    libk::InplaceVector<Alias, 8> aliases_{};
    libk::optional<kernel::boot::fdt::StrView> stdout_path_{};
    libk::optional<uint64_t> initrd_start_{};
    libk::optional<uint64_t> initrd_end_{};
};

[[nodiscard]] auto reserve_kernel(kernel::mm::BootMapBuilder& memory) noexcept -> bool {
    const auto boot_entry = kernel::image::boot_entry();
    const auto secondary = kernel::image::secondary_entry();
    const auto transition = kernel::image::transition();
    const auto high_image = kernel::image::physical_image();

    for (const auto& bank : memory.ram()) {
        if (!bank.contains(boot_entry)
            || !bank.contains(secondary)
            || !bank.contains(transition)
            || !bank.contains(high_image)) {
            continue;
        }
        const size_t prefix_pages = boot_entry.first().frame().raw()
            - bank.first().frame().raw();
        if (prefix_pages != 0
            && !memory.reserve(
                kernel::mm::PageRange{bank.first(), prefix_pages},
                kernel::mm::RegionKind::FirmwareReserved)) {
            return false;
        }
        return memory.reserve(boot_entry, kernel::mm::RegionKind::KernelImage)
            && memory.reserve(secondary, kernel::mm::RegionKind::KernelImage)
            && memory.reserve(
                transition, kernel::mm::RegionKind::ReclaimableBootData)
            && memory.reserve(high_image, kernel::mm::RegionKind::KernelImage);
    }
    return false;
}

} // namespace

namespace kernel::boot {

auto build_boot_info_from_fdt(
    BootInfo& info,
    CpuHardwareId boot_cpu,
    kernel::mm::PhysAddr fdt_physical,
    const void* fdt_pointer) noexcept -> libk::Expected<void, BootInfoError> {
    info.fdt = {};
    info.transition = {};
    info.module.reset();
    info.cpu.cpus.clear();
    info.cpu.boot_index = 0;
    info.timebase_frequency = 0;
    info.memory_regions.clear();

    kernel::boot::fdt::FDT_View view{};
    if (!kernel::boot::fdt::init_view(view, fdt_pointer)) {
        return libk::unexpected(BootInfoError::InvalidFdt);
    }

    if (!parse_fdt_cpus(view, boot_cpu, info.cpu)) {
        return libk::unexpected(BootInfoError::InvalidCpuTopology);
    }
    const auto timebase = parse_timebase_frequency(view);
    if (!timebase) {
        return libk::unexpected(BootInfoError::InvalidTimebase);
    }
    info.timebase_frequency = timebase.value();

    kernel::mm::BootMapBuilder memory{};
    BootCollector collector{memory};
    if (!kernel::boot::fdt::walk_view(view, collector)) {
        diag::console::print<"invalid FDT structure\n">();
        return libk::unexpected(BootInfoError::InvalidStructure);
    }

    const bool reservations_valid = kernel::boot::fdt::visit_memory_reservations(
        view,
        [&memory](uint64_t address, uint64_t size) {
            const auto range = Reg{address, size}.covering_pages();
            return range && static_cast<bool>(memory.reserve(
                *range,
                kernel::mm::RegionKind::FirmwareReserved));
        });
    if (!reservations_valid) {
        return libk::unexpected(BootInfoError::InvalidMemoryMap);
    }
    if (!reserve_kernel(memory)) {
        return libk::unexpected(BootInfoError::InvalidKernelRange);
    }

    auto module = collector.module();
    if (!module) {
        return libk::unexpected(module.error());
    }
    if (module.value()
        && !memory.reserve(
            module.value()->pages,
            kernel::mm::RegionKind::ReclaimableBootData)) {
        return libk::unexpected(BootInfoError::InvalidModuleRange);
    }

    const auto fdt_pages = kernel::mm::PageRange::covering_bytes(
        fdt_physical,
        view.size);
    if (!fdt_pages
        || !memory.reserve(
            *fdt_pages,
            kernel::mm::RegionKind::ReclaimableBootData)) {
        return libk::unexpected(BootInfoError::InvalidFdtRange);
    }

    const size_t ram_banks = memory.ram().size();
    if (!libk::move(memory).build_into(info.memory_regions)) {
        return libk::unexpected(BootInfoError::InvalidMemoryMap);
    }

    info.fdt = FdtSource{
        .physical = fdt_physical,
        .size = static_cast<uint32_t>(view.size),
        .pages = *fdt_pages,
    };
    info.transition = TransitionMemory{.pages = kernel::image::transition()};
    info.module = libk::move(module).value();

    const auto stdout = collector.stdout_path();
    if (stdout) {
        diag::console::print<"stdout-path(resolved)={}\n">(*stdout);
    } else {
        diag::console::print<"stdout-path(resolved)=notfound\n">();
    }
    diag::console::print<"ram_banks={:#x}\nmemory_regions={:#x}\n">(
        ram_banks, info.memory_regions.size());
    return libk::expected();
}

} // namespace kernel::boot
