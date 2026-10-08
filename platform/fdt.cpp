#include <expected>
#include <optional>
#include "fdt.hpp"
#include <boot/info.hpp>
#include <console.hpp>
#include <algorithm>
#include <limits>
#include <utility>

#include <libk/assert.hpp>


inline constexpr uint32_t FdtMagic = 0xd00dfeed;
inline constexpr uint32_t SupportedVersion = 17;
inline constexpr uint32_t HeaderSize = 10 * sizeof(uint32_t);

inline constexpr uint32_t BeginNodeToken = 0x00000001;
inline constexpr uint32_t EndNodeToken = 0x00000002;
inline constexpr uint32_t PropertyToken = 0x00000003;
inline constexpr uint32_t NopToken = 0x00000004;
inline constexpr uint32_t EndToken = 0x00000009;

[[nodiscard]] auto align_structure(
    libk::ByteReader& reader) noexcept -> bool {

    constexpr size_t Alignment = sizeof(uint32_t);
    const size_t remainder = reader.offset() % Alignment;

    return remainder == 0
        || reader.skip(Alignment - remainder);
}


struct Fdt::Item final {
    enum class Kind : uint8_t {
        BeginNode,
        EndNode,
        Property,
        Nop,
        End,
    };

    Kind kind{};
    uint32_t offset{};
    uint32_t next_offset{};
    libk::StrView name{};
    libk::ByteSpan value{};
};

auto Fdt::open(const void* dtb) noexcept
    -> std::expected<Fdt, FdtError> {

    if (dtb == nullptr) {
        return std::unexpected(FdtError::InvalidHeader);
    }

    const auto* base = static_cast<const uint8_t*>(dtb);

    if (reinterpret_cast<uintptr_t>(base) % sizeof(uint64_t) != 0) {
        return std::unexpected(FdtError::InvalidHeader);
    }

    libk::ByteReader header{base, HeaderSize};

    uint32_t magic{};
    uint32_t total_size{};
    uint32_t structure_offset{};
    uint32_t strings_offset{};
    uint32_t reservations_offset{};
    uint32_t version{};
    uint32_t last_compatible_version{};
    uint32_t boot_cpu_id{};
    uint32_t strings_size{};
    uint32_t structure_size{};

    if (!header.read_be32(magic)
        || !header.read_be32(total_size)
        || !header.read_be32(structure_offset)
        || !header.read_be32(strings_offset)
        || !header.read_be32(reservations_offset)
        || !header.read_be32(version)
        || !header.read_be32(last_compatible_version)
        || !header.read_be32(boot_cpu_id)
        || !header.read_be32(strings_size)
        || !header.read_be32(structure_size)) {
        return std::unexpected(FdtError::InvalidHeader);
    }

    (void)boot_cpu_id;

    auto within = [total_size](uint32_t offset, uint32_t size) {
        return offset >= HeaderSize
            && offset <= total_size
            && size <= total_size - offset;
    };

    if (magic != FdtMagic
        || version < SupportedVersion
        || last_compatible_version > SupportedVersion
        || last_compatible_version > version
        || total_size < HeaderSize
        || reservations_offset % sizeof(uint64_t) != 0
        || structure_offset % sizeof(uint32_t) != 0
        || structure_size < sizeof(uint32_t)
        || !within(structure_offset, structure_size)
        || !within(strings_offset, strings_size)) {
        return std::unexpected(FdtError::InvalidHeader);
    }

    // DTSpec places reservations, structure, then strings after the header.
    if (reservations_offset < HeaderSize
        || structure_offset < reservations_offset
        || structure_offset - reservations_offset
            < 2 * sizeof(uint64_t)
        || strings_offset < structure_offset
        || structure_size > strings_offset - structure_offset) {
        return std::unexpected(FdtError::InvalidHeader);
    }

    Fdt result{
        libk::ByteSpan{base, total_size},
        libk::ByteSpan{base + structure_offset, structure_size},
        libk::ByteSpan{base + strings_offset, strings_size},
        libk::ByteSpan{
            base + reservations_offset,
            structure_offset - reservations_offset,
        },
    };

    if (!result.validate_structure()) {
        return std::unexpected(FdtError::InvalidStructure);
    }

    size_t reservations_size{};
    if (!result.validate_reservations(reservations_size)) {
        return std::unexpected(FdtError::InvalidReservations);
    }

    result.reservations_ = libk::ByteSpan{
        result.reservations_.data(),
        reservations_size,
    };

    return (result);
}

