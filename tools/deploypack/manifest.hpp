#pragma once

#include <user/lib/imports.hpp>
#include <test/user/channel/export_protocol.hpp>
#include <uapi/channel.h>
#include <test/user/io/file_fault.hpp>

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
    uint64_t caps_{};
    bool supervisor_{};
    uint64_t kinds_{MYOS_RESOURCE_E2_KINDS};
    uint16_t restart_{MYOS_DEPLOY_RESTART_NEVER};
    uint16_t readiness_{MYOS_DEPLOY_READINESS_START};
    uint64_t readiness_timeout_ns_{};
    std::string arguments_{};
    size_t argument_count_{};

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
    void import(BootstrapBinding binding, uint64_t source, uint16_t source_class,
                uint64_t rights, uint16_t mode = MYOS_DEPLOY_IMPORT_DUPLICATE,
                uint64_t side = 0, uint64_t badge = 0) {
        const auto destination = key("cap." + (binding.role != 0
            ? std::to_string(binding.role) : std::string{binding.imported.name}));
        auto r = manifest_.row(MYOS_DEPLOY_TABLE_IMPORT);
        r.u64(MYOS_DEPLOY_IMPORT_SOURCE, source);
        r.u64(MYOS_DEPLOY_IMPORT_DESTINATION, destination);
        r.u16(MYOS_DEPLOY_IMPORT_MODE, mode);
        r.u16(MYOS_DEPLOY_IMPORT_SOURCE_CLASS, source_class);
        constexpr auto a = MYOS_DEPLOY_IMPORT_ATTENUATION;
        r.u16(a + MYOS_DEPLOY_ATTENUATION_VERSION, MYOS_CAP_ATTENUATION_VERSION_CURRENT);
        r.u16(a + MYOS_DEPLOY_ATTENUATION_KIND, binding.kind());
        r.u32(a + MYOS_DEPLOY_ATTENUATION_SIZE, MYOS_CAP_ATTENUATION_SIZE);
        r.u64(a + MYOS_DEPLOY_ATTENUATION_RIGHTS, rights);
        if (mode == MYOS_DEPLOY_IMPORT_CHANNEL_MINT) {
            r.u64(a + MYOS_DEPLOY_ATTENUATION_WORD0, side);
            r.u64(a + MYOS_DEPLOY_ATTENUATION_WORD1, badge);
            r.u64(a + MYOS_DEPLOY_ATTENUATION_WORD2, UINT64_MAX);
        }
        auto b = manifest_.row(MYOS_DEPLOY_TABLE_BOOTSTRAP);
        b.u32(MYOS_DEPLOY_BOOTSTRAP_KIND, binding.role);
        if (binding.role == 0) {
            b.u64(MYOS_DEPLOY_BOOTSTRAP_NAME, manifest_.name(binding.imported.name));
            b.u32(MYOS_DEPLOY_BOOTSTRAP_PROTOCOL, binding.imported.protocol);
            b.u16(MYOS_DEPLOY_BOOTSTRAP_MAJOR, binding.imported.major);
            b.u16(MYOS_DEPLOY_BOOTSTRAP_MINOR, binding.imported.minor);
            b.u16(MYOS_DEPLOY_BOOTSTRAP_OBJECT_KIND, binding.kind());
        }
        b.u64(MYOS_DEPLOY_BOOTSTRAP_DESTINATION, destination);
    }
    void local(BootstrapBinding binding, std::string_view suffix, uint64_t rights) {
        import(binding, key(suffix), MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY, rights);
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
          cspace_pages_(supervisor ? 9 : 4), caps_(supervisor ? 512 : 64), supervisor_(supervisor) {
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
    void authority(BootstrapBinding binding, std::string_view source, uint64_t rights) {
        import(binding, manifest_.name(source), MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY, rights);
    }
    void kinds(uint64_t value) { kinds_ = value; }
    void restart(uint16_t policy) { restart_ = policy; }
    void explicit_readiness(uint64_t timeout_ns) {
        if (timeout_ns == 0) throw std::invalid_argument("readiness needs a deadline");
        readiness_ = MYOS_DEPLOY_READINESS_EXPLICIT;
        readiness_timeout_ns_ = timeout_ns;
        notification("readiness", 1);
        local(MYOS_BOOTSTRAP_CAP_READINESS_NOTIFICATION, "readiness", MYOS_RIGHT_SIGNAL);
    }
    void argument(std::string_view value) {
        if (argument_count_ == MYOS_BOOTSTRAP_ARG_MAX
            || arguments_.size() >= MYOS_BOOTSTRAP_ARG_BYTES
            || value.size() >= MYOS_BOOTSTRAP_ARG_BYTES - arguments_.size()
            || value.find('\0') != std::string_view::npos)
            throw std::invalid_argument("task argument exceeds bootstrap envelope");
        arguments_.append(value);
        arguments_.push_back('\0');
        ++argument_count_;
    }
    void requires_service(uint32_t target, std::string_view relation,
                          uint16_t flags = MYOS_DEPLOY_DEPENDENCY_STARTUP) {
        auto r = manifest_.row(MYOS_DEPLOY_TABLE_DEPENDENCY);
        r.u32(MYOS_DEPLOY_DEPENDENCY_TARGET, target);
        r.u16(MYOS_DEPLOY_DEPENDENCY_KIND, MYOS_DEPLOY_DEPENDENCY_REQUIRED);
        r.u16(MYOS_DEPLOY_DEPENDENCY_FLAGS, flags);
        r.u64(MYOS_DEPLOY_DEPENDENCY_RELATION, manifest_.name(relation));
    }
    void cspace(uint32_t slots, uint32_t pages) {
        cspace_slots_ = slots;
        cspace_pages_ = pages;
        if (caps_ < slots) caps_ = slots;
    }
    void channel(BootstrapBinding binding, std::string_view source, uint64_t side,
                 uint64_t badge, uint64_t rights) {
        import(binding, manifest_.name(source), MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY,
               rights, MYOS_DEPLOY_IMPORT_CHANNEL_MINT, side, badge);
    }
    void channel_service(BootstrapBinding binding, std::string_view label,
                         uint64_t server_side, uint64_t server_rights,
                         uint64_t client_rights, uint64_t depth = 16,
                         uint64_t transfers = 0, uint64_t relations = 2) {
        kinds_ |= MYOS_RESOURCE_CHANNEL;
        auto object = manifest_.row(MYOS_DEPLOY_TABLE_OBJECT);
        const auto first = key(std::string{label} + ".side0");
        const auto second = key(std::string{label} + ".side1");
        object.u64(MYOS_DEPLOY_OBJECT_OUTPUT, first);
        object.u64(MYOS_DEPLOY_OBJECT_OUTPUT_B, second);
        object.u16(MYOS_DEPLOY_OBJECT_KIND, MYOS_OBJECT_KIND_CHANNEL);
        for (auto offset : {MYOS_DEPLOY_OBJECT_REF0, MYOS_DEPLOY_OBJECT_REF1,
                            MYOS_DEPLOY_OBJECT_REF2, MYOS_DEPLOY_OBJECT_REF3})
            object.u32(offset, MYOS_DEPLOY_NO_INDEX);
        object.u64(MYOS_DEPLOY_OBJECT_ARG0, depth);
        object.u64(MYOS_DEPLOY_OBJECT_ARG1, MYOS_CHANNEL_MAX_WORDS);
        object.u64(MYOS_DEPLOY_OBJECT_ARG2, transfers);
        object.u64(MYOS_DEPLOY_OBJECT_ARG3, relations);
        import(binding, server_side == 0 ? first : second, MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY,
               server_rights, MYOS_DEPLOY_IMPORT_CHANNEL_MINT, server_side, 1);
        auto exported = manifest_.row(MYOS_DEPLOY_TABLE_EXPORT);
        exported.u64(MYOS_DEPLOY_EXPORT_SOURCE, server_side == 0 ? second : first);
        exported.u64(MYOS_DEPLOY_EXPORT_KEY, manifest_.name(label));
        exported.u16(MYOS_DEPLOY_EXPORT_CLASS, MYOS_DEPLOY_EXPORT_PREPARED_KEY);
        constexpr auto a = MYOS_DEPLOY_EXPORT_CEILING;
        exported.u16(a + MYOS_DEPLOY_ATTENUATION_VERSION, MYOS_CAP_ATTENUATION_VERSION_CURRENT);
        exported.u16(a + MYOS_DEPLOY_ATTENUATION_KIND, MYOS_OBJECT_KIND_CHANNEL);
        exported.u32(a + MYOS_DEPLOY_ATTENUATION_SIZE, MYOS_CAP_ATTENUATION_SIZE);
        exported.u64(a + MYOS_DEPLOY_ATTENUATION_RIGHTS, client_rights | MYOS_RIGHT_DUPLICATE);
        exported.u64(a + MYOS_DEPLOY_ATTENUATION_WORD0, 1 - server_side);
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
        r.u64(MYOS_DEPLOY_TASK_POOL_CAPS, caps_);
        r.u64(MYOS_DEPLOY_TASK_KIND_MASK, kinds_);
        r.u64(MYOS_DEPLOY_TASK_CRITICAL_BYTES, image_.critical_code_bytes + StackSize + 8192);
        r.u32(MYOS_DEPLOY_TASK_CSPACE_SLOTS, cspace_slots_);
        r.u32(MYOS_DEPLOY_TASK_CSPACE_PAGES, cspace_pages_);
        r.u32(MYOS_DEPLOY_TASK_BOOTSTRAP_MAPPING, bootstrap_);
        r.u16(MYOS_DEPLOY_TASK_READINESS, readiness_);
        r.u64(MYOS_DEPLOY_TASK_READINESS_TIMEOUT_NS, readiness_timeout_ns_);
        r.u16(MYOS_DEPLOY_TASK_TERMINAL, MYOS_DEPLOY_TERMINAL_CLOSE);
        r.u16(MYOS_DEPLOY_TASK_RESTART, restart_);
        r.u64(MYOS_DEPLOY_TASK_ARGUMENTS,
            arguments_.empty() ? 0 : manifest_.name(arguments_));
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
        task.cspace(128, 20);
        task.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_IO_SPACE);
        task.authority(MYOS_BOOTSTRAP_CAP_DEVICE, "block.device", MYOS_RIGHT_CONNECT);
        task.channel(myos::bootstrap::imports::Block, "block.server", 1, 1, rights);
        task.finish();
    }
    {
        Task task{manifest, "io-client", client, 2 * 1024 * 1024};
        task.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_CHANNEL);
        task.channel(myos::bootstrap::imports::Block, "block.client", 0, 1, MYOS_RIGHT_SEND);
        task.finish();
    }
    return manifest.finish();
}

