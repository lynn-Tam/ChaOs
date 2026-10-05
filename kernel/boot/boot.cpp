#include <expected>
#include <boot/info.hpp>
#include <boot/cpu_topology.hpp>
#include <boot/fdt.hpp>

#include <console.hpp>
#include <limits>
#include <optional>
#include <utility>
#include <algorithm>
#include <boot/link.hpp>

extern "C" {
extern char kernel_text_start[], kernel_text_end[];
extern char kernel_rodata_start[], kernel_rodata_end[];
extern char kernel_data_start[], kernel_data_end[];
extern char kernel_bss_start[], kernel_bss_end[];
extern char kernel_bootstack_start[], kernel_bootstack_end[];
}

auto kernel_root(mm::Pmm& pmm) noexcept -> std::expected<mm::PageTable, mm::PtErr> {
    using mm::PtPerm;
    using mm::PtErr;
    auto root = mm::PageTable::create(pmm, mm::PageTable::Kind::Kernel);
    if (!root) return std::unexpected(root.error());
    const struct Section {
        const char* begin;
        const char* end;
        PtPerm perms;
    } sections[] = {
        {kernel_text_start, kernel_text_end, PtPerm::Rx},
        {kernel_rodata_start, kernel_rodata_end, PtPerm::Ro},
        {kernel_data_start, kernel_data_end, PtPerm::Rw},
        {kernel_bss_start, kernel_bss_end, PtPerm::Rw},
        {kernel_bootstack_start, kernel_bootstack_end, PtPerm::Rw},
    };
    for (const auto& s : sections) {
        const usize begin = reinterpret_cast<usize>(s.begin), end = reinterpret_cast<usize>(s.end);
        libk_assert(begin <= end && begin % mm::page_size == 0 && end % mm::page_size == 0);
        if (begin == end) continue;
        auto physical = kernel_phys(mm::Virt{begin});
        if (!physical) return std::unexpected(PtErr::BadPhys);
        auto pages = mm::Pages::from_aligned_bytes(*physical, end - begin);
        if (!pages) return std::unexpected(PtErr::BadPhys);
        auto installed = root->map(mm::Virt{begin}, *pages, s.perms);
        if (!installed) return std::unexpected(installed.error());
    }
    for (auto pages : pmm.ram()) {
        auto va = pmm.virt(pages.base().base(), pages.page_count() * mm::page_size);
        libk_assert(va);
        auto installed = root->map(*va, pages, PtPerm::Rw);
        if (!installed) return std::unexpected(installed.error());
    }
    // MMIO has the same immutable resource inventory, but is not allocatable RAM.
    for (const auto& region : pmm.regions()) {
        if (region.is_ram()) continue;
        const auto pages = region.range;
        auto installed = root->map(mm::Virt{mm::DirectBegin + pages.base().base().raw()}, pages, PtPerm::Rw);
        if (!installed) return std::unexpected(installed.error());
    }
    // Secondaries enter physically; these leaves bridge to their high entry.
    for (auto pages : {boot_pages(), secondary_pages()}) {
        auto installed = root->map(mm::Virt{pages.base().base().raw()}, pages, PtPerm::Rx);
        if (!installed) return std::unexpected(installed.error());
    }
    return root;
}

auto BootMap::add_ram(mm::Pages r) noexcept -> std::expected<void, Err> {
    if (!r.valid()) return std::unexpected(Err::Invalid);
    if (!input_.try_emplace_back(mm::Region{r, mm::Region::Kind::Ram}))
        return std::unexpected(Err::Capacity);
    return {};
}

auto BootMap::reserve(mm::Pages r, mm::Region::Kind kind) noexcept -> std::expected<void, Err> {
    if (!r.valid() || kind == mm::Region::Kind::Ram || kind == mm::Region::Kind::Mmio)
        return std::unexpected(Err::Invalid);
    if (!input_.try_emplace_back(mm::Region{r, kind}))
        return std::unexpected(Err::Capacity);
    return {};
}

