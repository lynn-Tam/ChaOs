#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <uapi/boot_bundle.h>

namespace {

struct Segment final {
    std::uint64_t virtual_address{};
    std::uint64_t image_offset{};
    std::uint64_t file_size{};
    std::uint64_t memory_size{};
    std::uint64_t alignment{};
    std::uint32_t access{};
};

struct Image final {
    std::vector<std::byte> bytes;
    std::vector<Segment> segments;
    std::uint64_t entry{};
};

struct Module final {
    std::string name;
    bool data{};
    Image image;
    std::vector<std::byte> payload;
};

[[nodiscard]] constexpr auto add_overflows(
    std::uint64_t left,
    std::uint64_t right) noexcept -> bool {
    return right > std::numeric_limits<std::uint64_t>::max() - left;
}

[[nodiscard]] constexpr auto is_power_of_two(std::uint64_t value) noexcept
    -> bool {
    return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] constexpr auto is_user_address(std::uint64_t address) noexcept
    -> bool {
    // The cross-architecture manifest accepts the positive canonical
    // half. The selected kernel applies its own stricter layout policy.
    return address < (std::uint64_t{1} << 38);
}

template<typename T>
[[nodiscard]] auto read_object(
    std::span<const std::byte> bytes,
    std::uint64_t offset,
    T& output) noexcept -> bool {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        return false;
    }
    std::memcpy(&output, bytes.data() + offset, sizeof(T));
    return true;
}

[[nodiscard]] auto read_file(const std::string& path)
    -> std::vector<std::byte> {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open input: " + path);
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot size input: " + path);
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty()
        && !input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("cannot read input: " + path);
    }
    return bytes;
}

[[nodiscard]] auto parse_elf(std::vector<std::byte> bytes) -> Image {
    const std::span<const std::byte> view{bytes};
    Elf64_Ehdr header{};
    if (!read_object(view, 0, header)
        || header.e_ident[EI_MAG0] != ELFMAG0
        || header.e_ident[EI_MAG1] != ELFMAG1
        || header.e_ident[EI_MAG2] != ELFMAG2
        || header.e_ident[EI_MAG3] != ELFMAG3
        || header.e_ident[EI_CLASS] != ELFCLASS64
        || header.e_ident[EI_DATA] != ELFDATA2LSB
        || header.e_ident[EI_VERSION] != EV_CURRENT
        || header.e_type != ET_EXEC
        || header.e_machine != EM_RISCV
        || header.e_version != EV_CURRENT
        || header.e_phentsize != sizeof(Elf64_Phdr)
        || header.e_phnum == 0) {
        throw std::runtime_error("input is not a supported RISC-V ELF64 executable");
    }
    if (add_overflows(
            header.e_phoff,
            static_cast<std::uint64_t>(header.e_phnum)
                * sizeof(Elf64_Phdr))
        || header.e_phoff
                + static_cast<std::uint64_t>(header.e_phnum)
                    * sizeof(Elf64_Phdr)
            > view.size()) {
        throw std::runtime_error("program header table is out of bounds");
    }

    Image image{
        .bytes = std::move(bytes),
        .segments = {},
        .entry = header.e_entry,
    };
    const std::span<const std::byte> image_view{image.bytes};
    bool entry_covered{};
    for (std::uint16_t index = 0; index < header.e_phnum; ++index) {
        Elf64_Phdr program{};
        if (!read_object(
            image_view,
            header.e_phoff
                + static_cast<std::uint64_t>(index) * sizeof(Elf64_Phdr),
            program)) {
            throw std::runtime_error("program header is truncated");
        }
        if (program.p_type != PT_LOAD) {
            continue;
        }
        if (program.p_memsz == 0) {
            if (program.p_filesz != 0) {
                throw std::runtime_error("empty PT_LOAD contains file bytes");
            }
            continue;
        }
        if (program.p_filesz > program.p_memsz
            || !is_power_of_two(program.p_align)
            || program.p_align < 4096
            || (program.p_vaddr & 4095) != 0
            || (program.p_vaddr & (program.p_align - 1))
                != (program.p_offset & (program.p_align - 1))
            || add_overflows(program.p_offset, program.p_filesz)
            || program.p_offset + program.p_filesz > view.size()
            || add_overflows(program.p_vaddr, program.p_memsz)
            || !is_user_address(program.p_vaddr)
            || !is_user_address(program.p_vaddr + program.p_memsz - 1)
            || ((program.p_flags & PF_W) != 0
                && (program.p_flags & PF_X) != 0)) {
            throw std::runtime_error("invalid PT_LOAD segment");
        }

        std::uint32_t access{};
        if ((program.p_flags & PF_R) != 0) {
            access |= MYOS_BOOT_SEGMENT_READ;
        }
        if ((program.p_flags & PF_W) != 0) {
            access |= MYOS_BOOT_SEGMENT_WRITE;
        }
        if ((program.p_flags & PF_X) != 0) {
            access |= MYOS_BOOT_SEGMENT_EXECUTE;
        }
        image.segments.push_back(Segment{
            .virtual_address = program.p_vaddr,
            .image_offset = program.p_offset,
            .file_size = program.p_filesz,
            .memory_size = program.p_memsz,
            .alignment = program.p_align,
            .access = access,
        });
        if (header.e_entry >= program.p_vaddr
            && header.e_entry - program.p_vaddr < program.p_memsz
            && (program.p_flags & PF_X) != 0) {
            entry_covered = true;
        }
    }
    if (image.segments.empty() || !entry_covered) {
        throw std::runtime_error("entry is not covered by an executable PT_LOAD");
    }

    std::ranges::sort(image.segments, {}, &Segment::virtual_address);
    for (std::size_t index = 1; index < image.segments.size(); ++index) {
        const Segment& previous = image.segments[index - 1];
        const Segment& current = image.segments[index];
        const std::uint64_t previous_end =
            (previous.virtual_address + previous.memory_size + 4095)
            & ~std::uint64_t{4095};
        if (previous_end
            > current.virtual_address) {
            throw std::runtime_error("PT_LOAD page ranges overlap");
        }
    }
    return image;
}

