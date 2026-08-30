#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <fstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

#include <uapi/deploy.h>
#include <uapi/object.h>
#include <uapi/resource.h>
#include <uapi/vm.h>

namespace myos::deploy::host {

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
    const Table (&tables)[MYOS_DEPLOY_TABLE_COUNT]) {
    put(bytes, MYOS_DEPLOY_HEADER_MAGIC, MYOS_DEPLOY_MAGIC, 8);
    put(bytes, MYOS_DEPLOY_HEADER_MAJOR, MYOS_DEPLOY_MAJOR, 2);
    put(bytes, MYOS_DEPLOY_HEADER_MINOR, MYOS_DEPLOY_MINOR, 2);
    put(bytes, MYOS_DEPLOY_HEADER_SIZE_FIELD, MYOS_DEPLOY_HEADER_SIZE, 4);
    put(bytes, MYOS_DEPLOY_HEADER_TOTAL_SIZE, bytes.size(), 8);
    put(bytes, MYOS_DEPLOY_HEADER_ARCHITECTURE,
        MYOS_DEPLOY_ARCH_GENERIC, 4);
    put(bytes, MYOS_DEPLOY_HEADER_ABI, MYOS_DEPLOY_ABI_ID, 4);
    put(bytes, MYOS_DEPLOY_HEADER_TABLE_COUNT,
        MYOS_DEPLOY_TABLE_COUNT, 4);
    for (std::uint32_t index = 0; index < MYOS_DEPLOY_TABLE_COUNT; ++index) {
        const std::size_t descriptor = MYOS_DEPLOY_HEADER_TABLES
            + static_cast<std::size_t>(index) * MYOS_DEPLOY_TABLE_DESC_SIZE;
        put(bytes, descriptor + MYOS_DEPLOY_TABLE_OFFSET,
            tables[index].count == 0 ? 0 : tables[index].offset, 8);
        put(bytes, descriptor + MYOS_DEPLOY_TABLE_COUNT_FIELD,
            tables[index].count, 4);
        put(bytes, descriptor + MYOS_DEPLOY_TABLE_STRIDE,
            tables[index].stride, 4);
    }
}

/* Keep the original fixture wire image stable for legacy host tests.  The
 * current parser accepts this v1.0 nine-table shape as a compatibility input;
 * production manifests use finalize() and the ten-table v1.1 shape above. */
inline void finalize_legacy(
    std::vector<std::uint8_t>& bytes,
    const Table (&tables)[9]) {
    put(bytes, MYOS_DEPLOY_HEADER_MAGIC, MYOS_DEPLOY_MAGIC, 8);
    put(bytes, MYOS_DEPLOY_HEADER_MAJOR, MYOS_DEPLOY_MAJOR, 2);
    put(bytes, MYOS_DEPLOY_HEADER_MINOR, 0, 2);
    put(bytes, MYOS_DEPLOY_HEADER_SIZE_FIELD, 208, 4);
    put(bytes, MYOS_DEPLOY_HEADER_TOTAL_SIZE, bytes.size(), 8);
    put(bytes, MYOS_DEPLOY_HEADER_ARCHITECTURE,
        MYOS_DEPLOY_ARCH_GENERIC, 4);
    put(bytes, MYOS_DEPLOY_HEADER_ABI, MYOS_DEPLOY_ABI_ID, 4);
    put(bytes, MYOS_DEPLOY_HEADER_TABLE_COUNT, 9, 4);
    for (std::uint32_t index = 0; index < 9; ++index) {
        const std::size_t descriptor = MYOS_DEPLOY_HEADER_TABLES
            + static_cast<std::size_t>(index) * MYOS_DEPLOY_TABLE_DESC_SIZE;
        put(bytes, descriptor + MYOS_DEPLOY_TABLE_OFFSET,
            tables[index].count == 0 ? 0 : tables[index].offset, 8);
        put(bytes, descriptor + MYOS_DEPLOY_TABLE_COUNT_FIELD,
            tables[index].count, 4);
        put(bytes, descriptor + MYOS_DEPLOY_TABLE_STRIDE,
            tables[index].stride, 4);
    }
}

/* The first production deployment is intentionally small but uses the same
 * wire writer as every host fixture.  Both child images have the two
 * PT_LOADs emitted by the freestanding linker (code and read-only data); the
 * stack and generated bootstrap are ordinary zero mappings.  Keeping this
 * topology in the host packer makes the manifest the one policy source used
 * by init and process_server rather than embedding a second deployment script
 * in either service. */
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

/* Deployment policy follows the same PT_LOAD topology that bootpack will
 * validate.  Only the count is needed here; segment bytes and permissions
 * remain canonical in the BootBundle/ELF materializer path. */
inline auto production_segment_count(std::string_view path) -> std::size_t {
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
    for (std::uint16_t index = 0; index < header.e_phnum; ++index) {
        Elf64_Phdr program{};
        const std::size_t offset = static_cast<std::size_t>(header.e_phoff)
            + static_cast<std::size_t>(index) * sizeof(program);
        std::memcpy(&program, bytes.data() + offset, sizeof(program));
        if (program.p_type == PT_LOAD && program.p_memsz != 0) {
            ++count;
        }
    }
    if (count == 0 || count > 3) {
        throw std::runtime_error("production image has unsupported PT_LOAD count");
    }
    return count;
}