auto BootMap::finish(mm::RegionList& out) && noexcept -> std::expected<void, Err> {
    out.clear();
    auto fail = [&](Err e) -> std::expected<void, Err> {
        out.clear();
        return std::unexpected(e);
    };
    std::ranges::sort(input_, {}, [](const mm::Region& r) { return r.range.base(); });
    bool found = false;
    mm::Pages previous{};
    for (const auto& source : input_) {
        if (source.kind != mm::Region::Kind::Ram) continue;
        const auto bank = source.range;
        if (previous.valid() && previous.intersects(bank)) return fail(Err::Overlap);
        found = true;
        previous = bank;
        auto end = *bank.limit();
        for (auto p = bank.base(); p < end;) {
            auto next = end;
            mm::Region::Kind kind = mm::Region::Kind::Ram;
            for (const auto& r : input_) {
                // Ram < Boot < Firmware < Kernel. Keep enum order semantic,
                // rather than encoding precedence in public classification.
                auto rank = [](mm::Region::Kind k) {
                    return k == mm::Region::Kind::Kernel ? 3 : k == mm::Region::Kind::Firmware ? 2
                         : k == mm::Region::Kind::Boot ? 1 : 0;
                };
                auto last = *r.range.limit();
                if (p < r.range.base()) next = std::min(next, r.range.base());
                if (p < last) next = std::min(next, last);
                if (r.range.contains(p) && rank(r.kind) > rank(kind)) kind = r.kind;
            }
            auto n = next.raw() - p.raw();
            // Boot runs preserve individual reclaimable resource boundaries.
            if (!out.empty() && kind != mm::Region::Kind::Boot && out.back().kind == kind
                && *out.back().range.limit() == p) {
                auto& r = out.back().range;
                r = mm::Pages{r.base(), r.page_count() + n};
            } else if (!out.try_emplace_back(mm::Region{mm::Pages{p, n}, kind})) {
                return fail(Err::Capacity);
            }
            p = next;
        }
    }
    return found ? std::expected<void, Err>{} : fail(Err::NoRam);
}

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
        -> std::optional<mm::Pages> {
        if (address > std::numeric_limits<uintptr_t>::max()
            || size > std::numeric_limits<size_t>::max()) {
            return std::nullopt;
        }
        return mm::Pages::contained_bytes(
            mm::Phys{static_cast<uintptr_t>(address)},
            static_cast<size_t>(size));
    }

    [[nodiscard]] auto covering_pages() const noexcept
        -> std::optional<mm::Pages> {
        if (address > std::numeric_limits<uintptr_t>::max()
            || size > std::numeric_limits<size_t>::max()) {
            return std::nullopt;
        }
        return mm::Pages::covering_bytes(
            mm::Phys{static_cast<uintptr_t>(address)},
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
             BootMap& memory) noexcept
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
        -> std::optional<mm::Pages> { return iommu_; }

    [[nodiscard]] auto stdout_path() const noexcept
        -> std::optional<libk::StrView> { return stdout_path_; }

    [[nodiscard]] auto module() const noexcept
        -> std::expected<std::optional<BootModule>,
                          BootInfoError> {
        if (!initrd_start_ && !initrd_end_) {
            return (std::optional<BootModule>{});
        }
        if (!initrd_start_ || !initrd_end_
            || *initrd_start_ >= *initrd_end_
            || *initrd_start_ > std::numeric_limits<usize>::max()
            || *initrd_end_ - *initrd_start_ > std::numeric_limits<usize>::max()) {
            return std::unexpected(BootInfoError::InvalidModuleRange);
        }
        const mm::Phys physical{static_cast<usize>(*initrd_start_)};
        const usize size = static_cast<usize>(*initrd_end_ - *initrd_start_);
        const auto pages = mm::Pages::covering_bytes(physical, size);
        if (!pages) {
            return std::unexpected(BootInfoError::InvalidModuleRange);
        }
        return (std::optional<BootModule>{
            BootModule{
                .physical = physical, .size = size, .pages = *pages,
            }});
    }

    [[nodiscard]] auto timebase() const noexcept -> std::optional<u64> {
        const auto cpus = tree_.child(tree_.root(), "cpus");
        u32 hz{};
        if (!cpus || tree_.property_count(*cpus, "timebase-frequency") != 1
            || !Fdt::read_u32(*tree_.property(*cpus, "timebase-frequency"), hz)
            || hz == 0) return std::nullopt;
        return hz;
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
                        *range, mm::Region::Kind::Firmware));
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
                ? Fdt::first_string(*compatible) : std::nullopt;
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
    BootMap& memory_;
    RegFormat root_format_{};
    std::optional<mm::Pages> iommu_{};
    std::optional<libk::StrView> stdout_path_{};
    std::optional<uint64_t> initrd_start_{};
    std::optional<uint64_t> initrd_end_{};
};

