#pragma once

#include <sys/start.hpp>
#include <servers/vfs/protocol.hpp>
#include <uapi/ipc.h>

#include <array>
#include <map>
#include <sys/channel.hpp>
#include "packer.hpp"

namespace deploy::host {

// Rows own their bytes until final assembly. References are table indices or
// interned names, so adding a service never requires renumbering another row.
class Manifest final {
    static constexpr std::array<uint32_t, DEPLOY_TABLE_COUNT> strides{
        DEPLOY_TASK_STRIDE, DEPLOY_IMAGE_STRIDE,
        DEPLOY_MAPPING_STRIDE, DEPLOY_OBJECT_STRIDE,
        DEPLOY_EXECUTION_STRIDE, DEPLOY_IMPORT_STRIDE,
        DEPLOY_DEPENDENCY_STRIDE, DEPLOY_EXPORT_STRIDE,
        1, DEPLOY_BOOTSTRAP_STRIDE};
    using Bytes = std::vector<uint8_t>;
    std::array<std::vector<Bytes>, DEPLOY_TABLE_COUNT> rows_{};
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
        Bytes bytes(DEPLOY_HEADER_SIZE);
        Table tables[DEPLOY_TABLE_COUNT]{};
        for (unsigned table = 0; table < DEPLOY_TABLE_COUNT; ++table) {
            const auto& rows = rows_[table];
            const bool string = table == DEPLOY_TABLE_STRING;
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
    image_info image_;
    std::array<uint32_t, DEPLOY_TABLE_COUNT> first_{};
    uint32_t bootstrap_{};
    uint64_t budget_{};
    uint32_t cspace_slots_{};
    uint32_t cspace_pages_{};
    uint64_t caps_{};
    bool supervisor_{};
    uint64_t kinds_{DEPLOY_BASE_KINDS};
    uint16_t restart_{DEPLOY_RESTART_NEVER};
    uint16_t readiness_{DEPLOY_READINESS_START};
    uint64_t readiness_timeout_ns_{};
    std::string arguments_{};
    size_t argument_count_{};

    auto key(std::string_view suffix) -> uint64_t {
        return manifest_.name(name_ + "." + std::string{suffix});
    }
    void notification(std::string_view suffix, uint64_t badge) {
        auto r = manifest_.row(DEPLOY_TABLE_OBJECT);
        r.u64(DEPLOY_OBJECT_OUTPUT, key(suffix));
        r.u16(DEPLOY_OBJECT_KIND, OBJECT_KIND_NOTIFICATION);
        for (auto offset : {DEPLOY_OBJECT_REF0, DEPLOY_OBJECT_REF1,
                            DEPLOY_OBJECT_REF2, DEPLOY_OBJECT_REF3})
            r.u32(offset, DEPLOY_NO_INDEX);
        r.u64(DEPLOY_OBJECT_ARG0, badge);
    }
    auto zero(std::string_view suffix, uint64_t address, uint64_t size,
              uint16_t critical, uint32_t access) -> uint32_t {
        const auto index = manifest_.count(DEPLOY_TABLE_MAPPING);
        auto r = manifest_.row(DEPLOY_TABLE_MAPPING);
        r.u64(DEPLOY_MAPPING_PRODUCED, key(suffix));
        r.u32(DEPLOY_MAPPING_IMAGE, DEPLOY_NO_INDEX);
        r.u32(DEPLOY_MAPPING_SEGMENT, DEPLOY_NO_INDEX);
        r.u16(DEPLOY_MAPPING_SOURCE, DEPLOY_MAPPING_SOURCE_ZERO);
        r.u16(DEPLOY_MAPPING_CRITICAL, critical);
        r.u32(DEPLOY_MAPPING_ACCESS, access);
        r.u64(DEPLOY_MAPPING_ADDRESS, address);
        r.u64(DEPLOY_MAPPING_SIZE, size);
        return index;
    }
    void import(BootstrapBinding binding, uint64_t source, uint16_t source_class,
                uint64_t rights, uint16_t mode = DEPLOY_IMPORT_DUPLICATE,
                uint64_t side = 0, uint64_t badge = 0) {
        const auto destination = key("cap." + (binding.role != 0
            ? std::to_string(binding.role) : std::string{binding.imported.name}));
        auto r = manifest_.row(DEPLOY_TABLE_IMPORT);
        r.u64(DEPLOY_IMPORT_SOURCE, source);
        r.u64(DEPLOY_IMPORT_DESTINATION, destination);
        r.u16(DEPLOY_IMPORT_MODE, mode);
        r.u16(DEPLOY_IMPORT_SOURCE_CLASS, source_class);
        constexpr auto a = DEPLOY_IMPORT_ATTENUATION;
        r.u16(a + DEPLOY_ATTENUATION_VERSION, CAP_ATTENUATION_VERSION_CURRENT);
        r.u16(a + DEPLOY_ATTENUATION_KIND, binding.kind());
        r.u32(a + DEPLOY_ATTENUATION_SIZE, CAP_ATTENUATION_SIZE);
        r.u64(a + DEPLOY_ATTENUATION_RIGHTS, rights);
        if (mode == DEPLOY_IMPORT_CHANNEL_MINT) {
            r.u64(a + DEPLOY_ATTENUATION_WORD0, side);
            r.u64(a + DEPLOY_ATTENUATION_WORD1, badge);
            r.u64(a + DEPLOY_ATTENUATION_WORD2, UINT64_MAX);
        }
        auto b = manifest_.row(DEPLOY_TABLE_BOOTSTRAP);
        b.u32(DEPLOY_BOOTSTRAP_KIND, binding.role);
        if (binding.role == 0) {
            b.u64(DEPLOY_BOOTSTRAP_NAME, manifest_.name(binding.imported.name));
            b.u32(DEPLOY_BOOTSTRAP_PROTOCOL, binding.imported.protocol);
            b.u16(DEPLOY_BOOTSTRAP_MAJOR, binding.imported.major);
            b.u16(DEPLOY_BOOTSTRAP_MINOR, binding.imported.minor);
            b.u16(DEPLOY_BOOTSTRAP_OBJECT_KIND, binding.kind());
        }
        b.u64(DEPLOY_BOOTSTRAP_DESTINATION, destination);
    }
    void local(BootstrapBinding binding, std::string_view suffix, uint64_t rights) {
        import(binding, key(suffix), DEPLOY_IMPORT_SOURCE_TASK_KEY, rights);
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
        : manifest_(manifest), name_(name), image_(read_image(elf)),
          budget_(budget), cspace_slots_(supervisor ? 128 : 32),
          cspace_pages_(supervisor ? 9 : 4), caps_(supervisor ? 512 : 64), supervisor_(supervisor) {
        for (unsigned t = 0; t < first_.size(); ++t) first_[t] = manifest_.count(t);
        auto image = manifest_.row(DEPLOY_TABLE_IMAGE);
        image.u64(DEPLOY_IMAGE_SOURCE, manifest_.name(name));
        for (uint32_t segment = 0; segment < image_.segments; ++segment) {
            auto r = manifest_.row(DEPLOY_TABLE_MAPPING);
            r.u64(DEPLOY_MAPPING_PRODUCED, key("segment." + std::to_string(segment)));
            r.u32(DEPLOY_MAPPING_IMAGE, first_[DEPLOY_TABLE_IMAGE]);
            r.u32(DEPLOY_MAPPING_SEGMENT, segment);
            r.u16(DEPLOY_MAPPING_CRITICAL,
                  segment == 0 ? DEPLOY_CRITICAL_CODE : DEPLOY_CRITICAL_NONE);
        }
        const auto stack = zero("stack", Stack, StackSize, DEPLOY_CRITICAL_STACK,
                                VM_READ | VM_WRITE);
        bootstrap_ = zero("bootstrap", Bootstrap, 4096, DEPLOY_CRITICAL_BOOTSTRAP, VM_READ);
        const auto ipc = zero("ipc", Ipc, 4096, DEPLOY_CRITICAL_IPC_HEADER,
                              VM_READ | VM_WRITE);
        notification("terminal", 1);
        notification("events", sys::service::EventsBadge);
        auto e = manifest_.row(DEPLOY_TABLE_EXECUTION);
        e.u64(DEPLOY_EXECUTION_KEY, key("thread"));
        e.u64(DEPLOY_EXECUTION_SC, key("sc"));
        e.u64(DEPLOY_EXECUTION_DOMAIN, manifest_.name("domain"));
        e.u32(DEPLOY_EXECUTION_IMAGE, first_[DEPLOY_TABLE_IMAGE]);
        e.u32(DEPLOY_EXECUTION_STACK, stack);
        e.u32(DEPLOY_EXECUTION_BOOTSTRAP, bootstrap_);
        e.u32(DEPLOY_EXECUTION_IPC, ipc);
        e.u32(DEPLOY_EXECUTION_CONTROL, DEPLOY_NO_INDEX);
        e.u32(DEPLOY_EXECUTION_EVENT, DEPLOY_NO_INDEX);
        e.u64(DEPLOY_EXECUTION_STACK_TOP, Stack + StackSize);
        e.u64(DEPLOY_EXECUTION_SC_BUDGET, execution_budget);
        e.u64(DEPLOY_EXECUTION_SC_PERIOD, 10'000'000);
        e.u32(DEPLOY_EXECUTION_URGENCY, 8);
        e.u32(DEPLOY_EXECUTION_HOME_CPU, DEPLOY_HOME_CPU_ANY);
        local(BOOT_POOL, "pool",
              RIGHT_CREATE | (supervisor ? RIGHT_SPLIT : 0));
        local(BOOT_VSPACE, "vspace",
              RIGHT_DELEGATE | RIGHT_MAP | RIGHT_PROTECT
                  | RIGHT_UNMAP | RIGHT_DESTROY);
        local(BOOT_CSPACE, "cspace", RIGHT_MANAGE);
        local(BOOT_EVENTS, "events",
              RIGHT_SIGNAL | RIGHT_RECEIVE | RIGHT_DUPLICATE);
        if (supervisor) {
            authority(BOOT_DOMAIN, "domain", RIGHT_DUPLICATE | RIGHT_CONTROL);
            authority(BOOT_BUNDLE, "bundle", RIGHT_DUPLICATE | RIGHT_MAP | RIGHT_INSPECT);
        }
    }
    void authority(BootstrapBinding binding, std::string_view source, uint64_t rights) {
        import(binding, manifest_.name(source), DEPLOY_IMPORT_SOURCE_AUTHORITY, rights);
    }
    void kinds(uint64_t value) { kinds_ = value; }
    void restart(uint16_t policy) { restart_ = policy; }
    void explicit_readiness(uint64_t timeout_ns) {
        if (timeout_ns == 0) throw std::invalid_argument("readiness needs a deadline");
        readiness_ = DEPLOY_READINESS_EXPLICIT;
        readiness_timeout_ns_ = timeout_ns;
        notification("readiness", 1);
        local(BOOT_READY, "readiness", RIGHT_SIGNAL);
    }
    void argument(std::string_view value) {
        if (argument_count_ == boot::Args::Max
            || arguments_.size() >= boot::Args::Bytes
            || value.size() >= boot::Args::Bytes - arguments_.size()
            || value.find('\0') != std::string_view::npos)
            throw std::invalid_argument("task argument exceeds bootstrap envelope");
        arguments_.append(value);
        arguments_.push_back('\0');
        ++argument_count_;
    }
    void requires_service(uint32_t target, std::string_view relation,
                          uint16_t flags = DEPLOY_DEPENDENCY_STARTUP) {
        auto r = manifest_.row(DEPLOY_TABLE_DEPENDENCY);
        r.u32(DEPLOY_DEPENDENCY_TARGET, target);
        r.u16(DEPLOY_DEPENDENCY_KIND, DEPLOY_DEPENDENCY_REQUIRED);
        r.u16(DEPLOY_DEPENDENCY_FLAGS, flags);
        r.u64(DEPLOY_DEPENDENCY_RELATION, manifest_.name(relation));
    }
    void cspace(uint32_t slots, uint32_t pages) {
        cspace_slots_ = slots;
        cspace_pages_ = pages;
        if (caps_ < slots) caps_ = slots;
    }
    void channel(BootstrapBinding binding, std::string_view source, uint64_t side,
                 uint64_t badge, uint64_t rights) {
        import(binding, manifest_.name(source), DEPLOY_IMPORT_SOURCE_AUTHORITY,
               rights, DEPLOY_IMPORT_CHANNEL_MINT, side, badge);
    }
    void channel_service(BootstrapBinding binding, std::string_view label,
                         uint64_t server_side, uint64_t server_rights,
                         uint64_t client_rights, uint64_t depth = 16,
                         uint64_t transfers = 0, uint64_t relations = 2) {
        kinds_ |= OBJ_BIT(OBJECT_KIND_CHANNEL);
        auto object = manifest_.row(DEPLOY_TABLE_OBJECT);
        const auto first = key(std::string{label} + ".side0");
        const auto second = key(std::string{label} + ".side1");
        object.u64(DEPLOY_OBJECT_OUTPUT, first);
        object.u64(DEPLOY_OBJECT_OUTPUT_B, second);
        object.u16(DEPLOY_OBJECT_KIND, OBJECT_KIND_CHANNEL);
        for (auto offset : {DEPLOY_OBJECT_REF0, DEPLOY_OBJECT_REF1,
                            DEPLOY_OBJECT_REF2, DEPLOY_OBJECT_REF3})
            object.u32(offset, DEPLOY_NO_INDEX);
        object.u64(DEPLOY_OBJECT_ARG0, depth);
        object.u64(DEPLOY_OBJECT_ARG1, CHANNEL_MAX_WORDS);
        object.u64(DEPLOY_OBJECT_ARG2, transfers);
        object.u64(DEPLOY_OBJECT_ARG3, relations);
        import(binding, server_side == 0 ? first : second, DEPLOY_IMPORT_SOURCE_TASK_KEY,
               server_rights, DEPLOY_IMPORT_CHANNEL_MINT, server_side, 1);
        auto exported = manifest_.row(DEPLOY_TABLE_EXPORT);
        exported.u64(DEPLOY_EXPORT_SOURCE, server_side == 0 ? second : first);
        exported.u64(DEPLOY_EXPORT_KEY, manifest_.name(label));
        exported.u16(DEPLOY_EXPORT_CLASS, DEPLOY_EXPORT_PREPARED_KEY);
        constexpr auto a = DEPLOY_EXPORT_CEILING;
        exported.u16(a + DEPLOY_ATTENUATION_VERSION, CAP_ATTENUATION_VERSION_CURRENT);
        exported.u16(a + DEPLOY_ATTENUATION_KIND, OBJECT_KIND_CHANNEL);
        exported.u32(a + DEPLOY_ATTENUATION_SIZE, CAP_ATTENUATION_SIZE);
        exported.u64(a + DEPLOY_ATTENUATION_RIGHTS, client_rights | RIGHT_DUPLICATE);
        exported.u64(a + DEPLOY_ATTENUATION_WORD0, 1 - server_side);
    }
    void finish() {
        auto r = manifest_.row(DEPLOY_TABLE_TASK);
        r.u64(DEPLOY_TASK_NAME, manifest_.name(name_));
        r.u64(DEPLOY_TASK_POOL, key("pool"));
        r.u64(DEPLOY_TASK_VSPACE, key("vspace"));
        r.u64(DEPLOY_TASK_CSPACE, key("cspace"));
        constexpr unsigned tables[] = {DEPLOY_TABLE_IMAGE, DEPLOY_TABLE_MAPPING,
            DEPLOY_TABLE_OBJECT, DEPLOY_TABLE_EXECUTION, DEPLOY_TABLE_IMPORT,
            DEPLOY_TABLE_DEPENDENCY, DEPLOY_TABLE_EXPORT, DEPLOY_TABLE_BOOTSTRAP};
        constexpr unsigned offsets[] = {DEPLOY_TASK_IMAGE_FIRST, DEPLOY_TASK_MAPPING_FIRST,
            DEPLOY_TASK_OBJECT_FIRST, DEPLOY_TASK_EXECUTION_FIRST, DEPLOY_TASK_IMPORT_FIRST,
            DEPLOY_TASK_DEPENDENCY_FIRST, DEPLOY_TASK_EXPORT_FIRST, DEPLOY_TASK_BOOTSTRAP_FIRST};
        for (unsigned i = 0; i < std::size(tables); ++i) {
            r.u32(offsets[i], first_[tables[i]]);
            r.u32(offsets[i] + 4, manifest_.count(tables[i]) - first_[tables[i]]);
        }
        r.u64(DEPLOY_TASK_POOL_MEMORY, budget_);
        r.u64(DEPLOY_TASK_POOL_CAPS, caps_);
        r.u64(DEPLOY_TASK_KIND_MASK, kinds_);
        r.u64(DEPLOY_TASK_CRITICAL_BYTES, image_.critical_code_bytes + StackSize + 8192);
        r.u32(DEPLOY_TASK_CSPACE_SLOTS, cspace_slots_);
        r.u32(DEPLOY_TASK_CSPACE_PAGES, cspace_pages_);
        r.u32(DEPLOY_TASK_BOOTSTRAP_MAPPING, bootstrap_);
        r.u16(DEPLOY_TASK_READINESS, readiness_);
        r.u64(DEPLOY_TASK_READINESS_TIMEOUT_NS, readiness_timeout_ns_);
        r.u16(DEPLOY_TASK_TERMINAL, DEPLOY_TERMINAL_CLOSE);
        r.u16(DEPLOY_TASK_RESTART, restart_);
        r.u64(DEPLOY_TASK_ARGUMENTS,
            arguments_.empty() ? 0 : manifest_.name(arguments_));
    }
};

// Application reservations participate in the same per-hart admission domain
// as native services; this budget leaves room for four concurrent jobs.
inline constexpr uint64_t ApplicationBudget = 500'000;
enum class access { none, read, write, admin };

inline auto pack_application(const char* name, const char* image,
    access files = access::none, uint64_t budget = ApplicationBudget) -> std::vector<uint8_t> {
    Manifest manifest;
    Task task{manifest, name, image, 1024 * 1024, false, budget};
    task.authority(boot::Stdout, "stdout", RIGHT_SEND);
    task.authority(boot::Stderr, "stderr", RIGHT_SEND);
    task.authority(boot::Stdin, "stdin", RIGHT_RECEIVE);
    if (files != access::none) {
        task.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_CHANNEL));
        task.cspace(128, 20);
        if (files == access::read)
            task.authority(boot::VfsRead, "vfs.read.directory", RIGHT_SEND);
        else if (files == access::write)
            task.authority(boot::Vfs, "vfs.directory", RIGHT_SEND);
        else
            task.authority(boot::StoreAdmin, "store.admin.directory", RIGHT_SEND);
    }
    task.finish();
    return manifest.finish();
}

inline auto pack_console(char** paths, bool storage = false,
    std::string_view volume_id = {}, uint64_t shell_memory = 2 * 1024 * 1024)
    -> std::vector<uint8_t> {
    Manifest manifest;
    // Row identities belong to this manifest, not to init or the kernel.
    constexpr uint32_t uart = 0, process = 1, shell = 2, block = 3, files = 4;
    constexpr uint32_t data_block = 5;
    constexpr auto send = RIGHT_SEND;
    constexpr auto receive = RIGHT_RECEIVE;
    constexpr uint64_t service_budget = 600'000; // 6% per 10 ms period
    {
        Task t{manifest, "uart", paths[0], 1024 * 1024, false, service_budget};
        t.restart(DEPLOY_RESTART_ON_FAULT);
        t.authority(boot::UartMem, "uart.memory", RIGHT_MAP);
        t.authority(boot::UartIrq, "uart.irq", RIGHT_ROUTE | RIGHT_OBSERVE | RIGHT_ACK);
        t.channel_service(boot::ConsoleOutput, "console.sender", 1, receive, send);
        t.channel_service(boot::ConsoleInput, "input.receiver", 0,
            send, receive | RIGHT_DUPLICATE);
        t.finish();
    }
    {
        Task t{manifest, "process_server", paths[1], 32 * 1024 * 1024, true, service_budget};
        t.restart(DEPLOY_RESTART_ON_FAULT);
        // The sole Process session owns its jobs. Losing shell closes this
        // session's supervisor and jobs; Files, Block and UART are independent.
        t.requires_service(shell, "session-owner", DEPLOY_DEPENDENCY_LIFETIME);
        t.requires_service(uart, "console");
        t.requires_service(files, "files");
        t.requires_service(storage ? 7 : 5, "vfs");
        if (storage) t.requires_service(6, "store admin");
        // Four live task authorities, package mappings and stream endpoints.
        t.cspace(512, 68);
        t.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_CHANNEL));
        t.channel(boot::Files, "files.client", 0, 3, send | RIGHT_DUPLICATE);
        t.channel(boot::Vfs, "vfs.client", 0,
            sys::vfs::WriteDirectory, send | RIGHT_DUPLICATE);
        t.channel(boot::VfsRead, "vfs.client", 0,
            sys::vfs::ReadDirectory, send | RIGHT_DUPLICATE);
        if (storage) t.channel(boot::StoreAdmin,
            "store.client", 0, 3, send | RIGHT_DUPLICATE);
        t.channel_service(boot::Process, "process.client", 1,
            send | receive, send | receive, 16, 0, 3);
        t.channel(boot::ConsoleInput, "input.receiver", 1, 1,
            receive | RIGHT_DUPLICATE);
        t.channel(boot::ConsoleOutput, "console.sender", 0, 1, send | RIGHT_DUPLICATE);
        t.finish();
    }
    {
        Task t{manifest, "shell", paths[2], shell_memory,
            false, service_budget};
        t.restart(DEPLOY_RESTART_ON_FAULT);
        t.requires_service(uart, "console");
        t.requires_service(process, "process");
        t.cspace(128, 20);
        t.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_CHANNEL));
        if (storage) t.argument("storage");
        t.channel(boot::Process, "process.client", 0, 1, send | receive);
        t.channel(boot::ConsoleOutput, "console.sender", 0, 2, send);
        t.channel(boot::ConsoleInput, "input.receiver", 1, 1, receive);
        t.channel(boot::ServiceControl, "service.control", 1, 1, send | receive);
        t.finish();
    }
    {
        Task t{manifest, "block", paths[3], 4 * 1024 * 1024, false, service_budget};
        t.restart(DEPLOY_RESTART_ON_FAULT);
        t.cspace(128, 20);
        t.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_IO_SPACE));
        t.authority(BOOT_DEVICE, "pci.0008", RIGHT_CONNECT);
        t.channel_service(boot::Block, "block.client", 1, receive, send, 4, 4);
        t.finish();
    }
    {
        Task t{manifest, "files", paths[4], 16 * 1024 * 1024, false, service_budget};
        t.restart(DEPLOY_RESTART_ON_FAULT);
        t.requires_service(block, "block");
        t.cspace(1024, 132);
        t.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_PAGER) | OBJ_BIT(OBJECT_KIND_CHANNEL));
        t.channel(boot::Block, "block.client", 0, 1, send);
        t.channel_service(boot::Files, "files.client", 1, receive, send, 8, 4);
        t.finish();
    }
    if (storage) {
        Task t{manifest, "block_data", paths[5], 4 * 1024 * 1024, false, service_budget};
        t.restart(DEPLOY_RESTART_ON_FAULT);
        t.cspace(128, 20);
        t.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_IO_SPACE));
        t.authority(BOOT_DEVICE, "pci.0010", RIGHT_CONNECT);
        t.channel_service(boot::Block, "block_data.client", 1,
            receive, send, 4, 4);
        t.finish();
    }
    if (storage) {
        Task t{manifest, "store", paths[6], 8 * 1024 * 1024, false, service_budget};
        if (!volume_id.empty()) t.argument(volume_id);
        t.restart(DEPLOY_RESTART_ON_FAULT);
        t.explicit_readiness(10'000'000'000);
        t.requires_service(data_block, "data block");
        t.cspace(256, 36);
        t.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_CHANNEL));
        t.channel(boot::Block, "block_data.client", 0, 1, send);
        t.authority(boot::ServiceWake, "service.wake", RIGHT_SIGNAL);
        t.channel_service(boot::Store, "store.client", 1,
            receive, send, 4, 4);
        t.finish();
    }
    {
        Task t{manifest, "vfs", paths[storage ? 7 : 5], 4 * 1024 * 1024,
            false, service_budget};
        t.restart(DEPLOY_RESTART_ON_FAULT);
        t.explicit_readiness(10'000'000'000);
        t.requires_service(files, "files");
        if (storage) t.requires_service(6, "store");
        t.cspace(256, 36);
        t.kinds(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_CHANNEL));
        t.channel(boot::Files, "files.client", 0, 1, send);
        if (storage) t.channel(boot::Store, "store.client", 0, 2, send);
        t.authority(boot::ServiceWake, "service.wake", RIGHT_SIGNAL);
        t.channel_service(boot::Vfs, "vfs.client", 1,
            receive, send, 4, 4);
        t.finish();
    }
    return manifest.finish();
}

} // namespace deploy::host
