#pragma once

#include <array>
#include <map>
#include <user/lib/service_protocol.hpp>
#include "packer.hpp"

namespace myos::deploy::host {

// Rows own their bytes until final assembly. References are table indices or
// interned names, so adding a service never requires renumbering another row.
class Manifest final {
    static constexpr std::array<uint32_t, MYOS_DEPLOY_TABLE_COUNT> strides{
        MYOS_DEPLOY_TASK_STRIDE, MYOS_DEPLOY_IMAGE_STRIDE,
        MYOS_DEPLOY_MAPPING_STRIDE, MYOS_DEPLOY_OBJECT_STRIDE,
        MYOS_DEPLOY_EXECUTION_STRIDE, MYOS_DEPLOY_IMPORT_STRIDE,
        MYOS_DEPLOY_DEPENDENCY_STRIDE, MYOS_DEPLOY_EXPORT_STRIDE,
        1, MYOS_DEPLOY_BOOTSTRAP_STRIDE};
    using Bytes = std::vector<uint8_t>;
    std::array<std::vector<Bytes>, MYOS_DEPLOY_TABLE_COUNT> rows_{};
    std::map<std::string, uint64_t> names_{};
    Bytes strings_{};

public:
    struct Row final {
        Bytes& bytes;
        void u16(size_t offset, uint64_t value) { put(bytes, offset, value, 2); }
        void u32(size_t offset, uint64_t value) { put(bytes, offset, value, 4); }
        void u64(size_t offset, uint64_t value) { put(bytes, offset, value, 8); }
    };
    auto count(unsigned table) const -> uint32_t { return rows_[table].size(); }
    auto row(unsigned table) -> Row {
        return Row{rows_[table].emplace_back(strides[table], 0)};
    }
    auto name(std::string_view text) -> uint64_t {
        auto [entry, inserted] = names_.try_emplace(std::string{text});
        if (inserted) {
            entry->second = strings_.size() | (uint64_t{text.size()} << 32);
            strings_.insert(strings_.end(), text.begin(), text.end());
        }
        return entry->second;
    }
    auto finish() -> Bytes {
        Bytes bytes(MYOS_DEPLOY_HEADER_SIZE);
        Table tables[MYOS_DEPLOY_TABLE_COUNT]{};
        for (unsigned table = 0; table < MYOS_DEPLOY_TABLE_COUNT; ++table) {
            const auto& rows = rows_[table];
            const bool string = table == MYOS_DEPLOY_TABLE_STRING;
            tables[table] = append_table(bytes,
                string ? strings_.size() : rows.size(), strides[table]);
            auto destination = bytes.begin() + tables[table].offset;
            if (string) {
                std::copy(strings_.begin(), strings_.end(), destination);
            } else {
                for (const auto& record : rows) {
                    destination = std::copy(record.begin(), record.end(), destination);
                }
            }
        }
        finalize(bytes, tables);
        return bytes;
    }
};

class Task final {
    Manifest& manifest_;
    std::string name_;
    ProductionImageMetrics image_;
    std::array<uint32_t, MYOS_DEPLOY_TABLE_COUNT> first_{};
    uint32_t bootstrap_{};
    uint64_t budget_{};
    uint32_t cspace_slots_{};
    uint32_t cspace_pages_{};
    bool supervisor_{};
    uint64_t kinds_{MYOS_RESOURCE_E2_KINDS};

