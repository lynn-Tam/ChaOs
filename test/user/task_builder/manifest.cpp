#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "tools/deploypack/packer.hpp"

namespace myos::deploy::task_builder_test {

using namespace myos::deploy::host;

inline auto pack_task_builder_fixture() -> std::vector<std::uint8_t> {
    struct KeyRef final {
        std::uint32_t offset{};
        std::uint32_t length{};

        [[nodiscard]] auto packed() const noexcept -> std::uint64_t {
            return static_cast<std::uint64_t>(offset)
                | (static_cast<std::uint64_t>(length) << 32);
        }
    };
    struct RowSpec final {
        std::uint32_t images{};
        std::uint32_t mappings{};
        std::uint32_t objects{};
        std::uint32_t executions{};
        std::uint32_t imports{};
        std::uint32_t exports{};
        std::uint64_t pool_memory{};
        std::uint64_t critical_bytes{};
    };
    /*
     * Row zero is an independent early-budget probe.  Rows one through four
     * deliberately have distinct construction prefixes:
     * typed-only, execution/terminal, critical PT_LOAD/BSS, and valid Prepared.
     */
    constexpr RowSpec specs[] = {
        {0, 1, 0, 0, 1, 1, 16 * 1024, 4 * 1024},
        {0, 1, 0, 0, 1, 1, 8 * 1024 * 1024, 4 * 1024},
        {1, 4, 1, 1, 1, 1, 8 * 1024 * 1024, 8 * 1024},
        {1, 4, 0, 0, 1, 1, 8 * 1024 * 1024, 8 * 1024},
        {1, 5, 2, 1, 1, 1, 8 * 1024 * 1024, 12 * 1024},
    };
    constexpr std::size_t task_count = sizeof(specs) / sizeof(specs[0]);

    std::vector<std::string> strings;
    const auto key = [&](const std::string& value) {
        for (std::size_t index = 0; index < strings.size(); ++index) {
            if (strings[index] == value) {
                std::size_t offset = 0;
                for (std::size_t previous = 0; previous < index; ++previous) {
                    offset += strings[previous].size();
                }
                return KeyRef{static_cast<std::uint32_t>(offset),
                              static_cast<std::uint32_t>(value.size())};
            }
        }
        std::size_t offset = 0;
        for (const std::string& existing : strings) {
            offset += existing.size();
        }
        strings.push_back(value);
        return KeyRef{static_cast<std::uint32_t>(offset),
                      static_cast<std::uint32_t>(value.size())};
    };

    struct Ranges final {
        Table table{};
        std::uint32_t image_first{};
        std::uint32_t mapping_first{};
        std::uint32_t object_first{};
        std::uint32_t execution_first{};
        std::uint32_t import_first{};
        std::uint32_t export_first{};
    };
    Ranges ranges[task_count]{};
    std::uint32_t image_count{};
    std::uint32_t mapping_count{};
    std::uint32_t object_count{};
    std::uint32_t execution_count{};
    std::uint32_t import_count{};
    std::uint32_t export_count{};
    for (const RowSpec& spec : specs) {
        image_count += spec.images;
        mapping_count += spec.mappings;
        object_count += spec.objects;
        execution_count += spec.executions;
        import_count += spec.imports;
        export_count += spec.exports;
    }

    std::vector<std::uint8_t> bytes(MYOS_DEPLOY_HEADER_SIZE, 0);
    Table tables[MYOS_DEPLOY_TABLE_COUNT]{};
    tables[MYOS_DEPLOY_TABLE_TASK] = append_table(
        bytes, static_cast<std::uint32_t>(task_count),
        MYOS_DEPLOY_TASK_STRIDE);
    tables[MYOS_DEPLOY_TABLE_IMAGE] = append_table(
        bytes, image_count, MYOS_DEPLOY_IMAGE_STRIDE);
    tables[MYOS_DEPLOY_TABLE_MAPPING] = append_table(
        bytes, mapping_count, MYOS_DEPLOY_MAPPING_STRIDE);
    tables[MYOS_DEPLOY_TABLE_OBJECT] = append_table(
        bytes, object_count, MYOS_DEPLOY_OBJECT_STRIDE);
    tables[MYOS_DEPLOY_TABLE_EXECUTION] = append_table(
        bytes, execution_count, MYOS_DEPLOY_EXECUTION_STRIDE);
    tables[MYOS_DEPLOY_TABLE_IMPORT] = append_table(
        bytes, import_count, MYOS_DEPLOY_IMPORT_STRIDE);
    tables[MYOS_DEPLOY_TABLE_DEPENDENCY] = append_table(
        bytes, 0, MYOS_DEPLOY_DEPENDENCY_STRIDE);
    tables[MYOS_DEPLOY_TABLE_EXPORT] = append_table(
        bytes, export_count, MYOS_DEPLOY_EXPORT_STRIDE);

    std::uint32_t image_cursor{};
    std::uint32_t mapping_cursor{};
    std::uint32_t object_cursor{};
    std::uint32_t execution_cursor{};
    std::uint32_t import_cursor{};
    std::uint32_t export_cursor{};
    for (std::size_t task = 0; task < task_count; ++task) {
        Ranges& range = ranges[task];
        const RowSpec& spec = specs[task];
        range.image_first = image_cursor;
        range.mapping_first = mapping_cursor;
        range.object_first = object_cursor;
        range.execution_first = execution_cursor;
        range.import_first = import_cursor;
        range.export_first = export_cursor;
        image_cursor += spec.images;
        mapping_cursor += spec.mappings;
        object_cursor += spec.objects;
        execution_cursor += spec.executions;
        import_cursor += spec.imports;
        export_cursor += spec.exports;
    }

    const KeyRef child = key("child");
    const KeyRef typed_source = key("typed-source");
    const KeyRef domain = key("domain");
    const auto task_offset = [&](std::size_t index) {
        return tables[MYOS_DEPLOY_TABLE_TASK].offset
            + index * MYOS_DEPLOY_TASK_STRIDE;
    };
    const auto image_offset = [&](std::size_t index) {
        return tables[MYOS_DEPLOY_TABLE_IMAGE].offset
            + index * MYOS_DEPLOY_IMAGE_STRIDE;
    };
    const auto mapping_offset = [&](std::size_t index) {
        return tables[MYOS_DEPLOY_TABLE_MAPPING].offset
            + index * MYOS_DEPLOY_MAPPING_STRIDE;
    };
    const auto object_offset = [&](std::size_t index) {
        return tables[MYOS_DEPLOY_TABLE_OBJECT].offset
            + index * MYOS_DEPLOY_OBJECT_STRIDE;
    };
    const auto execution_offset = [&](std::size_t index) {
        return tables[MYOS_DEPLOY_TABLE_EXECUTION].offset
            + index * MYOS_DEPLOY_EXECUTION_STRIDE;
    };
    const auto import_offset = [&](std::size_t index) {
        return tables[MYOS_DEPLOY_TABLE_IMPORT].offset
            + index * MYOS_DEPLOY_IMPORT_STRIDE;
    };
    const auto export_offset = [&](std::size_t index) {
        return tables[MYOS_DEPLOY_TABLE_EXPORT].offset
            + index * MYOS_DEPLOY_EXPORT_STRIDE;
    };
    const auto mapping_key = [&](std::size_t task, const char* suffix) {
        return key("t" + std::to_string(task) + "_" + suffix);
    };
    const auto object_key = [&](std::size_t task, const char* suffix) {
        return key("t" + std::to_string(task) + "_" + suffix);
    };
    const auto execution_key = [&](std::size_t task) {
        return key("t" + std::to_string(task) + "_thread");
    };
    const auto sc_key = [&](std::size_t task) {
        return key("t" + std::to_string(task) + "_sc");
    };
    const auto import_key = [&](std::size_t task) {
        return key("t" + std::to_string(task) + "_import");
    };
    const auto export_key = [&](std::size_t task) {
        return key("t" + std::to_string(task) + "_export");
    };
    const auto task_name = [&](std::size_t task) {
        constexpr const char* names[] = {
            "budget-probe", "typed-only", "terminal",
            "critical", "prepared",
        };
        return key(names[task]);
    };

    const auto write_descriptor = [&](std::size_t offset,
                                      std::uint16_t kind,
                                      std::uint64_t rights,
                                      std::uint64_t word0,
                                      std::uint64_t word1,
                                      std::uint64_t word2,
                                      std::uint64_t word3) {
        put(bytes, offset + MYOS_DEPLOY_ATTENUATION_VERSION,
            MYOS_DEPLOY_ATTENUATION_VERSION_CURRENT, 2);
        put(bytes, offset + MYOS_DEPLOY_ATTENUATION_KIND, kind, 2);
        put(bytes, offset + MYOS_DEPLOY_ATTENUATION_SIZE,
            MYOS_DEPLOY_ATTENUATION_STRIDE, 4);
        put(bytes, offset + MYOS_DEPLOY_ATTENUATION_RIGHTS, rights, 8);
        put(bytes, offset + MYOS_DEPLOY_ATTENUATION_WORD0, word0, 8);
        put(bytes, offset + MYOS_DEPLOY_ATTENUATION_WORD1, word1, 8);
        put(bytes, offset + MYOS_DEPLOY_ATTENUATION_WORD2, word2, 8);
        put(bytes, offset + MYOS_DEPLOY_ATTENUATION_WORD3, word3, 8);
    };

    for (std::size_t task = 0; task < task_count; ++task) {
        const RowSpec& spec = specs[task];
        const Ranges& range = ranges[task];
        const std::size_t task_row = task_offset(task);
        put(bytes, task_row + MYOS_DEPLOY_TASK_NAME,
            task_name(task).packed(), 8);
        put(bytes, task_row + MYOS_DEPLOY_TASK_POOL,
            key("t" + std::to_string(task) + "_pool").packed(), 8);
        put(bytes, task_row + MYOS_DEPLOY_TASK_VSPACE,
            key("t" + std::to_string(task) + "_vspace").packed(), 8);
        put(bytes, task_row + MYOS_DEPLOY_TASK_CSPACE,
            key("t" + std::to_string(task) + "_cspace").packed(), 8);
        put(bytes, task_row + MYOS_DEPLOY_TASK_IMAGE_FIRST,
            range.image_first, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_IMAGE_COUNT,
            spec.images, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_MAPPING_FIRST,
            range.mapping_first, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_MAPPING_COUNT,
            spec.mappings, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_OBJECT_FIRST,
            range.object_first, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_OBJECT_COUNT,
            spec.objects, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_EXECUTION_FIRST,
            range.execution_first, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_EXECUTION_COUNT,
            spec.executions, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_IMPORT_FIRST,
            range.import_first, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_IMPORT_COUNT,
            spec.imports, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_DEPENDENCY_FIRST, 0, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_DEPENDENCY_COUNT, 0, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_EXPORT_FIRST,
            range.export_first, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_EXPORT_COUNT,
            spec.exports, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_POOL_MEMORY,
            spec.pool_memory, 8);
        put(bytes, task_row + MYOS_DEPLOY_TASK_POOL_CAPS, 512, 8);
        put(bytes, task_row + MYOS_DEPLOY_TASK_KIND_MASK,
            MYOS_RESOURCE_E4_KINDS, 8);
        put(bytes, task_row + MYOS_DEPLOY_TASK_CRITICAL_BYTES,
            spec.critical_bytes, 8);
        put(bytes, task_row + MYOS_DEPLOY_TASK_CSPACE_SLOTS, 128, 4);
        put(bytes, task_row + MYOS_DEPLOY_TASK_CSPACE_PAGES, 3, 4);
        const bool has_image = spec.images != 0;
        const std::uint32_t bootstrap_mapping = has_image
            ? range.mapping_first + 3 : range.mapping_first;
        put(bytes, task_row + MYOS_DEPLOY_TASK_BOOTSTRAP_MAPPING,
            bootstrap_mapping, 4);

        if (spec.images != 0) {
            put(bytes, image_offset(range.image_first)
                    + MYOS_DEPLOY_IMAGE_SOURCE, child.packed(), 8);
        }

        const KeyRef code = mapping_key(task, "code");
        const KeyRef data = mapping_key(task, "data");
        const KeyRef stack = mapping_key(task, "stack");
        const KeyRef bootstrap = mapping_key(task, "bootstrap");
        const KeyRef descriptor = mapping_key(task, "descriptor");
        const std::size_t image_mappings = has_image ? 2 : 0;
        for (std::size_t local = 0; local < spec.mappings; ++local) {
            const std::size_t row = mapping_offset(
                range.mapping_first + local);
            const bool image_mapping = has_image && local < image_mappings;
            const std::size_t zero_index = image_mapping
                ? 0 : local - image_mappings;
            const KeyRef produced = image_mapping
                ? (local == 0 ? code : data)
                : (!has_image ? mapping_key(task, "mapped")
                              : zero_index == 0 ? stack
                              : zero_index == 1 ? bootstrap : descriptor);
            put(bytes, row + MYOS_DEPLOY_MAPPING_PRODUCED,
                produced.packed(), 8);
            put(bytes, row + MYOS_DEPLOY_MAPPING_IMAGE,
                image_mapping ? range.image_first : MYOS_DEPLOY_NO_INDEX, 4);
            put(bytes, row + MYOS_DEPLOY_MAPPING_SEGMENT,
                image_mapping ? local : MYOS_DEPLOY_NO_INDEX, 4);
            put(bytes, row + MYOS_DEPLOY_MAPPING_SOURCE,
                image_mapping ? MYOS_DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
                              : MYOS_DEPLOY_MAPPING_SOURCE_ZERO, 2);
            put(bytes, row + MYOS_DEPLOY_MAPPING_RESIDENCY,
                MYOS_DEPLOY_MAPPING_RESIDENT, 2);

            std::uint16_t critical = MYOS_DEPLOY_CRITICAL_NONE;
            if (task >= 3 && image_mapping && local == 0) {
                critical = MYOS_DEPLOY_CRITICAL_CODE;
            } else if (!image_mapping && zero_index == 0
                       && spec.executions != 0) {
                critical = MYOS_DEPLOY_CRITICAL_STACK;
            } else if (!image_mapping && has_image && zero_index == 1
                       && task >= 2) {
                critical = MYOS_DEPLOY_CRITICAL_BOOTSTRAP;
            }
            put(bytes, row + MYOS_DEPLOY_MAPPING_CRITICAL, critical, 2);
            if (!image_mapping) {
                const bool readonly = (!has_image)
                    || (has_image && zero_index == 1);
                put(bytes, row + MYOS_DEPLOY_MAPPING_ACCESS,
                    readonly ? MYOS_VM_READ
                             : MYOS_VM_READ | MYOS_VM_WRITE, 4);
                const std::uint64_t base = readonly
                    ? 0x2200'0000ULL
                    : has_image && zero_index == 0
                        ? 0x2100'0000ULL : 0x2300'0000ULL;
                put(bytes, row + MYOS_DEPLOY_MAPPING_ADDRESS,
                    base + task * 0x20'0000ULL, 8);
                put(bytes, row + MYOS_DEPLOY_MAPPING_SIZE,
                    MYOS_DEPLOY_PAGE_SIZE, 8);
            }
        }

        if (spec.objects != 0) {
            const std::size_t notification = object_offset(range.object_first);
            put(bytes, notification + MYOS_DEPLOY_OBJECT_OUTPUT,
                object_key(task, "notify").packed(), 8);
            put(bytes, notification + MYOS_DEPLOY_OBJECT_KIND,
                MYOS_OBJECT_KIND_NOTIFICATION, 2);
            put(bytes, notification + MYOS_DEPLOY_OBJECT_ARG0, 0x40 + task, 8);
            for (std::size_t field = MYOS_DEPLOY_OBJECT_REF0;
                 field <= MYOS_DEPLOY_OBJECT_REF3;
                 field += sizeof(std::uint32_t)) {
                put(bytes, notification + field, MYOS_DEPLOY_NO_INDEX, 4);
            }
        }
        if (spec.objects > 1) {
            const std::size_t endpoint = object_offset(
                range.object_first + 1);
            put(bytes, endpoint + MYOS_DEPLOY_OBJECT_OUTPUT,
                object_key(task, "endpoint").packed(), 8);
            put(bytes, endpoint + MYOS_DEPLOY_OBJECT_KIND,
                MYOS_OBJECT_KIND_ENDPOINT, 2);
            put(bytes, endpoint + MYOS_DEPLOY_OBJECT_FLAGS,
                MYOS_DEPLOY_OBJECT_POST_MAPPING, 2);
            put(bytes, endpoint + MYOS_DEPLOY_OBJECT_REF0,
                range.mapping_first + 4, 4);
            for (std::size_t field = MYOS_DEPLOY_OBJECT_REF1;
                 field <= MYOS_DEPLOY_OBJECT_REF3;
                 field += sizeof(std::uint32_t)) {
                put(bytes, endpoint + field, MYOS_DEPLOY_NO_INDEX, 4);
            }
        }

        if (spec.executions != 0) {
            const std::size_t execution = execution_offset(
                range.execution_first);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_KEY,
                execution_key(task).packed(), 8);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_SC,
                sc_key(task).packed(), 8);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_DOMAIN,
                domain.packed(), 8);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_IMAGE,
                range.image_first, 4);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_STACK,
                range.mapping_first + 2, 4);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_BOOTSTRAP,
                range.mapping_first + 3, 4);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_IPC,
                MYOS_DEPLOY_NO_INDEX, 4);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_CONTROL,
                MYOS_DEPLOY_NO_INDEX, 4);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_EVENT,
                MYOS_DEPLOY_NO_INDEX, 4);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_MODEL,
                MYOS_DEPLOY_EXECUTION_THREAD, 2);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_FAULT,
                MYOS_DEPLOY_EXECUTION_FAULT_TERMINATE, 2);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_TERMINAL,
                MYOS_DEPLOY_EXECUTION_TERMINAL_LEADER_EXIT, 2);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_ENTRY, 0x200000, 8);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_STACK_TOP,
                0x2100'1000ULL + task * 0x20'0000ULL, 8);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_SC_BUDGET,
                1'000'000, 8);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_SC_PERIOD,
                10'000'000, 8);
            put(bytes, execution + MYOS_DEPLOY_EXECUTION_HOME_CPU,
                MYOS_DEPLOY_HOME_CPU_ANY, 4);
        }

        const std::size_t import = import_offset(range.import_first);
        put(bytes, import + MYOS_DEPLOY_IMPORT_SOURCE,
            typed_source.packed(), 8);
        put(bytes, import + MYOS_DEPLOY_IMPORT_DESTINATION,
            import_key(task).packed(), 8);
        put(bytes, import + MYOS_DEPLOY_IMPORT_MODE,
            MYOS_DEPLOY_IMPORT_TYPED_DELEGATE, 2);
        put(bytes, import + MYOS_DEPLOY_IMPORT_SELECTOR,
            MYOS_DEPLOY_SELECTOR_ALLOCATED_KEYED, 2);
        write_descriptor(
            import + MYOS_DEPLOY_IMPORT_ATTENUATION,
            MYOS_OBJECT_KIND_MEMORY, MYOS_RIGHT_MAP,
            0, 1, MYOS_VM_READ, MYOS_VM_NORMAL);

        const std::size_t output = export_offset(range.export_first);
        const bool valid_prepared = task == 4;
        const KeyRef source = valid_prepared
            ? execution_key(task)
            : has_image ? bootstrap : mapping_key(task, "mapped");
        put(bytes, output + MYOS_DEPLOY_EXPORT_SOURCE, source.packed(), 8);
        put(bytes, output + MYOS_DEPLOY_EXPORT_KEY,
            export_key(task).packed(), 8);
        put(bytes, output + MYOS_DEPLOY_EXPORT_CLASS,
            MYOS_DEPLOY_EXPORT_PREPARED_KEY, 2);
        write_descriptor(
            output + MYOS_DEPLOY_EXPORT_CEILING,
            valid_prepared ? MYOS_OBJECT_KIND_THREAD
                           : MYOS_OBJECT_KIND_MEMORY,
            valid_prepared ? 0 : MYOS_RIGHT_MAP,
            valid_prepared ? 0 : 0,
            valid_prepared ? 0 : 1,
            valid_prepared ? 0 : MYOS_VM_READ,
            valid_prepared ? 0 : MYOS_VM_NORMAL);
    }

    std::size_t strings_size{};
    for (const std::string& value : strings) {
        strings_size += value.size();
    }
    tables[MYOS_DEPLOY_TABLE_STRING] = append_table(
        bytes, static_cast<std::uint32_t>(strings_size), 1);
    const std::size_t string_table = tables[MYOS_DEPLOY_TABLE_STRING].offset;
    std::size_t string_offset{};
    for (const std::string& value : strings) {
        for (const char byte : value) {
            bytes[string_table + string_offset++] =
                static_cast<std::uint8_t>(byte);
        }
    }

    finalize(bytes, tables);
    return bytes;
}

inline auto emit_task_builder_source(
    const std::vector<std::uint8_t>& bytes) -> std::string {
    std::string output;
    output += "#include <stddef.h>\n#include <stdint.h>\n\n";
    output += "namespace myos::task_builder_fixture {\n";
    output += "alignas(8) extern const uint8_t manifest[] = {";
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if ((index % 12) == 0) {
            output += "\n    ";
        }
        output += std::to_string(bytes[index]);
        output += index + 1 == bytes.size() ? "" : ", ";
    }
    output += "\n};\n";
    output += "extern const size_t manifest_size = sizeof(manifest);\n";
    output += "} // namespace myos::task_builder_fixture\n";
    return output;
}
} // namespace myos::deploy::task_builder_test

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const auto bytes =
        myos::deploy::task_builder_test::pack_task_builder_fixture();
    const std::string source =
        myos::deploy::task_builder_test::emit_task_builder_source(bytes);
    std::ofstream output(argv[1]);
    if (!output) {
        return 1;
    }
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    return output ? 0 : 1;
}