inline auto pack_file_session(char** paths, bool fault_test = false) -> std::vector<uint8_t> {
    Manifest manifest;
    constexpr auto rights = MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE;
    {
        Task task{manifest, "block", paths[0], 4 * 1024 * 1024};
        task.cspace(128, 20);
        task.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_IO_SPACE);
        task.authority(MYOS_BOOTSTRAP_CAP_DEVICE, "block.device", MYOS_RIGHT_CONNECT);
        task.channel(myos::bootstrap::imports::Block, "block.server", 1, 1, rights);
        task.finish();
    }
    {
        Task task{manifest, "files", paths[1], 16 * 1024 * 1024};
        task.cspace(1024, 132);
        task.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_PAGER | MYOS_RESOURCE_CHANNEL);
        task.channel(myos::bootstrap::imports::Block, "block.client", 0, 1, MYOS_RIGHT_SEND);
        task.channel(myos::bootstrap::imports::Files, "files.server", 1, 1, MYOS_RIGHT_RECEIVE);
        task.finish();
    }
    {
        Task task{manifest, "file-client", paths[2], 4 * 1024 * 1024};
        task.cspace(128, 20);
        task.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_CHANNEL);
        task.channel(myos::bootstrap::imports::Files, "files.client", 0, 1, MYOS_RIGHT_SEND);
        if (fault_test) {
            task.authority(file_fault_test::Ready, "test.ready", MYOS_RIGHT_SIGNAL);
            task.authority(file_fault_test::Go, "test.go", MYOS_RIGHT_RECEIVE);
        }
        task.finish();
    }
    return manifest.finish();
}