auto Fdt::string_at(
    uint32_t offset,
    libk::StrView& string) const noexcept -> bool {

    if (offset >= strings_.size()) {
        return false;
    }

    libk::ByteReader reader{
        strings_.data() + offset,
        strings_.size() - offset,
    };

    return reader.read_cstr(string);
}

auto Fdt::read_item(
    uint32_t offset,
    Item& item) const noexcept -> bool {

    if (offset >= structure_.size()
        || offset % sizeof(uint32_t) != 0) {
        return false;
    }

    libk::ByteReader reader{
        structure_.data(),
        structure_.size(),
    };

    if (!reader.skip(offset)) {
        return false;
    }

    uint32_t token{};
    if (!reader.read_be32(token)) {
        return false;
    }

    item = {};
    item.offset = offset;

    switch (token) {
    case BeginNodeToken:
        item.kind = Item::Kind::BeginNode;
        if (!reader.read_cstr(item.name)
            || !align_structure(reader)) {
            return false;
        }
        break;

    case EndNodeToken:
        item.kind = Item::Kind::EndNode;
        break;

    case PropertyToken: {
        item.kind = Item::Kind::Property;

        uint32_t length{};
        uint32_t name_offset{};

        if (!reader.read_be32(length)
            || !reader.read_be32(name_offset)
            || !string_at(name_offset, item.name)
            || !reader.take_bytes(length, item.value)
            || !align_structure(reader)) {
            return false;
        }
        break;
    }

    case NopToken:
        item.kind = Item::Kind::Nop;
        break;

    case EndToken:
        item.kind = Item::Kind::End;
        break;

    default:
        return false;
    }

    item.next_offset =
        static_cast<uint32_t>(reader.offset());

    return true;
}

auto Fdt::validate_structure() noexcept -> bool {
    uint32_t offset = 0;
    int depth = -1;
    bool saw_root = false;

    for (;;) {
        Item item{};
        if (!read_item(offset, item)) {
            return false;
        }

        switch (item.kind) {
        case Item::Kind::BeginNode:
            if (depth == -1) {
                if (saw_root || !item.name.empty()) {
                    return false;
                }
                root_offset_ = item.offset;
                saw_root = true;
            }
            ++depth;
            break;

        case Item::Kind::EndNode:
            if (depth < 0) {
                return false;
            }
            --depth;
            break;

        case Item::Kind::Property:
            if (depth < 0) {
                return false;
            }
            break;

        case Item::Kind::Nop:
            break;

        case Item::Kind::End:
            return saw_root
                && depth == -1
                && item.next_offset == structure_.size();
        }

        offset = item.next_offset;
    }
}

auto Fdt::validate_reservations(
    size_t& actual_size) const noexcept -> bool {

    libk::ByteReader reader{
        reservations_.data(),
        reservations_.size(),
    };

    for (;;) {
        uint64_t address{};
        uint64_t size{};

        if (!reader.read_be64(address)
            || !reader.read_be64(size)) {
            return false;
        }

        if (address == 0 && size == 0) {
            actual_size = reader.offset();
            return true;
        }
    }
}

auto Fdt::node_name(u32 node) const noexcept
    -> libk::StrView {

    Item item{};
    const bool valid =
        read_item(node, item)
        && item.kind == Item::Kind::BeginNode;

    libk_assert(valid);
    return item.name;
}

auto Fdt::property(u32 node, libk::StrView name) const noexcept -> std::optional<libk::ByteSpan> {
    Item item{};
    if (!read_item(node, item) || item.kind != Item::Kind::BeginNode) return {};
    int depth = 0;
    std::optional<libk::ByteSpan> value;
    for (u32 off = item.next_offset; read_item(off, item); off = item.next_offset) {
        switch (item.kind) {
        case Item::Kind::BeginNode: ++depth; break;
        case Item::Kind::EndNode: if (!depth--) return value; break;
        case Item::Kind::Property:
            if (!depth && item.name == name) {
                if (value) return {};
                value = item.value;
            }
            break;
        case Item::Kind::End: return {};
        case Item::Kind::Nop: break;
        }
    }
    return {};
}

