#pragma once

#include <tools/deploypack/manifest.hpp>
#include <test/user/channel/export_protocol.hpp>
#include <test/user/io/file_fault.hpp>

namespace deploy::host {
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
    put(bytes, task + DEPLOY_TASK_KIND_MASK, DEPLOY_BASE_KINDS, 8);
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
        VM_READ | VM_WRITE, 4);
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
        VM_READ, 4);
    put(bytes, bootstrap_mapping + DEPLOY_MAPPING_ADDRESS, 0x220000, 8);
    put(bytes, bootstrap_mapping + DEPLOY_MAPPING_SIZE, 4096, 8);

    const std::size_t object = tables[DEPLOY_TABLE_OBJECT].offset;
    put(bytes, object + DEPLOY_OBJECT_OUTPUT_A, keys[7].packed(), 8);
    put(bytes, object + DEPLOY_OBJECT_KIND,
        OBJECT_KIND_NOTIFICATION, 2);
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
        OBJECT_KIND_THREAD, 2);
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
        OBJECT_KIND_THREAD, 2);
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

inline auto pack_io_test(const char* path) -> std::vector<uint8_t> {
    Manifest manifest;
    Task task{manifest, "io-test", path, 4 * 1024 * 1024};
    task.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_IO_SPACE));
    task.authority(BOOT_IO_HOST, "block.host", RIGHT_CONNECT);
    task.finish();
    return manifest.finish();
}

inline auto pack_io_session(const char* server, const char* client) -> std::vector<uint8_t> {
    Manifest manifest;
    constexpr auto rights = RIGHT_SEND | RIGHT_RECEIVE;
    {
        Task task{manifest, "block", server, 4 * 1024 * 1024};
        task.cspace(128, 20);
        task.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_IO_SPACE));
        task.authority(BOOT_IO_HOST, "block.host", RIGHT_CONNECT);
        task.channel(boot::Block, "block.server", 1, 1, rights);
        task.finish();
    }
    {
        Task task{manifest, "io-client", client, 2 * 1024 * 1024};
        task.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_CHANNEL));
        task.channel(boot::Block, "block.client", 0, 1, RIGHT_SEND);
        task.finish();
    }
    return manifest.finish();
}

inline auto pack_file_session(char** paths, bool fault_test = false) -> std::vector<uint8_t> {
    Manifest manifest;
    constexpr auto rights = RIGHT_SEND | RIGHT_RECEIVE;
    {
        Task task{manifest, "block", paths[0], 4 * 1024 * 1024};
        task.cspace(128, 20);
        task.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_IO_SPACE));
        task.authority(BOOT_IO_HOST, "block.host", RIGHT_CONNECT);
        task.channel(boot::Block, "block.server", 1, 1, rights);
        task.finish();
    }
    {
        Task task{manifest, "files", paths[1], 16 * 1024 * 1024};
        task.cspace(1024, 132);
        task.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_PAGER) | OBJ_BIT(OBJECT_KIND_CHANNEL));
        task.channel(boot::Block, "block.client", 0, 1, RIGHT_SEND);
        task.channel(boot::Files, "files.server", 1, 1, RIGHT_RECEIVE);
        task.finish();
    }
    {
        Task task{manifest, "file-client", paths[2], 4 * 1024 * 1024};
        task.cspace(128, 20);
        task.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_CHANNEL));
        task.channel(boot::Files, "files.client", 0, 1, RIGHT_SEND);
        if (fault_test) {
            task.authority(file_fault_test::Ready, "test.ready", RIGHT_SIGNAL);
            task.authority(file_fault_test::Go, "test.go", RIGHT_RECEIVE);
        }
        task.finish();
    }
    return manifest.finish();
}

inline auto pack_channel_test(const char* coordinator, const char* worker,
    const char* provider, const char* holder) -> std::vector<uint8_t> {
    Manifest manifest;
    {
        Task task{manifest, "channel-test", coordinator, 8 * 1024 * 1024, true};
        task.cspace(512, 68);
        task.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_CHANNEL));
        task.finish();
    }
    {
        Task task{manifest, "writer", worker, 1024 * 1024, false, ApplicationBudget};
        task.channel(boot::Stdout, "data", 0, 1, RIGHT_SEND);
        task.channel(boot::Stderr, "ready", 0, 1, RIGHT_SEND);
        task.finish();
    }
    {
        Task task{manifest, "provider", provider, 1024 * 1024};
        task.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_CHANNEL));
        task.channel(boot::Stdout, "handoff", 0, 1,
            RIGHT_SEND | RIGHT_RECEIVE);
        task.channel_service(channel_test::Provider, "provider.client", 1,
            RIGHT_RECEIVE, RIGHT_SEND, 2);
        task.finish();
    }
    {
        Task task{manifest, "export-holder", holder, 1024 * 1024};
        task.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_CHANNEL));
        task.requires_service(2, "provider");
        task.channel(boot::Stdout, "handoff", 0, 1,
            RIGHT_SEND);
        task.channel(channel_test::Provider, "provider.client", 0, 1,
            RIGHT_SEND | RIGHT_DUPLICATE);
        task.finish();
    }
    return manifest.finish();
}

inline auto pack_denied(const char* name, const char* image) -> std::vector<uint8_t> {
    Manifest manifest;
    Task task{manifest, name, image, 1024 * 1024, false, ApplicationBudget};
    task.authority(boot::Stdout, "stdout", RIGHT_SEND);
    task.authority(boot::Stderr, "stderr", RIGHT_SEND);
    task.authority(boot::Stdin, "stdin", RIGHT_RECEIVE);
    task.authority(BOOT_IO_HOST, "block.host", RIGHT_CONNECT);
    task.finish();
    return manifest.finish();
}
}