void append_le(
    std::vector<std::byte>& output,
    std::uint64_t value,
    std::size_t width) {
    for (std::size_t index = 0; index < width; ++index) {
        output.push_back(static_cast<std::byte>(value >> (index * 8)));
    }
}

void pad_to(std::vector<std::byte>& output, std::size_t offset) {
    if (output.size() > offset) {
        throw std::runtime_error("internal bundle layout overlap");
    }
    output.resize(offset);
}

[[nodiscard]] constexpr auto align_up(
    std::size_t value,
    std::size_t alignment) noexcept -> std::size_t {
    return (value + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] auto pack(
    const std::vector<Module>& modules,
    std::size_t root_index)
    -> std::vector<std::byte> {
    if (modules.empty() || root_index >= modules.size()
        || modules.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("invalid module set");
    }
    if (modules[root_index].data) {
        throw std::runtime_error("root module must be bootable");
    }
    std::size_t segment_count{};
    std::size_t names_size{};
    for (const Module& module : modules) {
        if (module.name.empty()
            || module.name.size()
                > std::numeric_limits<std::uint32_t>::max()
            || (!module.data && module.image.segments.size()
                > std::numeric_limits<std::uint32_t>::max()
                || module.data && module.payload.empty())
            || segment_count
                > std::numeric_limits<std::uint32_t>::max()
                    - (module.data ? 0 : module.image.segments.size())) {
            throw std::runtime_error("module metadata exceeds the wire format");
        }
        segment_count += module.data ? 0 : module.image.segments.size();
        names_size += module.name.size();
    }

    const std::size_t modules_offset = MYOS_BOOT_HEADER_SIZE;
    const std::size_t segments_offset = modules_offset
        + modules.size() * MYOS_BOOT_MODULE_SIZE;
    const std::size_t names_offset = segments_offset
        + segment_count * MYOS_BOOT_SEGMENT_SIZE;

    std::vector<std::size_t> name_offsets;
    std::vector<std::size_t> image_offsets;
    name_offsets.reserve(modules.size());
    image_offsets.reserve(modules.size());
    std::size_t cursor = names_offset;
    for (const Module& module : modules) {
        name_offsets.push_back(cursor);
        cursor += module.name.size();
    }
    cursor = align_up(cursor, 8);
    for (const Module& module : modules) {
        const std::size_t image_size = module.data
            ? module.payload.size() : module.image.bytes.size();
        image_offsets.push_back(cursor);
        cursor = align_up(cursor + image_size, 8);
    }
    const std::size_t total_size = cursor;

    std::vector<std::byte> output;
    output.reserve(total_size);
    append_le(output, MYOS_BOOT_MAGIC, 8);
    append_le(output, MYOS_BOOT_MAJOR, 2);
    append_le(output, MYOS_BOOT_MINOR, 2);
    append_le(output, MYOS_BOOT_HEADER_SIZE, 4);
    append_le(output, total_size, 8);
    append_le(output, MYOS_BOOT_ARCH_RISCV64, 4);
    append_le(output, MYOS_BOOT_ABI_RISCV_LP64, 4);
    append_le(output, 0, 8); // required features
    append_le(output, modules_offset, 8);
    append_le(output, modules.size(), 4);
    append_le(output, root_index, 4);
    append_le(output, segments_offset, 8);
    append_le(output, segment_count, 4);
    append_le(output, 0, 4);
    append_le(output, 0, 8); // optional checksum
    pad_to(output, modules_offset);

    std::size_t segment_first{};
    for (std::size_t index = 0; index < modules.size(); ++index) {
        const Module& module = modules[index];
        append_le(output, name_offsets[index], 8);
        append_le(output, module.name.size(), 4);
        append_le(
            output,
            module.data ? MYOS_BOOT_MODULE_DATA : MYOS_BOOT_MODULE_BOOTABLE,
            4);
        append_le(output, image_offsets[index], 8);
        append_le(
            output,
            module.data ? module.payload.size() : module.image.bytes.size(),
            8);
        append_le(output, module.data ? 0 : module.image.entry, 8);
        append_le(output, segment_first, 4);
        append_le(
            output, module.data ? 0 : module.image.segments.size(), 4);
        append_le(output, 0, 8); // TLS offset
        append_le(output, 0, 8); // TLS size
        segment_first += module.data ? 0 : module.image.segments.size();
    }
    pad_to(output, segments_offset);

    for (std::size_t index = 0; index < modules.size(); ++index) {
        for (const Segment& segment : modules[index].image.segments) {
            append_le(output, segment.virtual_address, 8);
            append_le(
                output, image_offsets[index] + segment.image_offset, 8);
            append_le(output, segment.file_size, 8);
            append_le(output, segment.memory_size, 8);
            append_le(output, segment.alignment, 8);
            append_le(output, segment.access, 4);
            append_le(output, 0, 4);
        }
    }
    for (const Module& module : modules) {
        output.insert(
            output.end(),
            reinterpret_cast<const std::byte*>(module.name.data()),
            reinterpret_cast<const std::byte*>(
                module.name.data() + module.name.size()));
    }
    for (std::size_t index = 0; index < modules.size(); ++index) {
        pad_to(output, image_offsets[index]);
        const auto& bytes = modules[index].data
            ? modules[index].payload : modules[index].image.bytes;
        output.insert(output.end(), bytes.begin(), bytes.end());
    }
    pad_to(output, total_size);
    if (output.size() != total_size) {
        throw std::runtime_error("internal bundle size mismatch");
    }
    return output;
}

void write_file(const std::string& path, std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output
        || (!bytes.empty()
            && !output.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())))) {
        throw std::runtime_error("cannot write output: " + path);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: bootpack OUTPUT.BUNDLE ROOT_NAME "
                     "NAME=INPUT.ELF... data:NAME=INPUT.BIN...\n";
        return 2;
    }
    try {
        const std::string_view root_name{argv[2]};
        std::vector<Module> modules;
        std::size_t root_index = std::numeric_limits<std::size_t>::max();
        for (int index = 3; index < argc; ++index) {
            const std::string_view raw_spec{argv[index]};
            const bool data = raw_spec.starts_with("data:")
                || raw_spec.starts_with("DATA:");
            const std::string_view spec = data ? raw_spec.substr(5)
                                               : raw_spec;
            const std::size_t separator = spec.find('=');
            if (separator == 0 || separator == std::string_view::npos
                || separator + 1 == spec.size()) {
                throw std::runtime_error(
                    "module must be NAME=INPUT.ELF or data:NAME=INPUT.BIN");
            }
            const std::string name{spec.substr(0, separator)};
            if (std::ranges::any_of(
                    modules,
                    [&](const Module& module) { return module.name == name; })) {
                throw std::runtime_error("duplicate module name: " + name);
            }
            if (name == root_name) {
                root_index = modules.size();
            }
            if (data) {
                modules.push_back(Module{
                    .name = name,
                    .data = true,
                    .image = {},
                    .payload = read_file(
                        std::string{spec.substr(separator + 1)}),
                });
            } else {
                modules.push_back(Module{
                    .name = name,
                    .data = false,
                    .image = parse_elf(read_file(
                        std::string{spec.substr(separator + 1)})),
                    .payload = {},
                });
            }
        }
        if (root_index == std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("root module is absent");
        }
        const std::vector<std::byte> bundle = pack(modules, root_index);
        write_file(argv[1], bundle);
        std::cout << "bootpack: " << modules.size() << " modules, "
                  << bundle.size() << " bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bootpack: " << error.what() << '\n';
        return 1;
    }
}