    auto key(std::string_view suffix) -> uint64_t {
        return manifest_.name(name_ + "." + std::string{suffix});
    }
    void notification(std::string_view suffix, uint64_t badge) {
        auto r = manifest_.row(MYOS_DEPLOY_TABLE_OBJECT);
        r.u64(MYOS_DEPLOY_OBJECT_OUTPUT, key(suffix));
        r.u16(MYOS_DEPLOY_OBJECT_KIND, MYOS_OBJECT_KIND_NOTIFICATION);
        for (auto offset : {MYOS_DEPLOY_OBJECT_REF0, MYOS_DEPLOY_OBJECT_REF1,
                            MYOS_DEPLOY_OBJECT_REF2, MYOS_DEPLOY_OBJECT_REF3})
            r.u32(offset, MYOS_DEPLOY_NO_INDEX);
        r.u64(MYOS_DEPLOY_OBJECT_ARG0, badge);
    }
    auto zero(std::string_view suffix, uint64_t address, uint64_t size,
              uint16_t critical, uint32_t access) -> uint32_t {
        const auto index = manifest_.count(MYOS_DEPLOY_TABLE_MAPPING);
        auto r = manifest_.row(MYOS_DEPLOY_TABLE_MAPPING);
        r.u64(MYOS_DEPLOY_MAPPING_PRODUCED, key(suffix));
        r.u32(MYOS_DEPLOY_MAPPING_IMAGE, MYOS_DEPLOY_NO_INDEX);
        r.u32(MYOS_DEPLOY_MAPPING_SEGMENT, MYOS_DEPLOY_NO_INDEX);
        r.u16(MYOS_DEPLOY_MAPPING_SOURCE, MYOS_DEPLOY_MAPPING_SOURCE_ZERO);
        r.u16(MYOS_DEPLOY_MAPPING_CRITICAL, critical);
        r.u32(MYOS_DEPLOY_MAPPING_ACCESS, access);
        r.u64(MYOS_DEPLOY_MAPPING_ADDRESS, address);
        r.u64(MYOS_DEPLOY_MAPPING_SIZE, size);
        return index;
    }
    void import(uint32_t role, uint64_t source, uint16_t source_class,
                uint64_t rights, uint16_t mode = MYOS_DEPLOY_IMPORT_DUPLICATE,
                uint64_t side = 0, uint64_t badge = 0) {
        const auto destination = key("cap." + std::to_string(role));
        auto r = manifest_.row(MYOS_DEPLOY_TABLE_IMPORT);
        r.u64(MYOS_DEPLOY_IMPORT_SOURCE, source);
        r.u64(MYOS_DEPLOY_IMPORT_DESTINATION, destination);
        r.u16(MYOS_DEPLOY_IMPORT_MODE, mode);
        r.u16(MYOS_DEPLOY_IMPORT_SOURCE_CLASS, source_class);
        constexpr auto a = MYOS_DEPLOY_IMPORT_ATTENUATION;
        r.u16(a + MYOS_DEPLOY_ATTENUATION_VERSION, MYOS_CAP_ATTENUATION_VERSION_CURRENT);
        r.u16(a + MYOS_DEPLOY_ATTENUATION_KIND, myos_bootstrap_object_kind(role));
        r.u32(a + MYOS_DEPLOY_ATTENUATION_SIZE, MYOS_CAP_ATTENUATION_SIZE);
        r.u64(a + MYOS_DEPLOY_ATTENUATION_RIGHTS, rights);
        if (mode == MYOS_DEPLOY_IMPORT_CHANNEL_MINT) {
            r.u64(a + MYOS_DEPLOY_ATTENUATION_WORD0, side);
            r.u64(a + MYOS_DEPLOY_ATTENUATION_WORD1, badge);
            r.u64(a + MYOS_DEPLOY_ATTENUATION_WORD2, UINT64_MAX);
        }
        auto b = manifest_.row(MYOS_DEPLOY_TABLE_BOOTSTRAP);
        b.u32(MYOS_DEPLOY_BOOTSTRAP_KIND, role);
        b.u64(MYOS_DEPLOY_BOOTSTRAP_DESTINATION, destination);
    }
    void local(uint32_t role, std::string_view suffix, uint64_t rights) {
        import(role, key(suffix), MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY, rights);
    }

public:
    // Every native service has the same stack/bootstrap/IPC layout in its own
    // VSpace. Code extents still come from the actual ELF.
    static constexpr uint64_t Stack = 0x40010000;
    static constexpr uint64_t StackSize = 0x10000;
    static constexpr uint64_t Bootstrap = 0x40020000;
    static constexpr uint64_t Ipc = 0x40000000;