// Application reservations participate in the same per-hart admission domain
// as native services; this budget leaves room for four concurrent jobs.
inline constexpr uint64_t ApplicationBudget = 250'000;
inline auto pack_application(const char* name, const char* image, uint64_t budget = ApplicationBudget,
                             bool denied = false) -> std::vector<uint8_t> {
    Manifest manifest;
    Task task{manifest, name, image, 1024 * 1024, false, budget};
    task.authority(myos::bootstrap::imports::Stdout, "stdout", MYOS_RIGHT_SEND);
    task.authority(myos::bootstrap::imports::Stderr, "stderr", MYOS_RIGHT_SEND);
    task.authority(myos::bootstrap::imports::Stdin, "stdin", MYOS_RIGHT_RECEIVE);
    if (std::string_view{name} == "cat" || std::string_view{name} == "put"
        || std::string_view{name} == "get" || std::string_view{name} == "fs") {
        task.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_CHANNEL);
        task.cspace(128, 20);
        if (std::string_view{name} == "cat" || std::string_view{name} == "fs")
            task.authority(myos::bootstrap::imports::Files, "files.directory", MYOS_RIGHT_SEND);
        if (std::string_view{name} == "put" || std::string_view{name} == "fs")
            task.authority(myos::bootstrap::imports::Store, "store.directory", MYOS_RIGHT_SEND);
        if (std::string_view{name} == "get")
            task.authority(myos::bootstrap::imports::StoreRead, "store.read.directory", MYOS_RIGHT_SEND);
    }
    if (denied) task.authority(MYOS_BOOTSTRAP_CAP_DEVICE, "block.device", MYOS_RIGHT_CONNECT);
    task.finish();
    return manifest.finish();
}