auto Fdt::first_child(u32 parent) const noexcept
    -> std::optional<u32> {

    Item begin{};
    if (!read_item(parent, begin)
        || begin.kind != Item::Kind::BeginNode) {
        return std::nullopt;
    }

    uint32_t offset = begin.next_offset;

    for (;;) {
        Item item{};
        if (!read_item(offset, item)) {
            return std::nullopt;
        }

        switch (item.kind) {
        case Item::Kind::BeginNode:
            return item.offset;

        case Item::Kind::EndNode:
        case Item::Kind::End:
            return std::nullopt;

        case Item::Kind::Property:
        case Item::Kind::Nop:
            offset = item.next_offset;
            break;
        }
    }
}

auto Fdt::next_sibling(u32 node) const noexcept
    -> std::optional<u32> {

    Item begin{};
    if (!read_item(node, begin)
        || begin.kind != Item::Kind::BeginNode) {
        return std::nullopt;
    }

    uint32_t offset = begin.next_offset;
    int depth = 0;

    // Skip the current node's full subtree first.
    for (;;) {
        Item item{};
        if (!read_item(offset, item)) {
            return std::nullopt;
        }

        if (item.kind == Item::Kind::BeginNode) {
            ++depth;
        } else if (item.kind == Item::Kind::EndNode) {
            if (depth == 0) {
                offset = item.next_offset;
                break;
            }
            --depth;
        } else if (item.kind == Item::Kind::End) {
            return std::nullopt;
        }

        offset = item.next_offset;
    }

    // After the current node, the next BEGIN_NODE is its sibling.
    // END_NODE means the parent has ended.
    for (;;) {
        Item item{};
        if (!read_item(offset, item)) {
            return std::nullopt;
        }

        switch (item.kind) {
        case Item::Kind::BeginNode:
            return item.offset;

        case Item::Kind::EndNode:
        case Item::Kind::End:
            return std::nullopt;

        case Item::Kind::Property:
        case Item::Kind::Nop:
            offset = item.next_offset;
            break;
        }
    }
}

auto Fdt::child(
    u32 parent,
    libk::StrView name) const noexcept
    -> std::optional<u32> {

    for (auto node = first_child(parent);
         node;
         node = next_sibling(*node)) {

        if (node_name(*node) == name) {
            return node;
        }
    }

    return std::nullopt;
}

auto Fdt::read_u32(
    libk::ByteSpan bytes,
    uint32_t& value) noexcept -> bool {

    if (bytes.size() != sizeof(uint32_t)) {
        return false;
    }

    libk::ByteReader reader{
        bytes.data(),
        bytes.size(),
    };

    return reader.read_be32(value);
}