    Task(Manifest& manifest, std::string_view name, std::string_view elf,
         uint64_t budget, bool supervisor = false,
         uint64_t execution_budget = 1'000'000)
        : manifest_(manifest), name_(name), image_(production_image_metrics(elf)),
          budget_(budget), cspace_slots_(supervisor ? 128 : 32),
          cspace_pages_(supervisor ? 9 : 4), supervisor_(supervisor) {
        for (unsigned t = 0; t < first_.size(); ++t) first_[t] = manifest_.count(t);
        auto image = manifest_.row(MYOS_DEPLOY_TABLE_IMAGE);
        image.u64(MYOS_DEPLOY_IMAGE_SOURCE, manifest_.name(name));
        for (uint32_t segment = 0; segment < image_.segments; ++segment) {
            auto r = manifest_.row(MYOS_DEPLOY_TABLE_MAPPING);
            r.u64(MYOS_DEPLOY_MAPPING_PRODUCED, key("segment." + std::to_string(segment)));
            r.u32(MYOS_DEPLOY_MAPPING_IMAGE, first_[MYOS_DEPLOY_TABLE_IMAGE]);
            r.u32(MYOS_DEPLOY_MAPPING_SEGMENT, segment);
            r.u16(MYOS_DEPLOY_MAPPING_CRITICAL,
                  segment == 0 ? MYOS_DEPLOY_CRITICAL_CODE : MYOS_DEPLOY_CRITICAL_NONE);
        }
        const auto stack = zero("stack", Stack, StackSize, MYOS_DEPLOY_CRITICAL_STACK,
                                MYOS_VM_READ | MYOS_VM_WRITE);
        bootstrap_ = zero("bootstrap", Bootstrap, 4096, MYOS_DEPLOY_CRITICAL_BOOTSTRAP, MYOS_VM_READ);
        const auto ipc = zero("ipc", Ipc, 4096, MYOS_DEPLOY_CRITICAL_IPC_HEADER,
                              MYOS_VM_READ | MYOS_VM_WRITE);
        notification("terminal", 1);
        notification("events", service::EventsBadge);
        auto e = manifest_.row(MYOS_DEPLOY_TABLE_EXECUTION);
        e.u64(MYOS_DEPLOY_EXECUTION_KEY, key("thread"));
        e.u64(MYOS_DEPLOY_EXECUTION_SC, key("sc"));
        e.u64(MYOS_DEPLOY_EXECUTION_DOMAIN, manifest_.name("domain"));
        e.u32(MYOS_DEPLOY_EXECUTION_IMAGE, first_[MYOS_DEPLOY_TABLE_IMAGE]);
        e.u32(MYOS_DEPLOY_EXECUTION_STACK, stack);
        e.u32(MYOS_DEPLOY_EXECUTION_BOOTSTRAP, bootstrap_);
        e.u32(MYOS_DEPLOY_EXECUTION_IPC, ipc);
        e.u32(MYOS_DEPLOY_EXECUTION_CONTROL, MYOS_DEPLOY_NO_INDEX);
        e.u32(MYOS_DEPLOY_EXECUTION_EVENT, MYOS_DEPLOY_NO_INDEX);
        e.u64(MYOS_DEPLOY_EXECUTION_STACK_TOP, Stack + StackSize);
        e.u64(MYOS_DEPLOY_EXECUTION_SC_BUDGET, execution_budget);
        e.u64(MYOS_DEPLOY_EXECUTION_SC_PERIOD, 10'000'000);
        e.u32(MYOS_DEPLOY_EXECUTION_URGENCY, 8);
        e.u32(MYOS_DEPLOY_EXECUTION_HOME_CPU, MYOS_DEPLOY_HOME_CPU_ANY);
        local(MYOS_BOOTSTRAP_CAP_RESOURCE_POOL, "pool",
              MYOS_RIGHT_CREATE | (supervisor ? MYOS_RIGHT_SPLIT : 0));
        local(MYOS_BOOTSTRAP_CAP_VSPACE, "vspace",
              MYOS_RIGHT_CREATE_REGION | MYOS_RIGHT_MAP | MYOS_RIGHT_PROTECT
                  | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY);
        local(MYOS_BOOTSTRAP_CAP_CSPACE, "cspace", MYOS_RIGHT_MANAGE);
        local(MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION, "events",
              MYOS_RIGHT_SIGNAL | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_DUPLICATE);
        if (supervisor) {
            authority(MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN, "domain", MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_CONTROL);
            authority(MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE, "bundle", MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_MAP | MYOS_RIGHT_INSPECT);
        }
    }
    void authority(uint32_t role, std::string_view source, uint64_t rights) {
        import(role, manifest_.name(source), MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY, rights);
    }
    void kinds(uint64_t value) { kinds_ = value; }
    void cspace(uint32_t slots, uint32_t pages) { cspace_slots_ = slots; cspace_pages_ = pages; }
    void channel(uint32_t role, std::string_view source, uint64_t side,
                 uint64_t badge, uint64_t rights) {
        import(role, manifest_.name(source), MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY,
               rights, MYOS_DEPLOY_IMPORT_CHANNEL_MINT, side, badge);
    }
    void finish() {
        auto r = manifest_.row(MYOS_DEPLOY_TABLE_TASK);
        r.u64(MYOS_DEPLOY_TASK_NAME, manifest_.name(name_));
        r.u64(MYOS_DEPLOY_TASK_POOL, key("pool"));
        r.u64(MYOS_DEPLOY_TASK_VSPACE, key("vspace"));
        r.u64(MYOS_DEPLOY_TASK_CSPACE, key("cspace"));
        constexpr unsigned tables[] = {MYOS_DEPLOY_TABLE_IMAGE, MYOS_DEPLOY_TABLE_MAPPING,
            MYOS_DEPLOY_TABLE_OBJECT, MYOS_DEPLOY_TABLE_EXECUTION, MYOS_DEPLOY_TABLE_IMPORT,
            MYOS_DEPLOY_TABLE_DEPENDENCY, MYOS_DEPLOY_TABLE_EXPORT, MYOS_DEPLOY_TABLE_BOOTSTRAP};
        constexpr unsigned offsets[] = {MYOS_DEPLOY_TASK_IMAGE_FIRST, MYOS_DEPLOY_TASK_MAPPING_FIRST,
            MYOS_DEPLOY_TASK_OBJECT_FIRST, MYOS_DEPLOY_TASK_EXECUTION_FIRST, MYOS_DEPLOY_TASK_IMPORT_FIRST,
            MYOS_DEPLOY_TASK_DEPENDENCY_FIRST, MYOS_DEPLOY_TASK_EXPORT_FIRST, MYOS_DEPLOY_TASK_BOOTSTRAP_FIRST};
        for (unsigned i = 0; i < std::size(tables); ++i) {
            r.u32(offsets[i], first_[tables[i]]);
            r.u32(offsets[i] + 4, manifest_.count(tables[i]) - first_[tables[i]]);
        }
        r.u64(MYOS_DEPLOY_TASK_POOL_MEMORY, budget_);
        r.u64(MYOS_DEPLOY_TASK_POOL_CAPS, supervisor_ ? 512 : 64);
        r.u64(MYOS_DEPLOY_TASK_KIND_MASK, kinds_);
        r.u64(MYOS_DEPLOY_TASK_CRITICAL_BYTES, image_.critical_code_bytes + StackSize + 8192);
        r.u32(MYOS_DEPLOY_TASK_CSPACE_SLOTS, cspace_slots_);
        r.u32(MYOS_DEPLOY_TASK_CSPACE_PAGES, cspace_pages_);
        r.u32(MYOS_DEPLOY_TASK_BOOTSTRAP_MAPPING, bootstrap_);
        r.u16(MYOS_DEPLOY_TASK_READINESS, MYOS_DEPLOY_READINESS_START);
        r.u16(MYOS_DEPLOY_TASK_TERMINAL, MYOS_DEPLOY_TERMINAL_CLOSE);
    }
};

inline auto pack_io_test(const char* path) -> std::vector<uint8_t> {
    Manifest manifest;
    Task task{manifest, "io-test", path, 4 * 1024 * 1024};
    task.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_IO_SPACE);
    task.authority(MYOS_BOOTSTRAP_CAP_DEVICE, "block.device", MYOS_RIGHT_CONNECT);
    task.finish();
    return manifest.finish();
}

