#pragma once

#include <sys/start.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <fstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

#include <servers/deploy/format.h>
#include <uapi/cap.h>
#include <uapi/mem.h>

namespace deploy::host {

struct BootstrapBinding final {
    uint32_t role{};
    boot::Import imported{};
    constexpr BootstrapBinding(uint32_t fixed) : role(fixed) {}
    constexpr BootstrapBinding(boot::Import named) : imported(named) {}
    constexpr auto kind() const -> uint16_t {
        return role != 0 ? boot_kind(role) : imported.kind;
    }
};

struct Table final {
    std::size_t offset{};
    std::uint32_t count{};
    std::uint32_t stride{};
};

inline void put(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::uint64_t value,
    std::size_t width) {
    if (bytes.size() < offset + width) {
        bytes.resize(offset + width);
    }
    for (std::size_t byte = 0; byte < width; ++byte) {
        bytes[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
    }
}

inline auto align_table(std::vector<std::uint8_t>& bytes) -> std::size_t {
    const std::size_t aligned = (bytes.size() + 7) & ~std::size_t{7};
    bytes.resize(aligned);
    return aligned;
}

inline auto append_table(
    std::vector<std::uint8_t>& bytes,
    std::uint32_t count,
    std::uint32_t stride) -> Table {
    const std::size_t offset = align_table(bytes);
    bytes.resize(offset + static_cast<std::size_t>(count) * stride);
    return Table{offset, count, stride};
}

/* All host fixtures share this little-endian envelope finalizer.  Policy
 * generators only populate their decoded table rows; they do not duplicate
 * the wire header or table-descriptor encoding. */
inline void finalize(
    std::vector<std::uint8_t>& bytes,
    const Table (&tables)[DEPLOY_TABLE_COUNT]) {
    put(bytes, DEPLOY_HEADER_MAGIC, DEPLOY_MAGIC, 8);
    put(bytes, DEPLOY_HEADER_MAJOR, DEPLOY_MAJOR, 2);
    put(bytes, DEPLOY_HEADER_MINOR, DEPLOY_MINOR, 2);
    put(bytes, DEPLOY_HEADER_SIZE_FIELD, DEPLOY_HEADER_SIZE, 4);
    put(bytes, DEPLOY_HEADER_TOTAL_SIZE, bytes.size(), 8);
    put(bytes, DEPLOY_HEADER_ARCHITECTURE,
        DEPLOY_ARCH_GENERIC, 4);
    put(bytes, DEPLOY_HEADER_ABI, DEPLOY_ABI_ID, 4);
    put(bytes, DEPLOY_HEADER_TABLE_COUNT,
        DEPLOY_TABLE_COUNT, 4);
    for (std::uint32_t index = 0; index < DEPLOY_TABLE_COUNT; ++index) {
        const std::size_t descriptor = DEPLOY_HEADER_TABLES
            + static_cast<std::size_t>(index) * DEPLOY_TABLE_DESC_SIZE;
        put(bytes, descriptor + DEPLOY_TABLE_OFFSET,
            tables[index].count == 0 ? 0 : tables[index].offset, 8);
        put(bytes, descriptor + DEPLOY_TABLE_COUNT_FIELD,
            tables[index].count, 4);
        put(bytes, descriptor + DEPLOY_TABLE_STRIDE,
            tables[index].stride, 4);
    }
}

/* Production deployment uses the same wire writer as every host fixture.
 * Image rows carry the PT_LOAD count and the critical code extent observed
 * from the exact ELF inputs used by the boot bundle.  Keeping this topology
 * in the host packer makes the manifest the one policy source used by init
 * and process_server rather than embedding a second deployment script in
 * either service. */
inline auto read_file_bytes(std::string_view path)
    -> std::vector<std::uint8_t> {
    std::ifstream input(std::string{path}, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open ELF input");
    }
    const std::streamoff end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot size ELF input");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty()
        && !input.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("cannot read ELF input");
    }
    return bytes;
}

struct image_info final {
    std::size_t segments{};
    std::uint64_t critical_code_bytes{};
};

/* Deployment policy follows the same PT_LOAD topology that bootpack will
 * validate.  The first virtual segment is the executable mapping marked
 * critical by the manifest, so its page-rounded extent is measured from the
 * exact ELF input instead of being guessed by the packer. */
inline auto read_image(std::string_view path)
    -> image_info {
    const std::vector<std::uint8_t> bytes = read_file_bytes(path);
    Elf64_Ehdr header{};
    if (bytes.size() < sizeof(header)) {
        throw std::runtime_error("ELF header is truncated");
    }
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.e_ident[EI_MAG0] != ELFMAG0
        || header.e_ident[EI_MAG1] != ELFMAG1
        || header.e_ident[EI_MAG2] != ELFMAG2
        || header.e_ident[EI_MAG3] != ELFMAG3
        || header.e_ident[EI_CLASS] != ELFCLASS64
        || header.e_ident[EI_DATA] != ELFDATA2LSB
        || header.e_ident[EI_VERSION] != EV_CURRENT
        || header.e_type != ET_EXEC || header.e_machine != EM_RISCV
        || header.e_version != EV_CURRENT
        || header.e_phentsize != sizeof(Elf64_Phdr)
        || header.e_phnum == 0
        || header.e_phoff > bytes.size()
        || static_cast<std::uint64_t>(header.e_phnum)
               > (bytes.size() - header.e_phoff) / sizeof(Elf64_Phdr)) {
        throw std::runtime_error("input is not a supported RISC-V ELF64 executable");
    }
    std::size_t count{};
    std::uint64_t first_address = UINT64_MAX;
    std::uint64_t first_size{};
    std::uint32_t first_flags{};
    for (std::uint16_t index = 0; index < header.e_phnum; ++index) {
        Elf64_Phdr program{};
        const std::size_t offset = static_cast<std::size_t>(header.e_phoff)
            + static_cast<std::size_t>(index) * sizeof(program);
        std::memcpy(&program, bytes.data() + offset, sizeof(program));
        if (program.p_type == PT_LOAD && program.p_memsz != 0) {
            ++count;
            if (program.p_vaddr < first_address) {
                first_address = program.p_vaddr;
                first_size = program.p_memsz;
                first_flags = program.p_flags;
            }
        }
    }
    if (count == 0 || count > 3) {
        throw std::runtime_error("production image has unsupported PT_LOAD count");
    }
    if ((first_flags & PF_X) == 0
        || first_size > UINT64_MAX - (DEPLOY_PAGE_SIZE - 1)) {
        throw std::runtime_error("production image has no executable first PT_LOAD");
    }
    return image_info{
        .segments = count,
        .critical_code_bytes =
            (first_size + DEPLOY_PAGE_SIZE - 1)
            / DEPLOY_PAGE_SIZE * DEPLOY_PAGE_SIZE};
}

} // namespace deploy::host