auto Fdt::first_string(
    libk::ByteSpan bytes) noexcept
    -> std::optional<libk::StrView> {

    libk::ByteReader reader{
        bytes.data(),
        bytes.size(),
    };

    libk::StrView string{};

    if (!reader.read_cstr(string)) {
        return std::nullopt;
    }

    return string;
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

static auto read_cells(libk::ByteReader& r, u32 n, u64& value) noexcept -> bool {
    if (n > 2) return false;
    value = 0;
    while (n--) {
        u32 word;
        if (!r.read_be32(word)) return false;
        value = (value << 32) | word;
    }
    return true;
}
class RegFormat {
public:
    template<class F> auto visit(libk::ByteSpan bytes, F&& f) const noexcept -> bool {
        libk::ByteReader r{bytes.data(), bytes.size()};
        if (bytes.empty()) return false;
        while (r.remaining()) {
            Reg reg;
            if (!read_cells(r, addr_, reg.address) || !read_cells(r, size_, reg.size) || !f(reg)) return false;
        }
        return true;
    }
    auto set_address_cells(libk::ByteSpan b) noexcept -> bool { return set(b, addr_); }
    auto set_size_cells(libk::ByteSpan b) noexcept -> bool { return set(b, size_); }
    auto read_address(libk::ByteSpan bytes, u64& value) const noexcept -> bool {
        libk::ByteReader r{bytes.data(), bytes.size()};
        return bytes.size() == addr_ * 4 && read_cells(r, addr_, value);
    }
private:
    static auto set(libk::ByteSpan b, u32& cells) noexcept -> bool {
        u32 n;
        if (!Fdt::read_u32(b, n) || !n || n > 2) return false;
        cells = n;
        return true;
    }
    u32 addr_{2}, size_{2};
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
            }
        }
        return read_chosen(root);
    }

    [[nodiscard]] auto module() const noexcept
        -> std::expected<std::optional<BootModule>,
                          FdtError> {
        if (!initrd_start_ && !initrd_end_) {
            return (std::optional<BootModule>{});
        }
        if (!initrd_start_ || !initrd_end_
            || *initrd_start_ >= *initrd_end_
            || *initrd_start_ > std::numeric_limits<usize>::max()
            || *initrd_end_ - *initrd_start_ > std::numeric_limits<usize>::max()) {
            return std::unexpected(FdtError::InvalidStructure);
        }
        const mm::Phys physical{static_cast<usize>(*initrd_start_)};
        const usize size = static_cast<usize>(*initrd_end_ - *initrd_start_);
        const auto pages = mm::Pages::covering_bytes(physical, size);
        if (!pages) {
            return std::unexpected(FdtError::InvalidStructure);
        }
        return (std::optional<BootModule>{
            BootModule{
                .physical = physical, .size = size, .pages = *pages,
            }});
    }

    [[nodiscard]] auto timebase() const noexcept -> std::optional<u64> {
        const auto cpus = tree_.child(tree_.root(), "cpus");
        const auto hz = cpus ? tree_.number(*cpus, "timebase-frequency") : std::nullopt;
        return hz && *hz ? hz : std::nullopt;
    }

private:
    [[nodiscard]] auto read_reserved(u32 parent) noexcept -> bool {
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

    [[nodiscard]] auto read_chosen(u32 root) noexcept -> bool {
        const auto chosen = tree_.child(root, "chosen");
        if (!chosen) return true;
        if (const auto start = tree_.property(*chosen, "linux,initrd-start"); start
            && !root_format_.read_address(*start, initrd_start_.emplace())) return false;
        if (const auto end = tree_.property(*chosen, "linux,initrd-end"); end
            && !root_format_.read_address(*end, initrd_end_.emplace())) return false;
        return true;
    }

    const Fdt& tree_;
    BootMap& memory_;
    RegFormat root_format_{};
    std::optional<uint64_t> initrd_start_{};
    std::optional<uint64_t> initrd_end_{};
};

[[nodiscard]] auto reserve_kernel(BootMap& memory) noexcept -> bool {
    const auto boot_entry = boot_layout.entry.pages();
    const auto secondary = boot_layout.secondary.pages();
    const auto transition = boot_layout.scratch.pages();
    const auto high_image = boot_layout.kernel.pages();

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
        return memory.reserve(boot_entry, mm::Region::Kind::Boot)
            && memory.reserve(secondary, mm::Region::Kind::Kernel)
            && memory.reserve(
                transition, mm::Region::Kind::Boot)
            && memory.reserve(high_image, mm::Region::Kind::Kernel);
    }
    return false;
}


