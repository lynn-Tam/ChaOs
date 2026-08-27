#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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
    std::vector<std::uint8_t> bytes(MYOS_DEPLOY_HEADER_SIZE, 0);
    Table tables[MYOS_DEPLOY_TABLE_COUNT]{};
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

    finalize(bytes, tables);
    return bytes;
}

/*
 * Unit 4's userspace route is emitted by this tool rather than by the
 * freestanding scenario.  The scenario therefore consumes exactly the same
 * little-endian table writer as the host fixture; it does not carry a second
 * manifest encoder or construct PlanStorage by hand.
 */
} // namespace myos::deploy::host