inline auto pack_io_session(const char* server, const char* client) -> std::vector<uint8_t> {
    Manifest manifest;
    constexpr auto rights = MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE;
    {
        Task task{manifest, "block", server, 4 * 1024 * 1024};
        task.cspace(32, 6);
        task.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_IO_SPACE);
        task.authority(MYOS_BOOTSTRAP_CAP_DEVICE, "block.device", MYOS_RIGHT_CONNECT);
        task.channel(MYOS_BOOTSTRAP_CAP_SERVICE_CHANNEL, "block.server", 1, 1, rights);
        task.finish();
    }
    {
        Task task{manifest, "io-client", client, 2 * 1024 * 1024};
        task.channel(MYOS_BOOTSTRAP_CAP_SERVICE_CHANNEL, "block.client", 0, 1, rights);
        task.finish();
    }
    return manifest.finish();
}

inline auto pack_file_session(char** paths) -> std::vector<uint8_t> {
    Manifest manifest;
    constexpr auto rights = MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE;
    {
        Task task{manifest, "block", paths[0], 4 * 1024 * 1024};
        task.cspace(32, 6);
        task.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_IO_SPACE);
        task.authority(MYOS_BOOTSTRAP_CAP_DEVICE, "block.device", MYOS_RIGHT_CONNECT);
        task.channel(MYOS_BOOTSTRAP_CAP_SERVICE_CHANNEL, "block.server", 1, 1, rights);
        task.finish();
    }
    {
        Task task{manifest, "files", paths[1], 16 * 1024 * 1024};
        task.cspace(64, 10);
        task.channel(MYOS_BOOTSTRAP_CAP_BLOCK_CHANNEL, "block.client", 0, 1, rights);
        task.channel(MYOS_BOOTSTRAP_CAP_SERVICE_CHANNEL, "files.first.server", 1, 1, rights);
        task.channel(MYOS_BOOTSTRAP_CAP_FILE_CHANNEL, "files.second.server", 1, 1, rights);
        task.finish();
    }
    {
        Task task{manifest, "file-client", paths[2], 4 * 1024 * 1024};
        task.cspace(32, 6);
        task.channel(MYOS_BOOTSTRAP_CAP_SERVICE_CHANNEL, "files.first.client", 0, 1, rights);
        task.channel(MYOS_BOOTSTRAP_CAP_FILE_CHANNEL, "files.second.client", 0, 1, rights);
        task.finish();
    }
    return manifest.finish();
}