// Firmware status is boot input, not a second runtime CPU state machine.
static auto parse_cpus(const Fdt& tree, CpuHwId boot,
                       libk::InplaceVector<CpuHwId, MaxCpus>& out) noexcept -> bool {
    const auto node = tree.child(tree.root(), "cpus");
    if (!node) return false;
    u32 cells{}, sizes{};
    const auto address = tree.property(*node, "#address-cells");
    const auto size = tree.property(*node, "#size-cells");
    if (!address
        || !Fdt::read_u32(*address, cells) || cells < 1 || cells > 2
        || !size
        || !Fdt::read_u32(*size, sizes) || sizes != 0) return false;
    for (auto cpu = tree.first_child(*node); cpu; cpu = tree.next_sibling(*cpu)) {
        const auto name = tree.node_name(*cpu);
        if (name != "cpu" && !name.starts_with("cpu@")) continue;
        const auto type = tree.property(*cpu, "device_type");
        const auto reg = tree.property(*cpu, "reg");
        if (!type
            || Fdt::first_string(*type) != "cpu"
            || !reg
            || reg->size() != cells * sizeof(u32)) return false;
        libk::ByteReader reader{reg->data(), reg->size()};
        u64 raw{};
        for (u32 i = 0; i < cells; ++i) {
            u32 cell{};
            if (!reader.read_be32(cell)) return false;
            raw = (raw << 32) | cell;
        }
        if (raw > std::numeric_limits<usize>::max()) return false;
        const CpuHwId id{static_cast<usize>(raw)};
        if (const auto status = tree.property(*cpu, "status")) {
            const auto text = Fdt::first_string(*status);
            if (!text) return false;
            if (*text != "okay" && *text != "ok") {
                if (*text != "disabled" && *text != "fail" && !text->starts_with("fail-")) return false;
                continue;
            }
        }
        for (const auto prev : out) if (prev == id) return false;
        if (!out.try_push_back(id)) return false;
    }
    const auto first = std::find(out.begin(), out.end(), boot);
    if (first == out.end()) return false;
    std::iter_swap(out.begin(), first);
    return true;
}

auto Fdt::parent(u32 node) const noexcept -> std::optional<u32> {
    if (node == root()) return {};
    // Two linear passes avoid a second tree/index or a fixed-depth stack.
    int depth = 0, target = -1;
    for (u32 off = root(); off <= node;) {
        Item item{};
        if (!read_item(off, item)) return {};
        if (item.kind == Item::Kind::BeginNode) {
            if (off == node) { target = depth - 1; break; }
            ++depth;
        } else if (item.kind == Item::Kind::EndNode) --depth;
        off = item.next_offset;
    }
    std::optional<u32> result;
    depth = 0;
    for (u32 off = root(); off < node;) {
        Item item{};
        if (!read_item(off, item)) return {};
        if (item.kind == Item::Kind::BeginNode) {
            if (depth == target) result = off;
            ++depth;
        } else if (item.kind == Item::Kind::EndNode) --depth;
        off = item.next_offset;
    }
    return result;
}
auto Fdt::number(u32 node, libk::StrView name) const noexcept -> std::optional<u32> {
    const auto p = property(node, name);
    u32 value;
    return p && read_u32(*p, value) ? std::optional{value} : std::nullopt;
}
auto Fdt::matches(u32 node, libk::StrView name) const noexcept -> bool {
    const auto p = property(node, "compatible");
    if (!p) return false;
    libk::ByteReader r{p->data(), p->size()};
    libk::StrView s;
    while (r.remaining() && r.read_cstr(s)) if (s == name) return true;
    return false;
}
auto Fdt::find(libk::StrView name) const noexcept -> std::optional<u32> {
    std::optional<u32> found;
    for (u32 off = root(); off < structure_.size();) {
        Item item{};
        if (!read_item(off, item)) return {};
        if (item.kind == Item::Kind::BeginNode && matches(off, name)) {
            bool enabled = true;
            for (auto n = std::optional{off}; n; n = parent(*n)) {
                const auto p = property(*n, "status");
                const auto status = p ? first_string(*p) : std::nullopt;
                if (p && (!status || (*status != "okay" && *status != "ok"))) enabled = false;
            }
            if (enabled) {
                if (found) return {}; // This connector supports one instance of each protocol.
                found = off;
            }
        }
        off = item.next_offset;
    }
    return found;
}
auto Fdt::phandle(u32 value) const noexcept -> std::optional<u32> {
    if (!value || value == UINT32_MAX) return {};
    for (u32 off = root(); off < structure_.size();) {
        Item item{};
        if (!read_item(off, item)) return {};
        if (item.kind == Item::Kind::BeginNode && number(off, "phandle") == value) return off;
        off = item.next_offset;
    }
    return {};
}
auto Fdt::path(libk::StrView path) const noexcept -> std::optional<u32> {
    usize end = 0;
    while (end < path.size() && path.data()[end] != ':') ++end;
    path = path.substr(0, end);
    if (path.empty()) return {};
    if (path.data()[0] != '/') {
        const auto aliases = child(root(), "aliases");
        const auto p = aliases ? property(*aliases, path) : std::nullopt;
        const auto s = p ? first_string(*p) : std::nullopt;
        if (!s || s->empty() || s->data()[0] != '/') return {};
        path = *s;
    }
    u32 node = root();
    for (usize begin = 1; begin < path.size();) {
        end = begin;
        while (end < path.size() && path.data()[end] != '/') ++end;
        const auto next = child(node, path.substr(begin, end - begin));
        if (!next) return {};
        node = *next;
        begin = end + 1;
    }
    return node;
}