[[nodiscard]] auto reserve_kernel(BootMap& memory) noexcept -> bool {
    const auto boot_entry = boot_pages();
    const auto secondary = secondary_pages();
    const auto transition = transition_pages();
    const auto high_image = kernel_pages();

    for (const auto& bank : memory.ram()) {
        if (!bank.contains(boot_entry)
            || !bank.contains(secondary)
            || !bank.contains(transition)
            || !bank.contains(high_image)) {
            continue;
        }
        const size_t prefix_pages = boot_entry.base().raw()
            - bank.base().raw();
        if (prefix_pages != 0
            && !memory.reserve(
                mm::Pages{bank.base(), prefix_pages},
                mm::Region::Kind::Firmware)) {
            return false;
        }
        return memory.reserve(boot_entry, mm::Region::Kind::Kernel)
            && memory.reserve(secondary, mm::Region::Kind::Kernel)
            && memory.reserve(
                transition, mm::Region::Kind::Boot)
            && memory.reserve(high_image, mm::Region::Kind::Kernel);
    }
    return false;
}

} // namespace

auto build_boot_info_from_fdt(
    BootInfo& info,
    mm::RegionList& regions,
    CpuHwId boot_cpu,
    mm::Phys fdt_physical,
    const void* fdt_pointer) noexcept -> std::expected<void, BootInfoError> {
    info.fdt = {};
    info.transition = {};
    info.module.reset();
    info.cpu.cpus.clear();
    info.cpu.boot_index = 0;
    info.timebase_frequency = 0;
    info.iommu.reset();
    regions.clear();

    auto opened = Fdt::open(fdt_pointer);
    if (!opened) {
        return std::unexpected(BootInfoError::InvalidFdt);
    }
    const Fdt& tree = opened.value();

    if (!parse_fdt_cpus(tree, boot_cpu, info.cpu)) {
        return std::unexpected(BootInfoError::InvalidCpuTopology);
    }
    BootMap memory{};
    BootTree parsed{tree, memory};
    const auto timebase = parsed.timebase();
    if (!timebase) {
        return std::unexpected(BootInfoError::InvalidTimebase);
    }
    info.timebase_frequency = *timebase;

    if (!parsed.read()) {
        console::print<"invalid FDT structure\n">();
        return std::unexpected(BootInfoError::InvalidStructure);
    }
    info.iommu = parsed.iommu();

    const bool reservations_valid = tree.for_each_reservation(
        [&memory](uint64_t address, uint64_t size) {
            const auto range = Reg{address, size}.covering_pages();
            return range && static_cast<bool>(memory.reserve(
                *range,
                mm::Region::Kind::Firmware));
        });
    if (!reservations_valid) {
        return std::unexpected(BootInfoError::InvalidMemoryMap);
    }
    if (!reserve_kernel(memory)) {
        return std::unexpected(BootInfoError::InvalidKernelRange);
    }

    auto module = parsed.module();
    if (!module) {
        return std::unexpected(module.error());
    }
    if (module.value()
        && !memory.reserve(
            module.value()->pages,
            mm::Region::Kind::Boot)) {
        return std::unexpected(BootInfoError::InvalidModuleRange);
    }

    const auto fdt_pages = mm::Pages::covering_bytes(
        fdt_physical,
        tree.size());
    if (!fdt_pages
        || !memory.reserve(
            *fdt_pages,
            mm::Region::Kind::Boot)) {
        return std::unexpected(BootInfoError::InvalidFdtRange);
    }

    const size_t ram_banks = std::ranges::distance(memory.ram());
    if (!std::move(memory).finish(regions)) {
        return std::unexpected(BootInfoError::InvalidMemoryMap);
    }

    info.fdt = FdtSource{
        .physical = fdt_physical,
        .size = static_cast<uint32_t>(tree.size()),
        .pages = *fdt_pages,
    };
    info.transition = transition_pages();
    info.module = std::move(module).value();

    const auto stdout = parsed.stdout_path();
    if (stdout) {
        console::print<"stdout-path(resolved)={}\n">(*stdout);
    } else {
        console::print<"stdout-path(resolved)=notfound\n">();
    }
    console::print<"ram_banks={:#x}\nmemory_regions={:#x}\n">(
        ram_banks, regions.size());
    return {};
}