inline auto pack_channel_test(const char* coordinator, const char* worker,
    const char* provider, const char* holder) -> std::vector<uint8_t> {
    Manifest manifest;
    {
        Task task{manifest, "channel-test", coordinator, 8 * 1024 * 1024, true};
        task.cspace(512, 68);
        task.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_CHANNEL);
        task.finish();
    }
    {
        Task task{manifest, "writer", worker, 1024 * 1024, false, ApplicationBudget};
        task.channel(myos::bootstrap::imports::Stdout, "data", 0, 1, MYOS_RIGHT_SEND);
        task.channel(myos::bootstrap::imports::Stderr, "ready", 0, 1, MYOS_RIGHT_SEND);
        task.finish();
    }
    {
        Task task{manifest, "provider", provider, 1024 * 1024};
        task.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_CHANNEL);
        task.channel(myos::bootstrap::imports::Stdout, "handoff", 0, 1,
            MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE);
        task.channel_service(channel_test::Provider, "provider.client", 1,
            MYOS_RIGHT_RECEIVE, MYOS_RIGHT_SEND, 2);
        task.finish();
    }
    {
        Task task{manifest, "export-holder", holder, 1024 * 1024};
        task.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_CHANNEL);
        task.requires_service(2, "provider");
        task.channel(myos::bootstrap::imports::Stdout, "handoff", 0, 1,
            MYOS_RIGHT_SEND);
        task.channel(channel_test::Provider, "provider.client", 0, 1,
            MYOS_RIGHT_SEND | MYOS_RIGHT_DUPLICATE);
        task.finish();
    }
    return manifest.finish();
}