static auto cell_count(const Fdt& t, u32 node, libk::StrView key, u32 fallback) noexcept -> u32 {
    return t.property(node, key) ? t.number(node, key).value_or(UINT32_MAX) : fallback;
}
static auto translate(const Fdt& t, u32 bus, FwRange range) noexcept -> std::optional<FwRange> {
    while (const auto parent = t.parent(bus)) {
        const auto ranges = t.property(bus, "ranges");
        if (!ranges) return {};
        if (!ranges->empty()) {
            libk::ByteReader r{ranges->data(), ranges->size()};
            bool found = false;
            while (r.remaining()) {
                u64 child, pa, size;
                if (!read_cells(r, cell_count(t, bus, "#address-cells", 2), child) ||
                    !read_cells(r, cell_count(t, *parent, "#address-cells", 2), pa) ||
                    !read_cells(r, cell_count(t, bus, "#size-cells", 1), size)) return {};
                if (range.pa < child || range.pa - child > size || range.size > size - (range.pa - child)) continue;
                if (pa > UINT64_MAX - (range.pa - child)) return {};
                range.pa = pa + (range.pa - child);
                found = true;
                break;
            }
            if (!found) return {};
        }
        bus = *parent;
    }
    if (!range.size || range.pa >= mm::DirectSize || range.size > mm::DirectSize - range.pa) return {};
    return range;
}
static auto register_range(const Fdt& t, u32 node) noexcept -> std::optional<FwRange> {
    const auto parent = t.parent(node);
    const auto reg = t.property(node, "reg");
    if (!parent || !reg) return {};
    libk::ByteReader r{reg->data(), reg->size()};
    u64 pa, size;
    if (!read_cells(r, cell_count(t, *parent, "#address-cells", 2), pa) ||
        !read_cells(r, cell_count(t, *parent, "#size-cells", 1), size) || r.remaining()) return {};
    return translate(t, *parent, {pa, size});
}
static auto wired_irq(const Fdt& t, u32 node, u32 plic, u32 count) noexcept -> std::optional<u32> {
    std::optional<u32> parent;
    for (auto n = std::optional{node}; n && !parent; n = t.parent(*n)) parent = t.number(*n, "interrupt-parent");
    const auto p = t.property(node, "interrupts");
    if (parent != plic || !p) return {};
    // Vector zero is sufficient; IOMMU directs its fault queue to that vector.
    libk::ByteReader r{p->data(), p->size()};
    u32 id;
    return r.read_be32(id) && id && id <= count ? std::optional{id} : std::nullopt;
}
auto FwPci::irq(u16 rid, u8 pin) const noexcept -> std::optional<u32> {
    const std::array<u32, 4> key{u32{rid} << 8, 0, 0, pin};
    libk::ByteReader r{routes.data(), routes.size()};
    while (r.remaining()) {
        bool match = true;
        u32 word, parent, id;
        for (usize i = 0; i < key.size(); ++i) {
            if (!r.read_be32(word)) return {};
            match &= (word & mask[i]) == (key[i] & mask[i]);
        }
        if (!r.read_be32(parent) || !r.read_be32(id)) return {};
        if (match) return id;
    }
    return {};
}
auto FwPci::device(u16 rid) const noexcept -> std::optional<u16> {
    libk::ByteReader r{devices.data(), devices.size()};
    while (r.remaining()) {
        u32 first, parent, output, count;
        if (!r.read_be32(first) || !r.read_be32(parent) || !r.read_be32(output) || !r.read_be32(count)) return {};
        if (parent == iommu && rid >= first && u32{rid} - first < count && output <= 255 && rid - first <= 255 - output)
            return static_cast<u16>(output + rid - first);
    }
    return {};
}
static auto read_hw(const Fdt& t, CpuHwId hart, FwHw& hw) noexcept -> bool {
    const auto plic = t.find("sifive,plic-1.0.0");
    const auto chosen = t.child(t.root(), "chosen");
    const auto stdout_path = chosen ? t.property(*chosen, "stdout-path") : std::nullopt;
    const auto name = stdout_path ? Fdt::first_string(*stdout_path) : std::nullopt;
    const auto uart = name ? t.path(*name) : std::nullopt;
    if (!plic || !uart || !t.matches(*uart, "ns16550a") ||
        cell_count(t, *uart, "reg-shift", 0) != 0 || cell_count(t, *uart, "reg-io-width", 1) != 1 ||
        t.number(*plic, "#interrupt-cells") != 1 || t.number(*plic, "#address-cells") != 0) return false;
    const auto plic_id = t.number(*plic, "phandle");
    const auto nirq = t.number(*plic, "riscv,ndev");
    const auto pr = register_range(t, *plic), ur = register_range(t, *uart);
    const auto contexts = t.property(*plic, "interrupts-extended");
    if (!plic_id || !nirq || !*nirq || *nirq >= 1024 || !pr || !ur || !contexts) return false;
    libk::ByteReader r{contexts->data(), contexts->size()};
    std::optional<usize> ctx;
    for (usize i = 0; r.remaining(); ++i) {
        u32 ref, irq;
        if (!r.read_be32(ref) || !r.read_be32(irq)) return false;
        const auto intc = t.phandle(ref);
        const auto cpu = intc ? t.parent(*intc) : std::nullopt;
        if (!intc || !cpu || !t.matches(*intc, "riscv,cpu-intc") || t.number(*intc, "#interrupt-cells") != 1) return false;
        const auto id = t.property(*cpu, "reg");
        u64 value;
        if (!id || id->empty() || id->size() > 8) return false;
        libk::ByteReader cr{id->data(), id->size()};
        if (!read_cells(cr, id->size() / 4, value)) return false;
        if (value == hart.raw && irq == 9) { if (ctx) return false; ctx = i; }
    }
    const auto uart_irq = wired_irq(t, *uart, *plic_id, *nirq);
    if (!ctx || !uart_irq || *ctx > (UINT64_MAX - 0x200008) / 0x1000 ||
        pr->size < 0x200008 + *ctx * 0x1000) return false;
    hw.plic = *pr; hw.uart = *ur; hw.ctx = *ctx; hw.nirq = *nirq; hw.uart_irq = *uart_irq;
    const auto pci = t.find("pci-host-ecam-generic");
    const auto iommu = t.find("riscv,iommu");
    if (!pci || !iommu) return true; // No isolated PCI bus is exposed without its isolation backend.
    const auto ir = register_range(t, *iommu), ecam = register_range(t, *pci);
    const auto iommu_id = t.number(*iommu, "phandle");
    const auto iommu_irq = wired_irq(t, *iommu, *plic_id, *nirq);
    const auto parent = t.parent(*pci);
    const auto ranges = t.property(*pci, "ranges"), buses = t.property(*pci, "bus-range");
    const auto routes = t.property(*pci, "interrupt-map"), mask = t.property(*pci, "interrupt-map-mask");
    const auto devices = t.property(*pci, "iommu-map");
    if (!ir || ir->size < mm::page_size || !ecam || ecam->size < (1 << 20) || !iommu_id || !iommu_irq || !parent ||
        !ranges || !buses || !routes || !mask || !devices ||
        t.number(*iommu, "#iommu-cells") != 1 || cell_count(t, *pci, "#address-cells", 3) != 3 ||
        cell_count(t, *pci, "#size-cells", 2) != 2 || t.number(*pci, "#interrupt-cells") != 1 ||
        cell_count(t, *pci, "iommu-map-mask", 0xffff) != 0xffff ||
        buses->size() != 8 || routes->size() % 24 || mask->size() != 16 || devices->size() % 16) return false;
    libk::ByteReader br{buses->data(), buses->size()};
    u32 first, last;
    if (!br.read_be32(first) || !br.read_be32(last) || first != 0 || last > 255) return false;
    // Current DMA directory and user enumerator cover root-bus functions only.
    hw.pci.cfg = {ecam->pa, 1 << 20};
    libk::ByteReader rr{ranges->data(), ranges->size()};
    while (rr.remaining()) {
        u32 flags; u64 bus, pa, size;
        if (!rr.read_be32(flags) || !read_cells(rr, 2, bus) ||
            !read_cells(rr, cell_count(t, *parent, "#address-cells", 2), pa) || !read_cells(rr, 2, size)) return false;
        if ((flags & 0x43000000) != 0x02000000 || hw.pci.window.size) continue;
        const auto window = translate(t, *parent, {pa, size});
        // CPU and PCI addresses currently share the public BAR placement metadata.
        if (!window || window->pa != bus) return false;
        hw.pci.window = *window;
    }
    if (!hw.pci.window.size) return false;
    libk::ByteReader mr{mask->data(), mask->size()};
    for (auto& word : hw.pci.mask) if (!mr.read_be32(word)) return false;
    libk::ByteReader route{routes->data(), routes->size()};
    while (route.remaining()) {
        u64 addr, pin, ref, id;
        if (!read_cells(route, 2, addr) || !read_cells(route, 2, pin) ||
            !read_cells(route, 1, ref) || !read_cells(route, 1, id) || ref != *plic_id || !id || id > *nirq) return false;
    }
    hw.pci.routes = *routes; hw.pci.devices = *devices; hw.pci.iommu = *iommu_id;
    hw.iommu = *ir; hw.iommu_irq = *iommu_irq;
    return true;
}