inline auto pack_production(
    std::size_t process_segments,
    std::size_t proof_segments) -> std::vector<std::uint8_t> {
    if (process_segments == 0 || process_segments > 3
        || proof_segments == 0 || proof_segments > 3) {
        throw std::runtime_error("production image has unsupported PT_LOAD count");
    }
    constexpr std::string_view strings[] = {
        "process-server", "proof",
        "process.pool", "process.vspace", "process.cspace",
        "proof.pool", "proof.vspace", "proof.cspace",
        "process_server", "proof",
        "process.code", "process.rodata", "process.stack",
        "process.bootstrap", "proof.code", "proof.rodata",
        "proof.stack", "proof.bootstrap",
        "process.thread", "process.sc", "process.domain",
        "proof.thread", "proof.sc", "proof.domain",
        "process.notify", "proof.notify",
        "process.pool.cap", "process.vspace.cap", "process.cspace.cap",
        "process.domain.cap", "process.bundle.cap",
        "proof.pool.cap", "proof.vspace.cap", "proof.cspace.cap",
        "proof.domain.cap", "proof.bundle.cap",
        "root.domain", "root.bundle", "server.domain", "server.bundle",
        "process.segment2", "proof.segment2",
    };

    struct KeyRef final {
        std::uint32_t offset{};
        std::uint32_t length{};

        [[nodiscard]] auto packed() const noexcept -> std::uint64_t {
            return static_cast<std::uint64_t>(offset)
                | (static_cast<std::uint64_t>(length) << 32);
        }
    };

    constexpr std::size_t string_count = sizeof(strings) / sizeof(strings[0]);
    KeyRef keys[string_count]{};
    std::uint32_t string_bytes{};
    for (std::size_t index = 0; index < string_count; ++index) {
        keys[index] = KeyRef{
            string_bytes,
            static_cast<std::uint32_t>(strings[index].size())};
        string_bytes += static_cast<std::uint32_t>(strings[index].size());
    }
    const auto key = [&](std::size_t index) noexcept {
        return keys[index].packed();
    };

    std::vector<std::uint8_t> bytes(MYOS_DEPLOY_HEADER_SIZE, 0);
    Table tables[MYOS_DEPLOY_TABLE_COUNT]{};
    tables[MYOS_DEPLOY_TABLE_TASK] = append_table(
        bytes, 2, MYOS_DEPLOY_TASK_STRIDE);
    tables[MYOS_DEPLOY_TABLE_IMAGE] = append_table(
        bytes, 2, MYOS_DEPLOY_IMAGE_STRIDE);
    const std::uint32_t process_mapping_count =
        static_cast<std::uint32_t>(process_segments + 2);
    const std::uint32_t proof_mapping_count =
        static_cast<std::uint32_t>(proof_segments + 2);
    tables[MYOS_DEPLOY_TABLE_MAPPING] = append_table(
        bytes, process_mapping_count + proof_mapping_count,
        MYOS_DEPLOY_MAPPING_STRIDE);
    tables[MYOS_DEPLOY_TABLE_OBJECT] = append_table(
        bytes, 2, MYOS_DEPLOY_OBJECT_STRIDE);
    tables[MYOS_DEPLOY_TABLE_EXECUTION] = append_table(
        bytes, 2, MYOS_DEPLOY_EXECUTION_STRIDE);
    tables[MYOS_DEPLOY_TABLE_IMPORT] = append_table(
        bytes, 10, MYOS_DEPLOY_IMPORT_STRIDE);
    tables[MYOS_DEPLOY_TABLE_DEPENDENCY] = append_table(
        bytes, 0, MYOS_DEPLOY_DEPENDENCY_STRIDE);
    tables[MYOS_DEPLOY_TABLE_EXPORT] = append_table(
        bytes, 0, MYOS_DEPLOY_EXPORT_STRIDE);
    tables[MYOS_DEPLOY_TABLE_STRING] = append_table(bytes, string_bytes, 1);
    tables[MYOS_DEPLOY_TABLE_BOOTSTRAP] = append_table(
        bytes, 10, MYOS_DEPLOY_BOOTSTRAP_STRIDE);

    const auto attenuation = [&](std::size_t offset,
                                 std::uint16_t kind,
                                 std::uint64_t rights) {
        put(bytes, offset + MYOS_DEPLOY_ATTENUATION_VERSION,
            MYOS_DEPLOY_ATTENUATION_VERSION_CURRENT, 2);
        put(bytes, offset + MYOS_DEPLOY_ATTENUATION_KIND, kind, 2);
        put(bytes, offset + MYOS_DEPLOY_ATTENUATION_SIZE,
            MYOS_DEPLOY_ATTENUATION_STRIDE, 4);
        put(bytes, offset + MYOS_DEPLOY_ATTENUATION_RIGHTS, rights, 8);
    };
    const auto mapping = [&](std::size_t offset, std::size_t produced,
                             std::uint32_t image, std::uint32_t segment,
                             std::uint16_t critical, std::uint32_t access,
                             std::uint64_t address, std::uint64_t size) {
        put(bytes, offset + MYOS_DEPLOY_MAPPING_PRODUCED, key(produced), 8);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_IMAGE, image, 4);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_SEGMENT, segment, 4);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_SOURCE,
            MYOS_DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT, 2);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_RESIDENCY,
            MYOS_DEPLOY_MAPPING_RESIDENT, 2);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_CRITICAL, critical, 2);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_ACCESS, access, 4);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_ADDRESS, address, 8);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_SIZE, size, 8);
    };
    const auto zero_mapping = [&](std::size_t offset, std::size_t produced,
                                  std::uint16_t critical, std::uint32_t access,
                                  std::uint64_t address, std::uint64_t size) {
        put(bytes, offset + MYOS_DEPLOY_MAPPING_PRODUCED, key(produced), 8);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_IMAGE,
            MYOS_DEPLOY_NO_INDEX, 4);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_SEGMENT,
            MYOS_DEPLOY_NO_INDEX, 4);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_SOURCE,
            MYOS_DEPLOY_MAPPING_SOURCE_ZERO, 2);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_RESIDENCY,
            MYOS_DEPLOY_MAPPING_RESIDENT, 2);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_CRITICAL, critical, 2);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_ACCESS, access, 4);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_ADDRESS, address, 8);
        put(bytes, offset + MYOS_DEPLOY_MAPPING_SIZE, size, 8);
    };
    const auto task_row = [&](std::size_t offset, std::size_t name,
                              std::size_t pool, std::size_t vspace,
                              std::size_t cspace, std::uint32_t image_first,
                              std::uint32_t mapping_first,
                              std::uint32_t object_first,
                              std::uint32_t execution_first,
                              std::uint32_t import_first,
                              std::uint32_t bootstrap_first,
                              std::uint32_t mapping_count,
                              std::uint16_t readiness,
                              std::uint64_t pool_memory,
                              std::uint64_t pool_caps) {
        put(bytes, offset + MYOS_DEPLOY_TASK_NAME, key(name), 8);
        put(bytes, offset + MYOS_DEPLOY_TASK_POOL, key(pool), 8);
        put(bytes, offset + MYOS_DEPLOY_TASK_VSPACE, key(vspace), 8);
        put(bytes, offset + MYOS_DEPLOY_TASK_CSPACE, key(cspace), 8);
        put(bytes, offset + MYOS_DEPLOY_TASK_IMAGE_FIRST, image_first, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_IMAGE_COUNT, 1, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_MAPPING_FIRST, mapping_first, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_MAPPING_COUNT, mapping_count, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_OBJECT_FIRST, object_first, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_OBJECT_COUNT, 1, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_EXECUTION_FIRST,
            execution_first, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_EXECUTION_COUNT, 1, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_IMPORT_FIRST, import_first, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_IMPORT_COUNT, 5, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_DEPENDENCY_FIRST, 0, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_DEPENDENCY_COUNT, 0, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_EXPORT_FIRST, 0, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_EXPORT_COUNT, 0, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_POOL_MEMORY, pool_memory, 8);
        put(bytes, offset + MYOS_DEPLOY_TASK_POOL_CAPS, pool_caps, 8);
        put(bytes, offset + MYOS_DEPLOY_TASK_KIND_MASK,
            MYOS_RESOURCE_E2_KINDS, 8);
        put(bytes, offset + MYOS_DEPLOY_TASK_CRITICAL_BYTES,
            4U * 1024U * 1024U, 8);
        put(bytes, offset + MYOS_DEPLOY_TASK_CSPACE_SLOTS, 64, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_CSPACE_PAGES, 4, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_BOOTSTRAP_MAPPING,
            mapping_first + mapping_count - 1, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_READINESS,
            readiness, 2);
        put(bytes, offset + MYOS_DEPLOY_TASK_TERMINAL,
            MYOS_DEPLOY_TERMINAL_CLOSE, 2);
        put(bytes, offset + MYOS_DEPLOY_TASK_RESTART,
            MYOS_DEPLOY_RESTART_NEVER, 2);
        put(bytes, offset + MYOS_DEPLOY_TASK_BOOTSTRAP_FIRST,
            bootstrap_first, 4);
        put(bytes, offset + MYOS_DEPLOY_TASK_BOOTSTRAP_COUNT, 5, 4);
    };

    const std::size_t task0 = tables[MYOS_DEPLOY_TABLE_TASK].offset;
    const std::size_t task1 = task0 + MYOS_DEPLOY_TASK_STRIDE;
    task_row(task0, 0, 2, 3, 4, 0, 0, 0, 0, 0, 0,
             process_mapping_count, MYOS_DEPLOY_READINESS_NONE,
             32U * 1024U * 1024U + MYOS_DEPLOY_PAGE_SIZE, 513);
    task_row(task1, 1, 5, 6, 7, 1,
             process_mapping_count, 1, 1, 5, 5, proof_mapping_count,
             MYOS_DEPLOY_READINESS_START,
             16U * 1024U * 1024U, 256);

    const std::size_t images = tables[MYOS_DEPLOY_TABLE_IMAGE].offset;
    put(bytes, images + MYOS_DEPLOY_IMAGE_SOURCE, key(8), 8);
    put(bytes, images + MYOS_DEPLOY_IMAGE_SOURCE_KIND,
        MYOS_DEPLOY_IMAGE_SOURCE_BOOT_BUNDLE, 2);
    put(bytes, images + MYOS_DEPLOY_IMAGE_FLAGS, 0, 2);
    put(bytes, images + MYOS_DEPLOY_IMAGE_SOURCE + MYOS_DEPLOY_IMAGE_STRIDE,
        key(9), 8);
    put(bytes,
        images + MYOS_DEPLOY_IMAGE_SOURCE_KIND + MYOS_DEPLOY_IMAGE_STRIDE,
        MYOS_DEPLOY_IMAGE_SOURCE_BOOT_BUNDLE, 2);
    put(bytes, images + MYOS_DEPLOY_IMAGE_FLAGS + MYOS_DEPLOY_IMAGE_STRIDE,
        0, 2);

    const std::size_t mappings = tables[MYOS_DEPLOY_TABLE_MAPPING].offset;
    for (std::size_t segment = 0; segment < process_segments; ++segment) {
        const std::size_t name = segment == 0 ? 10 : segment == 1 ? 11 : 40;
        mapping(mappings + segment * MYOS_DEPLOY_MAPPING_STRIDE,
                name, 0, static_cast<std::uint32_t>(segment),
                segment == 0 ? MYOS_DEPLOY_CRITICAL_CODE
                              : MYOS_DEPLOY_CRITICAL_NONE,
                0, 0, 0);
    }
    const std::size_t process_stack = process_segments;
    const std::size_t process_bootstrap = process_segments + 1;
    zero_mapping(mappings + process_stack * MYOS_DEPLOY_MAPPING_STRIDE, 12,
                 MYOS_DEPLOY_CRITICAL_STACK, MYOS_VM_READ | MYOS_VM_WRITE,
                 0x40000000, 0x10000);
    zero_mapping(
        mappings + process_bootstrap * MYOS_DEPLOY_MAPPING_STRIDE, 13,
        MYOS_DEPLOY_CRITICAL_BOOTSTRAP, MYOS_VM_READ,
        0x40010000, MYOS_DEPLOY_PAGE_SIZE);

    const std::size_t proof_first = process_mapping_count;
    for (std::size_t segment = 0; segment < proof_segments; ++segment) {
        const std::size_t name = segment == 0 ? 14 : segment == 1 ? 15 : 41;
        mapping(mappings + (proof_first + segment) * MYOS_DEPLOY_MAPPING_STRIDE,
                name, 1, static_cast<std::uint32_t>(segment),
                segment == 0 ? MYOS_DEPLOY_CRITICAL_CODE
                              : MYOS_DEPLOY_CRITICAL_NONE,
                0, 0, 0);
    }
    const std::size_t proof_stack = proof_first + proof_segments;
    const std::size_t proof_bootstrap = proof_stack + 1;
    zero_mapping(mappings + proof_stack * MYOS_DEPLOY_MAPPING_STRIDE, 16,
                 MYOS_DEPLOY_CRITICAL_STACK, MYOS_VM_READ | MYOS_VM_WRITE,
                 0x41000000, 0x10000);
    zero_mapping(
        mappings + proof_bootstrap * MYOS_DEPLOY_MAPPING_STRIDE, 17,
        MYOS_DEPLOY_CRITICAL_BOOTSTRAP, MYOS_VM_READ,
        0x41010000, MYOS_DEPLOY_PAGE_SIZE);

    const std::size_t objects = tables[MYOS_DEPLOY_TABLE_OBJECT].offset;
    put(bytes, objects + MYOS_DEPLOY_OBJECT_OUTPUT_A, key(24), 8);
    put(bytes, objects + MYOS_DEPLOY_OBJECT_KIND,
        MYOS_OBJECT_KIND_NOTIFICATION, 2);
    put(bytes, objects + MYOS_DEPLOY_OBJECT_ARG0, 1, 8);
    put(bytes, objects + MYOS_DEPLOY_OBJECT_REF0,
        MYOS_DEPLOY_NO_INDEX, 4);
    put(bytes, objects + MYOS_DEPLOY_OBJECT_REF1,
        MYOS_DEPLOY_NO_INDEX, 4);
    put(bytes, objects + MYOS_DEPLOY_OBJECT_REF2,
        MYOS_DEPLOY_NO_INDEX, 4);
    put(bytes, objects + MYOS_DEPLOY_OBJECT_REF3,
        MYOS_DEPLOY_NO_INDEX, 4);
    const std::size_t object1 = objects + MYOS_DEPLOY_OBJECT_STRIDE;
    put(bytes, object1 + MYOS_DEPLOY_OBJECT_OUTPUT_A, key(25), 8);
    put(bytes, object1 + MYOS_DEPLOY_OBJECT_KIND,
        MYOS_OBJECT_KIND_NOTIFICATION, 2);
    put(bytes, object1 + MYOS_DEPLOY_OBJECT_ARG0, 2, 8);
    put(bytes, object1 + MYOS_DEPLOY_OBJECT_REF0,
        MYOS_DEPLOY_NO_INDEX, 4);
    put(bytes, object1 + MYOS_DEPLOY_OBJECT_REF1,
        MYOS_DEPLOY_NO_INDEX, 4);
    put(bytes, object1 + MYOS_DEPLOY_OBJECT_REF2,
        MYOS_DEPLOY_NO_INDEX, 4);
    put(bytes, object1 + MYOS_DEPLOY_OBJECT_REF3,
        MYOS_DEPLOY_NO_INDEX, 4);

    const std::size_t executions = tables[MYOS_DEPLOY_TABLE_EXECUTION].offset;
    const auto execution = [&](std::size_t offset, std::size_t key_index,
                               std::size_t sc_index, std::size_t domain_index,
                               std::uint32_t image, std::uint32_t stack,
                               std::uint32_t bootstrap,
                               std::uint64_t stack_top) {
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_KEY, key(key_index), 8);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_SC, key(sc_index), 8);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_DOMAIN,
            key(domain_index), 8);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_IMAGE, image, 4);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_STACK, stack, 4);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_BOOTSTRAP, bootstrap, 4);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_IPC,
            MYOS_DEPLOY_NO_INDEX, 4);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_CONTROL,
            MYOS_DEPLOY_NO_INDEX, 4);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_EVENT,
            MYOS_DEPLOY_NO_INDEX, 4);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_MODEL,
            MYOS_DEPLOY_EXECUTION_THREAD, 2);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_FAULT,
            MYOS_DEPLOY_EXECUTION_FAULT_TERMINATE, 2);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_TERMINAL,
            MYOS_DEPLOY_EXECUTION_TERMINAL_LEADER_EXIT, 2);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_STACK_TOP,
            stack_top, 8);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_SC_BUDGET,
            1'000'000, 8);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_SC_PERIOD,
            10'000'000, 8);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_URGENCY, 1, 4);
        put(bytes, offset + MYOS_DEPLOY_EXECUTION_HOME_CPU,
            MYOS_DEPLOY_HOME_CPU_ANY, 4);
    };
    execution(executions, 18, 19, 20, 0,
              static_cast<std::uint32_t>(process_stack),
              static_cast<std::uint32_t>(process_bootstrap), 0x40010000);
    execution(executions + MYOS_DEPLOY_EXECUTION_STRIDE,
              21, 22, 23, 1,
              static_cast<std::uint32_t>(proof_stack),
              static_cast<std::uint32_t>(proof_bootstrap), 0x41010000);

    const std::size_t imports = tables[MYOS_DEPLOY_TABLE_IMPORT].offset;
    const auto import = [&](std::size_t offset, std::size_t source,
                            std::size_t destination, std::uint16_t kind,
                            std::uint64_t rights, std::uint16_t source_class) {
        put(bytes, offset + MYOS_DEPLOY_IMPORT_SOURCE, key(source), 8);
        put(bytes, offset + MYOS_DEPLOY_IMPORT_DESTINATION,
            key(destination), 8);
        put(bytes, offset + MYOS_DEPLOY_IMPORT_MODE,
            MYOS_DEPLOY_IMPORT_DUPLICATE, 2);
        put(bytes, offset + MYOS_DEPLOY_IMPORT_SELECTOR,
            MYOS_DEPLOY_SELECTOR_ALLOCATED_KEYED, 2);
        attenuation(offset + MYOS_DEPLOY_IMPORT_ATTENUATION, kind, rights);
        put(bytes, offset + MYOS_DEPLOY_IMPORT_SOURCE_CLASS,
            source_class, 2);
    };
    constexpr std::uint64_t duplicate = MYOS_RIGHT_DUPLICATE;
    import(imports, 2, 26, MYOS_OBJECT_KIND_RESOURCE_POOL,
           MYOS_RIGHT_SPLIT,
           MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY);
    import(imports + MYOS_DEPLOY_IMPORT_STRIDE,
           3, 27, MYOS_OBJECT_KIND_VSPACE,
           duplicate | MYOS_RIGHT_CREATE_REGION | MYOS_RIGHT_MAP
               | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY | MYOS_RIGHT_INSPECT,
           MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY);
    import(imports + 2 * MYOS_DEPLOY_IMPORT_STRIDE,
           4, 28, MYOS_OBJECT_KIND_CSPACE,
           duplicate | MYOS_RIGHT_MANAGE,
           MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY);
    import(imports + 3 * MYOS_DEPLOY_IMPORT_STRIDE,
           36, 29, MYOS_OBJECT_KIND_SCHED_DOMAIN,
           duplicate | MYOS_RIGHT_CONTROL,
           MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY);
    import(imports + 4 * MYOS_DEPLOY_IMPORT_STRIDE,
           37, 30, MYOS_OBJECT_KIND_MEMORY,
           duplicate | MYOS_RIGHT_MAP | MYOS_RIGHT_INSPECT,
           MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY);

    import(imports + 5 * MYOS_DEPLOY_IMPORT_STRIDE,
           5, 31, MYOS_OBJECT_KIND_RESOURCE_POOL,
           duplicate | MYOS_RIGHT_CREATE,
           MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY);
    import(imports + 6 * MYOS_DEPLOY_IMPORT_STRIDE,
           6, 32, MYOS_OBJECT_KIND_VSPACE,
           duplicate | MYOS_RIGHT_CREATE_REGION | MYOS_RIGHT_MAP
               | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY | MYOS_RIGHT_INSPECT,
           MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY);
    import(imports + 7 * MYOS_DEPLOY_IMPORT_STRIDE,
           7, 33, MYOS_OBJECT_KIND_CSPACE,
           duplicate | MYOS_RIGHT_MANAGE,
           MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY);
    import(imports + 8 * MYOS_DEPLOY_IMPORT_STRIDE,
           38, 34, MYOS_OBJECT_KIND_SCHED_DOMAIN,
           duplicate | MYOS_RIGHT_CONTROL,
           MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY);
    import(imports + 9 * MYOS_DEPLOY_IMPORT_STRIDE,
           39, 35, MYOS_OBJECT_KIND_MEMORY,
           duplicate | MYOS_RIGHT_MAP | MYOS_RIGHT_INSPECT,
           MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY);

    const std::size_t bootstraps =
        tables[MYOS_DEPLOY_TABLE_BOOTSTRAP].offset;
    const auto bootstrap = [&](std::size_t offset, std::uint32_t kind,
                               std::size_t destination) {
        put(bytes, offset + MYOS_DEPLOY_BOOTSTRAP_KIND, kind, 4);
        put(bytes, offset + MYOS_DEPLOY_BOOTSTRAP_DESTINATION,
            key(destination), 8);
    };
    constexpr std::uint32_t bootstrap_kinds[] = {
        MYOS_BOOTSTRAP_CAP_RESOURCE_POOL,
        MYOS_BOOTSTRAP_CAP_VSPACE,
        MYOS_BOOTSTRAP_CAP_CSPACE,
        MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN,
        MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE,
    };
    constexpr std::size_t process_destinations[] = {26, 27, 28, 29, 30};
    constexpr std::size_t proof_destinations[] = {31, 32, 33, 34, 35};
    for (std::size_t index = 0; index < 5; ++index) {
        bootstrap(bootstraps + index * MYOS_DEPLOY_BOOTSTRAP_STRIDE,
                  bootstrap_kinds[index], process_destinations[index]);
        bootstrap(bootstraps + (index + 5) * MYOS_DEPLOY_BOOTSTRAP_STRIDE,
                  bootstrap_kinds[index], proof_destinations[index]);
    }

    const std::size_t string_table = tables[MYOS_DEPLOY_TABLE_STRING].offset;
    for (std::size_t index = 0; index < string_count; ++index) {
        for (std::size_t byte = 0; byte < strings[index].size(); ++byte) {
            bytes[string_table + keys[index].offset + byte] =
                static_cast<std::uint8_t>(strings[index][byte]);
        }
    }
    finalize(bytes, tables);
    return bytes;
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
    std::vector<std::uint8_t> bytes(208, 0);
    Table tables[9]{};
    tables[MYOS_DEPLOY_TABLE_TASK] = append_table(
        bytes, 1, MYOS_DEPLOY_TASK_STRIDE);
    tables[MYOS_DEPLOY_TABLE_IMAGE] = append_table(
        bytes, 1, MYOS_DEPLOY_IMAGE_STRIDE);
    tables[MYOS_DEPLOY_TABLE_MAPPING] = append_table(
        bytes, 3, MYOS_DEPLOY_MAPPING_STRIDE);
    tables[MYOS_DEPLOY_TABLE_OBJECT] = append_table(
        bytes, 1, MYOS_DEPLOY_OBJECT_STRIDE);
    tables[MYOS_DEPLOY_TABLE_EXECUTION] = append_table(
        bytes, 1, MYOS_DEPLOY_EXECUTION_STRIDE);
    tables[MYOS_DEPLOY_TABLE_IMPORT] = append_table(
        bytes, 1, MYOS_DEPLOY_IMPORT_STRIDE);
    tables[MYOS_DEPLOY_TABLE_DEPENDENCY] = append_table(
        bytes, 0, MYOS_DEPLOY_DEPENDENCY_STRIDE);
    tables[MYOS_DEPLOY_TABLE_EXPORT] = append_table(
        bytes, 1, MYOS_DEPLOY_EXPORT_STRIDE);
    tables[MYOS_DEPLOY_TABLE_STRING] = append_table(bytes, string_bytes, 1);

    const std::size_t task = tables[MYOS_DEPLOY_TABLE_TASK].offset;
    put(bytes, task + MYOS_DEPLOY_TASK_NAME, keys[0].packed(), 8);
    put(bytes, task + MYOS_DEPLOY_TASK_POOL, keys[1].packed(), 8);
    put(bytes, task + MYOS_DEPLOY_TASK_VSPACE, keys[2].packed(), 8);
    put(bytes, task + MYOS_DEPLOY_TASK_CSPACE, keys[3].packed(), 8);
    put(bytes, task + MYOS_DEPLOY_TASK_IMAGE_FIRST, 0, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_IMAGE_COUNT, 1, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_MAPPING_FIRST, 0, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_MAPPING_COUNT, 3, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_OBJECT_FIRST, 0, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_OBJECT_COUNT, 1, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_EXECUTION_FIRST, 0, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_EXECUTION_COUNT, 1, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_IMPORT_FIRST, 0, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_IMPORT_COUNT, 1, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_DEPENDENCY_FIRST, 0, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_DEPENDENCY_COUNT, 0, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_EXPORT_FIRST, 0, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_EXPORT_COUNT, 1, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_POOL_MEMORY, 16384, 8);
    put(bytes, task + MYOS_DEPLOY_TASK_POOL_CAPS, 16, 8);
    put(bytes, task + MYOS_DEPLOY_TASK_KIND_MASK, MYOS_RESOURCE_E2_KINDS, 8);
    put(bytes, task + MYOS_DEPLOY_TASK_CRITICAL_BYTES, 12288, 8);
    put(bytes, task + MYOS_DEPLOY_TASK_CSPACE_SLOTS, 16, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_CSPACE_PAGES, 1, 4);
    put(bytes, task + MYOS_DEPLOY_TASK_BOOTSTRAP_MAPPING, 2, 4);

    const std::size_t image = tables[MYOS_DEPLOY_TABLE_IMAGE].offset;
    put(bytes, image + MYOS_DEPLOY_IMAGE_SOURCE, keys[0].packed(), 8);

    const std::size_t mapping = tables[MYOS_DEPLOY_TABLE_MAPPING].offset;
    put(bytes, mapping + MYOS_DEPLOY_MAPPING_PRODUCED, keys[4].packed(), 8);
    put(bytes, mapping + MYOS_DEPLOY_MAPPING_IMAGE, 0, 4);
    put(bytes, mapping + MYOS_DEPLOY_MAPPING_SEGMENT, 0, 4);
    put(bytes, mapping + MYOS_DEPLOY_MAPPING_SOURCE,
        MYOS_DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT, 2);
    put(bytes, mapping + MYOS_DEPLOY_MAPPING_RESIDENCY,
        MYOS_DEPLOY_MAPPING_RESIDENT, 2);
    put(bytes, mapping + MYOS_DEPLOY_MAPPING_CRITICAL,
        MYOS_DEPLOY_CRITICAL_CODE, 2);

    const std::size_t stack_mapping = mapping + MYOS_DEPLOY_MAPPING_STRIDE;
    put(bytes, stack_mapping + MYOS_DEPLOY_MAPPING_PRODUCED,
        keys[5].packed(), 8);
    put(bytes, stack_mapping + MYOS_DEPLOY_MAPPING_IMAGE,
        MYOS_DEPLOY_NO_INDEX, 4);
    put(bytes, stack_mapping + MYOS_DEPLOY_MAPPING_SEGMENT,
        MYOS_DEPLOY_NO_INDEX, 4);
    put(bytes, stack_mapping + MYOS_DEPLOY_MAPPING_SOURCE,
        MYOS_DEPLOY_MAPPING_SOURCE_ZERO, 2);
    put(bytes, stack_mapping + MYOS_DEPLOY_MAPPING_RESIDENCY,
        MYOS_DEPLOY_MAPPING_RESIDENT, 2);
    put(bytes, stack_mapping + MYOS_DEPLOY_MAPPING_CRITICAL,
        MYOS_DEPLOY_CRITICAL_STACK, 2);
    put(bytes, stack_mapping + MYOS_DEPLOY_MAPPING_ACCESS,
        MYOS_VM_READ | MYOS_VM_WRITE, 4);
    put(bytes, stack_mapping + MYOS_DEPLOY_MAPPING_ADDRESS, 0x210000, 8);
    put(bytes, stack_mapping + MYOS_DEPLOY_MAPPING_SIZE, 4096, 8);

    const std::size_t bootstrap_mapping =
        stack_mapping + MYOS_DEPLOY_MAPPING_STRIDE;
    put(bytes, bootstrap_mapping + MYOS_DEPLOY_MAPPING_PRODUCED,
        keys[6].packed(), 8);
    put(bytes, bootstrap_mapping + MYOS_DEPLOY_MAPPING_IMAGE,
        MYOS_DEPLOY_NO_INDEX, 4);
    put(bytes, bootstrap_mapping + MYOS_DEPLOY_MAPPING_SEGMENT,
        MYOS_DEPLOY_NO_INDEX, 4);
    put(bytes, bootstrap_mapping + MYOS_DEPLOY_MAPPING_SOURCE,
        MYOS_DEPLOY_MAPPING_SOURCE_ZERO, 2);
    put(bytes, bootstrap_mapping + MYOS_DEPLOY_MAPPING_RESIDENCY,
        MYOS_DEPLOY_MAPPING_RESIDENT, 2);
    put(bytes, bootstrap_mapping + MYOS_DEPLOY_MAPPING_CRITICAL,
        MYOS_DEPLOY_CRITICAL_BOOTSTRAP, 2);
    put(bytes, bootstrap_mapping + MYOS_DEPLOY_MAPPING_ACCESS,
        MYOS_VM_READ, 4);
    put(bytes, bootstrap_mapping + MYOS_DEPLOY_MAPPING_ADDRESS, 0x220000, 8);
    put(bytes, bootstrap_mapping + MYOS_DEPLOY_MAPPING_SIZE, 4096, 8);

    const std::size_t object = tables[MYOS_DEPLOY_TABLE_OBJECT].offset;
    put(bytes, object + MYOS_DEPLOY_OBJECT_OUTPUT_A, keys[7].packed(), 8);
    put(bytes, object + MYOS_DEPLOY_OBJECT_KIND,
        MYOS_OBJECT_KIND_NOTIFICATION, 2);
    put(bytes, object + MYOS_DEPLOY_OBJECT_ARG0, 1, 8);
    for (std::size_t field = MYOS_DEPLOY_OBJECT_REF0;
         field <= MYOS_DEPLOY_OBJECT_REF3;
         field += sizeof(std::uint32_t)) {
        put(bytes, object + field, MYOS_DEPLOY_NO_INDEX, 4);
    }

    const std::size_t execution = tables[MYOS_DEPLOY_TABLE_EXECUTION].offset;
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_KEY, keys[8].packed(), 8);
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_SC, keys[9].packed(), 8);
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_DOMAIN, keys[10].packed(), 8);
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_IMAGE, 0, 4);
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_STACK, 1, 4);
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_BOOTSTRAP, 2, 4);
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_IPC,
        MYOS_DEPLOY_NO_INDEX, 4);
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_CONTROL,
        MYOS_DEPLOY_NO_INDEX, 4);
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_EVENT,
        MYOS_DEPLOY_NO_INDEX, 4);
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_ENTRY, 0x200000, 8);
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_STACK_TOP, 0x211000, 8);
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_SC_BUDGET, 1, 8);
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_SC_PERIOD, 1, 8);
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_URGENCY, 0, 4);
    put(bytes, execution + MYOS_DEPLOY_EXECUTION_HOME_CPU,
        MYOS_DEPLOY_HOME_CPU_ANY, 4);

    const std::size_t import = tables[MYOS_DEPLOY_TABLE_IMPORT].offset;
    put(bytes, import + MYOS_DEPLOY_IMPORT_SOURCE, keys[13].packed(), 8);
    put(bytes, import + MYOS_DEPLOY_IMPORT_DESTINATION, keys[11].packed(), 8);
    put(bytes, import + MYOS_DEPLOY_IMPORT_MODE,
        MYOS_DEPLOY_IMPORT_DUPLICATE, 2);
    put(bytes, import + MYOS_DEPLOY_IMPORT_SELECTOR,
        MYOS_DEPLOY_SELECTOR_ALLOCATED_KEYED, 2);
    put(bytes, import + MYOS_DEPLOY_IMPORT_ATTENUATION
            + MYOS_DEPLOY_ATTENUATION_VERSION,
        MYOS_DEPLOY_ATTENUATION_VERSION_CURRENT, 2);
    put(bytes, import + MYOS_DEPLOY_IMPORT_ATTENUATION
            + MYOS_DEPLOY_ATTENUATION_KIND,
        MYOS_OBJECT_KIND_THREAD, 2);
    put(bytes, import + MYOS_DEPLOY_IMPORT_ATTENUATION
            + MYOS_DEPLOY_ATTENUATION_SIZE,
        MYOS_DEPLOY_ATTENUATION_STRIDE, 4);

    const std::size_t output = tables[MYOS_DEPLOY_TABLE_EXPORT].offset;
    put(bytes, output + MYOS_DEPLOY_EXPORT_SOURCE, keys[8].packed(), 8);
    put(bytes, output + MYOS_DEPLOY_EXPORT_KEY, keys[12].packed(), 8);
    put(bytes, output + MYOS_DEPLOY_EXPORT_CLASS,
        MYOS_DEPLOY_EXPORT_PREPARED_KEY, 2);
    put(bytes, output + MYOS_DEPLOY_EXPORT_CEILING
            + MYOS_DEPLOY_ATTENUATION_VERSION,
        MYOS_DEPLOY_ATTENUATION_VERSION_CURRENT, 2);
    put(bytes, output + MYOS_DEPLOY_EXPORT_CEILING
            + MYOS_DEPLOY_ATTENUATION_KIND,
        MYOS_OBJECT_KIND_THREAD, 2);
    put(bytes, output + MYOS_DEPLOY_EXPORT_CEILING
            + MYOS_DEPLOY_ATTENUATION_SIZE,
        MYOS_DEPLOY_ATTENUATION_STRIDE, 4);

    const std::size_t string_table = tables[MYOS_DEPLOY_TABLE_STRING].offset;
    std::size_t string_offset{};
    for (const char* string : strings) {
        for (std::size_t index = 0; string[index] != '\0'; ++index) {
            bytes[string_table + string_offset++] =
                static_cast<std::uint8_t>(string[index]);
        }
    }

    finalize_legacy(bytes, tables);
    return bytes;
}

/*
 * Unit 4's userspace route is emitted by this tool rather than by the
 * freestanding scenario.  The scenario therefore consumes exactly the same
 * little-endian table writer as the host fixture; it does not carry a second
 * manifest encoder or construct PlanStorage by hand.
 */
} // namespace myos::deploy::host
