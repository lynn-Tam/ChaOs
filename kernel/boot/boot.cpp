#include <boot/boot_info.hpp>
#include <boot/cpu_topology.hpp>
#include <boot/firmware/devicetree/fdt.hpp>
#include <boot/timebase.hpp>

#include <diag/console.hpp>
#include <libk/limits.hpp>
#include <libk/optional.hpp>
#include <libk/utility.hpp>
#include <mm/boot_map.hpp>
#include <core/kernel_image.hpp>

namespace kernel::boot {
namespace {

[[nodiscard]] auto before_colon(libk::StrView string) noexcept -> libk::StrView {
    for (size_t index = 0; index < string.size(); ++index) {
        if (string[index] == ':') {
            return libk::StrView{string.data(), index};
        }
    }
    return string;
}

struct Reg {
    uint64_t address{};
    uint64_t size{};

    [[nodiscard]] auto contained_pages() const noexcept
        -> libk::optional<mm::PageRange> {
        if (address > libk::numeric_limits<uintptr_t>::max()
            || size > libk::numeric_limits<size_t>::max()) {
            return libk::nullopt;
        }
        return mm::PageRange::contained_bytes(
            mm::PhysAddr{static_cast<uintptr_t>(address)},
            static_cast<size_t>(size));
    }

    [[nodiscard]] auto covering_pages() const noexcept
        -> libk::optional<mm::PageRange> {
        if (address > libk::numeric_limits<uintptr_t>::max()
            || size > libk::numeric_limits<size_t>::max()) {
            return libk::nullopt;
        }
        return mm::PageRange::covering_bytes(
            mm::PhysAddr{static_cast<uintptr_t>(address)},
            static_cast<size_t>(size));
    }
};

class RegFormat {
public:
    template<typename Visitor>
    [[nodiscard]] auto visit(libk::ByteSpan bytes, Visitor&& visitor) const noexcept -> bool {
        if (!valid()) {
            return false;
        }
        const size_t tuple_size = (address_cells_ + size_cells_) * sizeof(uint32_t);
        if (bytes.empty() || bytes.size() % tuple_size != 0) {
            return false;
        }

        libk::ByteReader reader{bytes.data(), bytes.size()};
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

    [[nodiscard]] auto set_address_cells(libk::ByteSpan bytes) noexcept -> bool {
        return set(bytes, address_cells_);
    }

    [[nodiscard]] auto set_size_cells(libk::ByteSpan bytes) noexcept -> bool {
        return set(bytes, size_cells_);
    }

    [[nodiscard]] auto read_address(
        libk::ByteSpan bytes,
        uint64_t& value) const noexcept -> bool {
        if (!valid() || bytes.size() != address_cells_ * sizeof(uint32_t)) {
            return false;
        }
        libk::ByteReader reader{bytes.data(), bytes.size()};
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

    [[nodiscard]] static auto set(libk::ByteSpan bytes, uint32_t& cells) noexcept -> bool {
        libk::ByteReader reader{bytes.data(), bytes.size()};
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

class BootTree final {
public:
    BootTree(const Fdt& tree,
             mm::BootMapBuilder& memory) noexcept
        : tree_(tree), memory_(memory) {}

    [[nodiscard]] auto read() noexcept -> bool {
        const auto root = tree_.root();
        if (const auto address = tree_.property(root, "#address-cells");
            address && !root_format_.set_address_cells(*address)) return false;
        if (const auto size = tree_.property(root, "#size-cells");
            size && !root_format_.set_size_cells(*size)) return false;

        for (auto node = tree_.first_child(root); node;
             node = tree_.next_sibling(*node)) {
            const auto name = tree_.node_name(*node);
            if (name == "memory" || name.starts_with("memory@")) {
                if (const auto reg = tree_.property(*node, "reg"); reg
                    && !root_format_.visit(*reg, [this](Reg value) {
                        const auto range = value.contained_pages();
                        return range && static_cast<bool>(memory_.add_ram(*range));
                    })) return false;
            } else if (name == "reserved-memory") {
                if (!read_reserved(*node)) return false;
            } else if (name == "soc") {
                if (!read_soc(*node)) return false;
            }
        }
        return read_chosen(root);
    }

    [[nodiscard]] auto iommu() const noexcept
        -> libk::optional<mm::PageRange> { return iommu_; }

    [[nodiscard]] auto stdout_path() const noexcept
        -> libk::optional<libk::StrView> { return stdout_path_; }

    [[nodiscard]] auto module() const noexcept
        -> libk::Expected<libk::optional<BootModule>,
                          BootInfoError> {
        if (!initrd_start_ && !initrd_end_) {
            return libk::expected(libk::optional<BootModule>{});
        }
        if (!initrd_start_ || !initrd_end_
            || *initrd_start_ >= *initrd_end_
            || *initrd_start_ > libk::numeric_limits<usize>::max()
            || *initrd_end_ - *initrd_start_ > libk::numeric_limits<usize>::max()) {
            return libk::unexpected(BootInfoError::InvalidModuleRange);
        }
        const mm::PhysAddr physical{static_cast<usize>(*initrd_start_)};
        const usize size = static_cast<usize>(*initrd_end_ - *initrd_start_);
        const auto pages = mm::PageRange::covering_bytes(physical, size);
        if (!pages) {
            return libk::unexpected(BootInfoError::InvalidModuleRange);
        }
        return libk::expected(libk::optional<BootModule>{
            BootModule{
                .physical = physical, .size = size, .pages = *pages,
                .kind = BootModuleKind::Bundle,
            }});
    }

private:
    [[nodiscard]] auto read_reserved(FdtNode parent) noexcept -> bool {
        RegFormat format = root_format_;
        if (const auto address = tree_.property(parent, "#address-cells");
            address && !format.set_address_cells(*address)) return false;
        if (const auto size = tree_.property(parent, "#size-cells");
            size && !format.set_size_cells(*size)) return false;
        for (auto node = tree_.first_child(parent); node;
             node = tree_.next_sibling(*node)) {
            if (const auto reg = tree_.property(*node, "reg"); reg
                && !format.visit(*reg, [this](Reg value) {
                    const auto range = value.covering_pages();
                    return range && static_cast<bool>(memory_.reserve(
                        *range, mm::RegionKind::FirmwareReserved));
                })) return false;
        }
        return true;
    }

    [[nodiscard]] auto read_soc(FdtNode parent) noexcept -> bool {
        RegFormat format = root_format_;
        if (const auto address = tree_.property(parent, "#address-cells");
            address && !format.set_address_cells(*address)) return false;
        if (const auto size = tree_.property(parent, "#size-cells");
            size && !format.set_size_cells(*size)) return false;
        const auto ranges = tree_.property(parent, "ranges");
        for (auto node = tree_.first_child(parent); node;
             node = tree_.next_sibling(*node)) {
            if (!tree_.node_name(*node).starts_with("iommu@")) continue;
            const auto compatible = tree_.property(*node, "compatible");
            const auto name = compatible
                ? Fdt::first_string(*compatible) : libk::nullopt;
            if (!name || *name != "riscv,iommu") continue;
            if (!ranges || !ranges->empty() || iommu_) return false;
            const auto reg = tree_.property(*node, "reg");
            if (!reg || !format.visit(*reg, [this](Reg value) {
                if (iommu_ || value.address % mm::page_size != 0
                    || value.size != mm::page_size) return false;
                iommu_ = value.contained_pages();
                return static_cast<bool>(iommu_);
            })) return false;
        }
        return true;
    }

    [[nodiscard]] auto read_chosen(FdtNode root) noexcept -> bool {
        const auto chosen = tree_.child(root, "chosen");
        if (!chosen) return true;
        if (const auto start = tree_.property(*chosen, "linux,initrd-start"); start
            && !root_format_.read_address(*start, initrd_start_.emplace())) return false;
        if (const auto end = tree_.property(*chosen, "linux,initrd-end"); end
            && !root_format_.read_address(*end, initrd_end_.emplace())) return false;
        const auto stdout = tree_.property(*chosen, "stdout-path");
        if (!stdout) return true;
        const auto path = Fdt::first_string(*stdout);
        if (!path) return false;
        const auto name = before_colon(*path);
        if (name.empty()) return true;
        if (name[0] == '/') {
            stdout_path_ = name;
        } else if (const auto aliases = tree_.child(root, "aliases")) {
            if (const auto alias = tree_.property(*aliases, name)) {
                stdout_path_ = Fdt::first_string(*alias);
            }
        }
        return true;
    }

    const Fdt& tree_;
    mm::BootMapBuilder& memory_;
    RegFormat root_format_{};
    libk::optional<mm::PageRange> iommu_{};
    libk::optional<libk::StrView> stdout_path_{};
    libk::optional<uint64_t> initrd_start_{};
    libk::optional<uint64_t> initrd_end_{};
};

[[nodiscard]] auto reserve_kernel(mm::BootMapBuilder& memory) noexcept -> bool {
    const auto boot_entry = image::boot_entry();
    const auto secondary = image::secondary_entry();
    const auto transition = image::transition();
    const auto high_image = image::physical_image();

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
                mm::PageRange{bank.first(), prefix_pages},
                mm::RegionKind::FirmwareReserved)) {
            return false;
        }
        return memory.reserve(boot_entry, mm::RegionKind::KernelImage)
            && memory.reserve(secondary, mm::RegionKind::KernelImage)
            && memory.reserve(
                transition, mm::RegionKind::ReclaimableBootData)
            && memory.reserve(high_image, mm::RegionKind::KernelImage);
    }
    return false;
}

} // namespace

auto build_boot_info_from_fdt(
    BootInfo& info,
    CpuHardwareId boot_cpu,
    mm::PhysAddr fdt_physical,
    const void* fdt_pointer) noexcept -> libk::Expected<void, BootInfoError> {
    info.fdt = {};
    info.transition = {};
    info.module.reset();
    info.cpu.cpus.clear();
    info.cpu.boot_index = 0;
    info.timebase_frequency = 0;
    info.iommu.reset();
    info.memory_regions.clear();

    auto opened = Fdt::open(fdt_pointer);
    if (!opened) {
        return libk::unexpected(BootInfoError::InvalidFdt);
    }
    const Fdt& tree = opened.value();

    if (!parse_fdt_cpus(tree, boot_cpu, info.cpu)) {
        return libk::unexpected(BootInfoError::InvalidCpuTopology);
    }
    const auto timebase = parse_timebase_frequency(tree);
    if (!timebase) {
        return libk::unexpected(BootInfoError::InvalidTimebase);
    }
    info.timebase_frequency = timebase.value();

    mm::BootMapBuilder memory{};
    BootTree parsed{tree, memory};
    if (!parsed.read()) {
        diag::console::print<"invalid FDT structure\n">();
        return libk::unexpected(BootInfoError::InvalidStructure);
    }
    info.iommu = parsed.iommu();

    const bool reservations_valid = tree.for_each_reservation(
        [&memory](uint64_t address, uint64_t size) {
            const auto range = Reg{address, size}.covering_pages();
            return range && static_cast<bool>(memory.reserve(
                *range,
                mm::RegionKind::FirmwareReserved));
        });
    if (!reservations_valid) {
        return libk::unexpected(BootInfoError::InvalidMemoryMap);
    }
    if (!reserve_kernel(memory)) {
        return libk::unexpected(BootInfoError::InvalidKernelRange);
    }

    auto module = parsed.module();
    if (!module) {
        return libk::unexpected(module.error());
    }
    if (module.value()
        && !memory.reserve(
            module.value()->pages,
            mm::RegionKind::ReclaimableBootData)) {
        return libk::unexpected(BootInfoError::InvalidModuleRange);
    }

    const auto fdt_pages = mm::PageRange::covering_bytes(
        fdt_physical,
        tree.size());
    if (!fdt_pages
        || !memory.reserve(
            *fdt_pages,
            mm::RegionKind::ReclaimableBootData)) {
        return libk::unexpected(BootInfoError::InvalidFdtRange);
    }

    const size_t ram_banks = memory.ram().size();
    if (!libk::move(memory).build_into(info.memory_regions)) {
        return libk::unexpected(BootInfoError::InvalidMemoryMap);
    }

    info.fdt = FdtSource{
        .physical = fdt_physical,
        .size = static_cast<uint32_t>(tree.size()),
        .pages = *fdt_pages,
    };
    info.transition = TransitionMemory{.pages = image::transition()};
    info.module = libk::move(module).value();

    const auto stdout = parsed.stdout_path();
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