auto read_fdt(
    BootInfo& info,
    mm::RegionList& regions,
    CpuHwId boot_cpu,
    mm::Phys fdt_physical,
    const void* fdt_pointer, FwHw& hw) noexcept -> std::expected<void, FdtError> {
    info.firmware = {};
    info.module.reset();
    info.cpus.clear();
    info.timebase_frequency = 0;
    hw = {};
    regions.clear();

    auto opened = Fdt::open(fdt_pointer);
    if (!opened) {
        return std::unexpected(FdtError::InvalidHeader);
    }
    const Fdt& tree = opened.value();

    if (!parse_cpus(tree, boot_cpu, info.cpus)) {
        return std::unexpected(FdtError::InvalidStructure);
    }
    BootMap memory{};
    BootTree parsed{tree, memory};
    const auto timebase = parsed.timebase();
    if (!timebase) {
        return std::unexpected(FdtError::InvalidStructure);
    }
    info.timebase_frequency = *timebase;

    if (!parsed.read()) {
        console::print<"invalid FDT structure\n">();
        return std::unexpected(FdtError::InvalidStructure);
    }
    if (!read_hw(tree, boot_cpu, hw)) return std::unexpected(FdtError::InvalidStructure);

    const bool reservations_valid = tree.for_each_reservation(
        [&memory](uint64_t address, uint64_t size) {
            const auto range = Reg{address, size}.covering_pages();
            return range && static_cast<bool>(memory.reserve(
                *range,
                mm::Region::Kind::Firmware));
        });
    if (!reservations_valid) {
        return std::unexpected(FdtError::InvalidStructure);
    }
    if (!reserve_kernel(memory)) {
        return std::unexpected(FdtError::InvalidStructure);
    }

    auto module = parsed.module();
    if (!module) {
        return std::unexpected(module.error());
    }
    if (module.value()
        && !memory.reserve(
            module.value()->pages,
            mm::Region::Kind::Boot)) {
        return std::unexpected(FdtError::InvalidStructure);
    }

    const auto fdt_pages = mm::Pages::covering_bytes(
        fdt_physical,
        tree.size());
    if (!fdt_pages
        || !memory.reserve(
            *fdt_pages,
            mm::Region::Kind::Boot)) {
        return std::unexpected(FdtError::InvalidHeader);
    }

    const size_t ram_banks = std::ranges::distance(memory.ram());
    if (!std::move(memory).finish(regions)) {
        return std::unexpected(FdtError::InvalidStructure);
    }

    info.firmware = BootModule{
        .physical = fdt_physical,
        .size = static_cast<uint32_t>(tree.size()),
        .pages = *fdt_pages,
    };
    info.module = std::move(module).value();

    console::print<"ram_banks={:#x}\nmemory_regions={:#x}\n">(
        ram_banks, regions.size());
    return {};
}