inline auto pack_console(char** paths, bool fail_shell = false, bool storage = false,
    std::string_view volume_id = {})
    -> std::vector<uint8_t> {
    Manifest manifest;
    // Row identities belong to this manifest, not to init or the kernel.
    constexpr uint32_t uart = 0, process = 1, shell = 2, block = 3, files = 4;
    constexpr uint32_t data_block = 5;
    constexpr auto send = MYOS_RIGHT_SEND;
    constexpr auto receive = MYOS_RIGHT_RECEIVE;
    constexpr uint64_t service_budget = 500'000; // 5% per 10 ms period
    {
        Task t{manifest, "uart", paths[0], 1024 * 1024, false, service_budget};
        t.restart(MYOS_DEPLOY_RESTART_ON_FAULT);
        t.authority(MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY, "uart.memory", MYOS_RIGHT_MAP);
        t.authority(MYOS_BOOTSTRAP_CAP_IRQ, "uart.irq", MYOS_RIGHT_ROUTE | MYOS_RIGHT_OBSERVE | MYOS_RIGHT_ACK);
        t.channel_service(myos::bootstrap::imports::ConsoleOutput, "console.sender", 1, receive, send);
        t.channel_service(myos::bootstrap::imports::ConsoleInput, "input.receiver", 0, send, receive);
        t.finish();
    }
    {
        Task t{manifest, "process_server", paths[1], 32 * 1024 * 1024, true, service_budget};
        t.restart(MYOS_DEPLOY_RESTART_ON_FAULT);
        // The sole Process session owns its jobs. Losing shell closes this
        // session's supervisor and jobs; Files, Block and UART are independent.
        t.requires_service(shell, "session-owner", MYOS_DEPLOY_DEPENDENCY_LIFETIME);
        t.requires_service(uart, "console");
        t.requires_service(files, "files");
        if (storage) t.requires_service(6, "store");
        // Four live task authorities, package mappings and stream endpoints.
        t.cspace(512, 68);
        t.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_CHANNEL | MYOS_RESOURCE_PAGER);
        t.channel(myos::bootstrap::imports::Files, "files.client", 0, 3, send | MYOS_RIGHT_DUPLICATE);
        t.channel(myos::bootstrap::imports::FilesRead, "files.client", 0, 1, send | MYOS_RIGHT_DUPLICATE);
        if (storage) t.channel(myos::bootstrap::imports::Store,
            "store.client", 0, 2, send | MYOS_RIGHT_DUPLICATE);
        if (storage) t.channel(myos::bootstrap::imports::StoreRead,
            "store.client", 0, 1, send | MYOS_RIGHT_DUPLICATE);
        t.channel_service(myos::bootstrap::imports::Process, "process.client", 1, send | receive, send | receive, 16, 0, 3);
        t.channel(myos::bootstrap::imports::ConsoleOutput, "console.sender", 0, 1, send | MYOS_RIGHT_DUPLICATE);
        t.finish();
    }
    {
        Task t{manifest, "shell", paths[2], fail_shell ? uint64_t{128} * 1024 : uint64_t{2} * 1024 * 1024,
            false, service_budget};
        t.restart(MYOS_DEPLOY_RESTART_ON_FAULT);
        t.requires_service(uart, "console");
        t.requires_service(process, "process");
        t.requires_service(files, "files");
        if (storage) t.requires_service(6, "store");
        t.cspace(128, 20);
        t.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_CHANNEL);
        t.channel(myos::bootstrap::imports::Files, "files.client", 0, 1, send);
        if (storage) t.channel(myos::bootstrap::imports::Store,
            "store.client", 0, 3, send);
        t.channel(myos::bootstrap::imports::Process, "process.client", 0, 1, send | receive);
        t.channel(myos::bootstrap::imports::ConsoleOutput, "console.sender", 0, 2, send);
        t.channel(myos::bootstrap::imports::ConsoleInput, "input.receiver", 1, 1, receive);
        t.channel(myos::bootstrap::imports::ServiceControl, "service.control", 1, 1, send | receive);
        t.finish();
    }
    {
        Task t{manifest, "block", paths[3], 4 * 1024 * 1024, false, service_budget};
        t.restart(MYOS_DEPLOY_RESTART_ON_FAULT);
        t.cspace(128, 20);
        t.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_IO_SPACE);
        t.authority(MYOS_BOOTSTRAP_CAP_DEVICE, "pci.0008", MYOS_RIGHT_CONNECT);
        t.channel_service(myos::bootstrap::imports::Block, "block.client", 1, receive, send, 4, 4);
        t.finish();
    }
    {
        Task t{manifest, "files", paths[4], 16 * 1024 * 1024, false, service_budget};
        t.restart(MYOS_DEPLOY_RESTART_ON_FAULT);
        t.requires_service(block, "block");
        t.cspace(1024, 132);
        t.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_PAGER | MYOS_RESOURCE_CHANNEL);
        t.channel(myos::bootstrap::imports::Block, "block.client", 0, 1, send);
        t.channel_service(myos::bootstrap::imports::Files, "files.client", 1, receive, send, 8, 4);
        t.finish();
    }
    if (storage) {
        Task t{manifest, "block_data", paths[5], 4 * 1024 * 1024, false, service_budget};
        t.restart(MYOS_DEPLOY_RESTART_ON_FAULT);
        t.cspace(128, 20);
        t.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_IO_SPACE);
        t.authority(MYOS_BOOTSTRAP_CAP_DEVICE, "pci.0010", MYOS_RIGHT_CONNECT);
        t.channel_service(myos::bootstrap::imports::Block, "block_data.client", 1,
            receive, send, 4, 4);
        t.finish();
    }
    if (storage) {
        Task t{manifest, "store", paths[6], 8 * 1024 * 1024, false, service_budget};
        if (!volume_id.empty()) t.argument(volume_id);
        t.restart(MYOS_DEPLOY_RESTART_ON_FAULT);
        t.explicit_readiness(10'000'000'000);
        t.requires_service(data_block, "data block");
        t.cspace(256, 36);
        t.kinds(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_CHANNEL);
        t.channel(myos::bootstrap::imports::Block, "block_data.client", 0, 1, send);
        t.authority(myos::bootstrap::imports::ServiceWake, "service.wake", MYOS_RIGHT_SIGNAL);
        t.channel_service(myos::bootstrap::imports::Store, "store.client", 1,
            receive, send, 4, 4);
        t.finish();
    }
    return manifest.finish();
}

} // namespace myos::deploy::host
