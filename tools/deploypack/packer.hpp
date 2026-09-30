#pragma once

#include <user/abi/startup.hpp>

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
#include <uapi/object.h>
#include <uapi/resource.h>
#include <uapi/vm.h>

namespace deploy::host {

struct BootstrapBinding final {
    uint32_t role{};
    myos::bootstrap::Import imported{};
    constexpr BootstrapBinding(uint32_t fixed) : role(fixed) {}
    constexpr BootstrapBinding(myos::bootstrap::Import named) : imported(named) {}
    constexpr auto kind() const -> uint16_t {
        return role != 0 ? myos_bootstrap_object_kind(role) : imported.kind;
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

struct ProductionImageMetrics final {
    std::size_t segments{};
    std::uint64_t critical_code_bytes{};
};

/* Deployment policy follows the same PT_LOAD topology that bootpack will
 * validate.  The first virtual segment is the executable mapping marked
 * critical by the manifest, so its page-rounded extent is measured from the
 * exact ELF input instead of being guessed by the packer. */
inline auto production_image_metrics(std::string_view path)
    -> ProductionImageMetrics {
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
    return ProductionImageMetrics{
        .segments = count,
        .critical_code_bytes =
            (first_size + DEPLOY_PAGE_SIZE - 1)
            / DEPLOY_PAGE_SIZE * DEPLOY_PAGE_SIZE};
}

inline auto pack_fixture() -> std::vector<std::uint8_t> {
    struct KeyRef final {
        std::uint32_t offset{};
        std::uint32_t length{};
        [[nodiscard]] auto packed() const noexcept -> std::uint64_t {
            return static_cast<std::uint64_t>(offset)
                | (static_cast<std::uint64_t>(length) << 32);
        }
    };
    constexpr const char* strings[] = {
        "init", "pool", "vspace", "cspace", "code", "stack",
        "bootstrap", "notify", "thread", "sc", "domain", "import",
        "export", "authority",
    };
    constexpr std::size_t string_count = sizeof(strings) / sizeof(strings[0]);
    KeyRef keys[string_count]{};
    std::uint32_t string_bytes{};
    for (std::size_t index = 0; index < string_count; ++index) {
        std::size_t length{};
        while (strings[index][length] != '\0') {
            ++length;
        }
        keys[index] = KeyRef{string_bytes, static_cast<std::uint32_t>(length)};
        string_bytes += static_cast<std::uint32_t>(length);
    }
    std::vector<std::uint8_t> bytes(DEPLOY_HEADER_SIZE, 0);
    Table tables[DEPLOY_TABLE_COUNT]{};
    tables[DEPLOY_TABLE_TASK] = append_table(
        bytes, 1, DEPLOY_TASK_STRIDE);
    tables[DEPLOY_TABLE_IMAGE] = append_table(
        bytes, 1, DEPLOY_IMAGE_STRIDE);
    tables[DEPLOY_TABLE_MAPPING] = append_table(
        bytes, 3, DEPLOY_MAPPING_STRIDE);
    tables[DEPLOY_TABLE_OBJECT] = append_table(
        bytes, 1, DEPLOY_OBJECT_STRIDE);
    tables[DEPLOY_TABLE_EXECUTION] = append_table(
        bytes, 1, DEPLOY_EXECUTION_STRIDE);
    tables[DEPLOY_TABLE_IMPORT] = append_table(
        bytes, 1, DEPLOY_IMPORT_STRIDE);
    tables[DEPLOY_TABLE_DEPENDENCY] = append_table(
        bytes, 0, DEPLOY_DEPENDENCY_STRIDE);
    tables[DEPLOY_TABLE_EXPORT] = append_table(
        bytes, 1, DEPLOY_EXPORT_STRIDE);
    tables[DEPLOY_TABLE_STRING] = append_table(bytes, string_bytes, 1);
    tables[DEPLOY_TABLE_BOOTSTRAP] = append_table(
        bytes, 0, DEPLOY_BOOTSTRAP_STRIDE);

    const std::size_t task = tables[DEPLOY_TABLE_TASK].offset;
    put(bytes, task + DEPLOY_TASK_NAME, keys[0].packed(), 8);
    put(bytes, task + DEPLOY_TASK_POOL, keys[1].packed(), 8);
    put(bytes, task + DEPLOY_TASK_VSPACE, keys[2].packed(), 8);
    put(bytes, task + DEPLOY_TASK_CSPACE, keys[3].packed(), 8);
    put(bytes, task + DEPLOY_TASK_IMAGE_FIRST, 0, 4);
    put(bytes, task + DEPLOY_TASK_IMAGE_COUNT, 1, 4);
    put(bytes, task + DEPLOY_TASK_MAPPING_FIRST, 0, 4);
    put(bytes, task + DEPLOY_TASK_MAPPING_COUNT, 3, 4);
    put(bytes, task + DEPLOY_TASK_OBJECT_FIRST, 0, 4);
    put(bytes, task + DEPLOY_TASK_OBJECT_COUNT, 1, 4);
    put(bytes, task + DEPLOY_TASK_EXECUTION_FIRST, 0, 4);
    put(bytes, task + DEPLOY_TASK_EXECUTION_COUNT, 1, 4);
    put(bytes, task + DEPLOY_TASK_IMPORT_FIRST, 0, 4);
    put(bytes, task + DEPLOY_TASK_IMPORT_COUNT, 1, 4);
    put(bytes, task + DEPLOY_TASK_DEPENDENCY_FIRST, 0, 4);
    put(bytes, task + DEPLOY_TASK_DEPENDENCY_COUNT, 0, 4);
    put(bytes, task + DEPLOY_TASK_EXPORT_FIRST, 0, 4);
    put(bytes, task + DEPLOY_TASK_EXPORT_COUNT, 1, 4);
    put(bytes, task + DEPLOY_TASK_POOL_MEMORY, 16384, 8);
    put(bytes, task + DEPLOY_TASK_POOL_CAPS, 16, 8);
    put(bytes, task + DEPLOY_TASK_KIND_MASK, MYOS_RESOURCE_E2_KINDS, 8);
    put(bytes, task + DEPLOY_TASK_CRITICAL_BYTES, 12288, 8);
    put(bytes, task + DEPLOY_TASK_CSPACE_SLOTS, 16, 4);
    put(bytes, task + DEPLOY_TASK_CSPACE_PAGES, 1, 4);
    put(bytes, task + DEPLOY_TASK_BOOTSTRAP_MAPPING, 2, 4);

    const std::size_t image = tables[DEPLOY_TABLE_IMAGE].offset;
    put(bytes, image + DEPLOY_IMAGE_SOURCE, keys[0].packed(), 8);

    const std::size_t mapping = tables[DEPLOY_TABLE_MAPPING].offset;
    put(bytes, mapping + DEPLOY_MAPPING_PRODUCED, keys[4].packed(), 8);
    put(bytes, mapping + DEPLOY_MAPPING_IMAGE, 0, 4);
    put(bytes, mapping + DEPLOY_MAPPING_SEGMENT, 0, 4);
    put(bytes, mapping + DEPLOY_MAPPING_SOURCE,
        DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT, 2);
    put(bytes, mapping + DEPLOY_MAPPING_RESIDENCY,
        DEPLOY_MAPPING_RESIDENT, 2);
    put(bytes, mapping + DEPLOY_MAPPING_CRITICAL,
        DEPLOY_CRITICAL_CODE, 2);

    const std::size_t stack_mapping = mapping + DEPLOY_MAPPING_STRIDE;
    put(bytes, stack_mapping + DEPLOY_MAPPING_PRODUCED,
        keys[5].packed(), 8);
    put(bytes, stack_mapping + DEPLOY_MAPPING_IMAGE,
        DEPLOY_NO_INDEX, 4);
    put(bytes, stack_mapping + DEPLOY_MAPPING_SEGMENT,
        DEPLOY_NO_INDEX, 4);
    put(bytes, stack_mapping + DEPLOY_MAPPING_SOURCE,
        DEPLOY_MAPPING_SOURCE_ZERO, 2);
    put(bytes, stack_mapping + DEPLOY_MAPPING_RESIDENCY,
        DEPLOY_MAPPING_RESIDENT, 2);
    put(bytes, stack_mapping + DEPLOY_MAPPING_CRITICAL,
        DEPLOY_CRITICAL_STACK, 2);
    put(bytes, stack_mapping + DEPLOY_MAPPING_ACCESS,
        MYOS_VM_READ | MYOS_VM_WRITE, 4);
    put(bytes, stack_mapping + DEPLOY_MAPPING_ADDRESS, 0x210000, 8);
    put(bytes, stack_mapping + DEPLOY_MAPPING_SIZE, 4096, 8);

    const std::size_t bootstrap_mapping =
        stack_mapping + DEPLOY_MAPPING_STRIDE;
    put(bytes, bootstrap_mapping + DEPLOY_MAPPING_PRODUCED,
        keys[6].packed(), 8);
    put(bytes, bootstrap_mapping + DEPLOY_MAPPING_IMAGE,
        DEPLOY_NO_INDEX, 4);
    put(bytes, bootstrap_mapping + DEPLOY_MAPPING_SEGMENT,
        DEPLOY_NO_INDEX, 4);
    put(bytes, bootstrap_mapping + DEPLOY_MAPPING_SOURCE,
        DEPLOY_MAPPING_SOURCE_ZERO, 2);
    put(bytes, bootstrap_mapping + DEPLOY_MAPPING_RESIDENCY,
        DEPLOY_MAPPING_RESIDENT, 2);
    put(bytes, bootstrap_mapping + DEPLOY_MAPPING_CRITICAL,
        DEPLOY_CRITICAL_BOOTSTRAP, 2);
    put(bytes, bootstrap_mapping + DEPLOY_MAPPING_ACCESS,
        MYOS_VM_READ, 4);
    put(bytes, bootstrap_mapping + DEPLOY_MAPPING_ADDRESS, 0x220000, 8);
    put(bytes, bootstrap_mapping + DEPLOY_MAPPING_SIZE, 4096, 8);

    const std::size_t object = tables[DEPLOY_TABLE_OBJECT].offset;
    put(bytes, object + DEPLOY_OBJECT_OUTPUT_A, keys[7].packed(), 8);
    put(bytes, object + DEPLOY_OBJECT_KIND,
        MYOS_OBJECT_KIND_NOTIFICATION, 2);
    put(bytes, object + DEPLOY_OBJECT_ARG0, 1, 8);
    for (std::size_t field = DEPLOY_OBJECT_REF0;
         field <= DEPLOY_OBJECT_REF3;
         field += sizeof(std::uint32_t)) {
        put(bytes, object + field, DEPLOY_NO_INDEX, 4);
    }

    const std::size_t execution = tables[DEPLOY_TABLE_EXECUTION].offset;
    put(bytes, execution + DEPLOY_EXECUTION_KEY, keys[8].packed(), 8);
    put(bytes, execution + DEPLOY_EXECUTION_SC, keys[9].packed(), 8);
    put(bytes, execution + DEPLOY_EXECUTION_DOMAIN, keys[10].packed(), 8);
    put(bytes, execution + DEPLOY_EXECUTION_IMAGE, 0, 4);
    put(bytes, execution + DEPLOY_EXECUTION_STACK, 1, 4);
    put(bytes, execution + DEPLOY_EXECUTION_BOOTSTRAP, 2, 4);
    put(bytes, execution + DEPLOY_EXECUTION_IPC,
        DEPLOY_NO_INDEX, 4);
    put(bytes, execution + DEPLOY_EXECUTION_CONTROL,
        DEPLOY_NO_INDEX, 4);
    put(bytes, execution + DEPLOY_EXECUTION_EVENT,
        DEPLOY_NO_INDEX, 4);
    put(bytes, execution + DEPLOY_EXECUTION_ENTRY, 0x200000, 8);
    put(bytes, execution + DEPLOY_EXECUTION_STACK_TOP, 0x211000, 8);
    put(bytes, execution + DEPLOY_EXECUTION_SC_BUDGET, 1, 8);
    put(bytes, execution + DEPLOY_EXECUTION_SC_PERIOD, 1, 8);
    put(bytes, execution + DEPLOY_EXECUTION_URGENCY, 0, 4);
    put(bytes, execution + DEPLOY_EXECUTION_HOME_CPU,
        DEPLOY_HOME_CPU_ANY, 4);

    const std::size_t import = tables[DEPLOY_TABLE_IMPORT].offset;
    put(bytes, import + DEPLOY_IMPORT_SOURCE, keys[13].packed(), 8);
    put(bytes, import + DEPLOY_IMPORT_DESTINATION, keys[11].packed(), 8);
    put(bytes, import + DEPLOY_IMPORT_MODE,
        DEPLOY_IMPORT_DUPLICATE, 2);
    put(bytes, import + DEPLOY_IMPORT_SELECTOR,
        DEPLOY_SELECTOR_ALLOCATED_KEYED, 2);
    put(bytes, import + DEPLOY_IMPORT_ATTENUATION
            + DEPLOY_ATTENUATION_VERSION,
        DEPLOY_ATTENUATION_VERSION_CURRENT, 2);
    put(bytes, import + DEPLOY_IMPORT_ATTENUATION
            + DEPLOY_ATTENUATION_KIND,
        MYOS_OBJECT_KIND_THREAD, 2);
    put(bytes, import + DEPLOY_IMPORT_ATTENUATION
            + DEPLOY_ATTENUATION_SIZE,
        DEPLOY_ATTENUATION_STRIDE, 4);

    const std::size_t output = tables[DEPLOY_TABLE_EXPORT].offset;
    put(bytes, output + DEPLOY_EXPORT_SOURCE, keys[8].packed(), 8);
    put(bytes, output + DEPLOY_EXPORT_KEY, keys[12].packed(), 8);
    put(bytes, output + DEPLOY_EXPORT_CLASS,
        DEPLOY_EXPORT_PREPARED_KEY, 2);
    put(bytes, output + DEPLOY_EXPORT_CEILING
            + DEPLOY_ATTENUATION_VERSION,
        DEPLOY_ATTENUATION_VERSION_CURRENT, 2);
    put(bytes, output + DEPLOY_EXPORT_CEILING
            + DEPLOY_ATTENUATION_KIND,
        MYOS_OBJECT_KIND_THREAD, 2);
    put(bytes, output + DEPLOY_EXPORT_CEILING
            + DEPLOY_ATTENUATION_SIZE,
        DEPLOY_ATTENUATION_STRIDE, 4);

    const std::size_t string_table = tables[DEPLOY_TABLE_STRING].offset;
    std::size_t string_offset{};
    for (const char* string : strings) {
        for (std::size_t index = 0; string[index] != '\0'; ++index) {
            bytes[string_table + string_offset++] =
                static_cast<std::uint8_t>(string[index]);
        }
    }

    finalize(bytes, tables);
    return bytes;
}

} // namespace deploy::host