inline auto pack_application(const char* image, uint64_t budget = 1'000'000) -> std::vector<uint8_t> {
    Manifest manifest;
    Task task{manifest, "hello", image, 1024 * 1024, false, budget};
    task.authority(MYOS_BOOTSTRAP_CAP_CONSOLE_OUTPUT, "console.sender", MYOS_RIGHT_SEND);
    task.finish();
    return manifest.finish();
}

inline auto pack_console(char** paths)
    -> std::vector<uint8_t> {
    Manifest manifest;
    constexpr auto send = MYOS_RIGHT_SEND;
    constexpr auto receive = MYOS_RIGHT_RECEIVE;
    {
        Task t{manifest, "uart", paths[0], 1024 * 1024};
        t.authority(MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY, "uart.memory", MYOS_RIGHT_MAP);
        t.authority(MYOS_BOOTSTRAP_CAP_IRQ, "uart.irq", MYOS_RIGHT_ROUTE | MYOS_RIGHT_OBSERVE | MYOS_RIGHT_ACK);
        t.channel(MYOS_BOOTSTRAP_CAP_CONSOLE_OUTPUT, "console.receiver", 1, 1, receive);
        t.channel(MYOS_BOOTSTRAP_CAP_CONSOLE_INPUT, "input.sender", 0, 1, send);
        t.finish();
    }
    {
        Task t{manifest, "process_server", paths[1], 32 * 1024 * 1024, true};
        t.channel(MYOS_BOOTSTRAP_CAP_FILE_CHANNEL, "files.process.client", 0, 1, send | receive);
        t.channel(MYOS_BOOTSTRAP_CAP_SERVICE_CHANNEL, "process.server", 1, 1, send | receive);
        t.channel(MYOS_BOOTSTRAP_CAP_CONSOLE_OUTPUT, "console.sender", 0, 1, send | MYOS_RIGHT_DUPLICATE);
        t.finish();
    }
    {
        Task t{manifest, "shell", paths[2], 2 * 1024 * 1024};
        t.cspace(32, 6);
        t.channel(MYOS_BOOTSTRAP_CAP_FILE_CHANNEL, "files.shell.client", 0, 1, send | receive);
        t.channel(MYOS_BOOTSTRAP_CAP_SERVICE_CHANNEL, "process.client", 0, 1, send | receive);
        t.channel(MYOS_BOOTSTRAP_CAP_CONSOLE_OUTPUT, "console.sender", 0, 2, send);
        t.channel(MYOS_BOOTSTRAP_CAP_CONSOLE_INPUT, "input.receiver", 1, 1, receive);
        t.finish();
    }
    {
        Task t{manifest, "block", paths[3], 4 * 1024 * 1024};
        t.cspace(32, 6);
        t.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_IO_SPACE);
        t.authority(MYOS_BOOTSTRAP_CAP_DEVICE, "block.device", MYOS_RIGHT_CONNECT);
        t.channel(MYOS_BOOTSTRAP_CAP_SERVICE_CHANNEL, "block.server", 1, 1, send | receive);
        t.finish();
    }
    {
        Task t{manifest, "files", paths[4], 16 * 1024 * 1024};
        t.cspace(64, 10);
        t.channel(MYOS_BOOTSTRAP_CAP_BLOCK_CHANNEL, "block.client", 0, 1, send | receive);
        t.channel(MYOS_BOOTSTRAP_CAP_SERVICE_CHANNEL, "files.shell.server", 1, 1, send | receive);
        t.channel(MYOS_BOOTSTRAP_CAP_FILE_CHANNEL, "files.process.server", 1, 1, send | receive);
        t.finish();
    }
    return manifest.finish();
}

} // namespace myos::deploy::host
