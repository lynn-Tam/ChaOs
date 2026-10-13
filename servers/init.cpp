#include <protocols/file.hpp>
#include <protocols/block.hpp>
#include <servers/virtio_blk.hpp>
#include <protocols/stream.hpp>
#include <protocols/service.hpp>
#include <servers/pci.hpp>
#include <exec.hpp>
#include <libk/cpio.hpp>
#include <servers/uart.hpp>
#include <algorithm>
#include <deque>
#include <memory>
#include <vector>

struct sv_svc;
struct sv_export {
    sv_svc* provider;
    rt_req contract;
    word_t depth{4}, transfers{4}, relations{2}, side{1}, rights{RIGHT_RECEIVE};
    ch_pair channel;
};
struct sv_import {
    sv_export* source;
    rt_req contract;
    word_t side{}, badge{1}, rights{RIGHT_SEND};
};
struct sv_svc {
    std::string_view name, image;
    pr_quota quota;
    std::vector<std::string_view> args;
    std::vector<pr_cap> grants;
    std::deque<sv_export> exports;
    std::vector<sv_import> imports;
    bool ready{}, recovering{true};
    std::unique_ptr<pr_task> task;
    cap_owner ready_event, memory_pool;
    auto publish(rt_req contract, word_t depth = 4, word_t transfers = 4,
                 word_t side = 1, word_t rights = RIGHT_RECEIVE) -> sv_export& {
        return exports.emplace_back(this, contract, depth, transfers, 2, side, rights);
    }
    void use(sv_export& source, rt_req contract, word_t badge = 1,
             word_t rights = RIGHT_SEND, word_t side = 0) {
        imports.push_back({&source, contract, side, badge, rights});
    }
    auto runnable() const -> bool {
        return std::ranges::all_of(imports, [](const auto& i) {
            return i.source->provider->task && !i.source->provider->recovering;
        });
    }
};
static std::deque<sv_svc> services;
static vm_area archive_mem;
static libk::cpio_view archive;

static void report(const rt_env& info, const fw_view& fw, status_t status = STATUS_OK,
                   std::string_view name = {}) {
    constexpr auto address = 0x30010000;
    const auto region = sys::vm_slice(info.vspace, address, 4096, VM_READ | VM_WRITE,
                                      RIGHT_MAP | RIGHT_UNMAP | RIGHT_DESTROY);
    if (region.status != STATUS_OK) sys::exit(region.status);
    if (const auto rc =
            sys::vm_map(region.value, fw.console(ObjMem), address, 4096, 0, VM_READ | VM_WRITE)
                .status;
        rc != STATUS_OK)
        sys::exit(rc);
    uart_port port{address};
    if (status == STATUS_OK) {
        port.reset();
        port.write("init: native services\n");
    } else {
        (void) libk::fmt::format_to<"init: service startup failed: {} task=">(port, status);
        port.write(name.data(), name.size());
        port.write("\n");
    }
    if (const auto rc = sys::vm_unmap(region.value, address, 4096).status; rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = sys::vm_clear(region.value).status; rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = sys::cap_close(region.value).status; rc != STATUS_OK)
        sys::exit(rc);
}
static auto sv_cap(rt_req contract, cap_t source, word_t rights, word_t side = 0, word_t badge = 1)
    -> pr_cap {
    return {.contract = contract,
            .cap = source,
            .limit = CapView{.version = CAP_ATTENUATION_VERSION_CURRENT,
                             .kind = contract.kind,
                             .size = CAP_ATTENUATION_SIZE,
                             .rights = rights,
                             .words = {side}},
            .badge = badge};
}
static auto sv_fixed(rt_req contract, cap_t source, word_t rights, word_t first = 0, word_t count = 0,
                  word_t perms = 0) -> pr_cap {
    return {.contract = contract,
            .cap = source,
            .limit = CapView{.version = CAP_ATTENUATION_VERSION_CURRENT,
                             .kind = contract.kind,
                             .size = CAP_ATTENUATION_SIZE,
                             .rights = rights,
                             .words = {first, count, perms}}};
}
static auto start(sv_svc& service, const rt_env& info, cap_t events) -> status_t {
    auto caps = service.grants;
    for (auto& output : service.exports) {
        const auto status = output.channel.open(info.pool, output.depth,
                                                output.transfers, output.relations);
        if (status != STATUS_OK) return status;
        caps.push_back(sv_cap(output.contract, output.channel.ends[output.side].selector(),
                              output.rights, output.side));
    }
    for (const auto& input : service.imports)
        caps.push_back(sv_cap(input.contract, input.source->channel.ends[input.side].selector(),
                             input.rights, input.side, input.badge));
    if (service.ready) {
        const auto created = sys::notification_create(info.pool, 1);
        if (created.status != STATUS_OK) return created.status;
        service.ready_event = cap_owner{created.value};
        caps.push_back(sv_fixed(sv_ready, created.value, RIGHT_SIGNAL));
        caps.push_back(sv_cap(sv_wake, events, RIGHT_SIGNAL, 0, 0));
    }
    service.task = std::make_unique<pr_task>();
    const auto mod = archive.find(service.image);
    if (!mod) return STATUS_NOT_FOUND;
    const auto status = pr_exec(*service.task,
        {info.pool, fw_root(info).domain, info.vspace, static_cast<uint32_t>(info.cpus)}, pr_image{.bytes=mod->data},
        {.argv = service.args, .caps = caps, .resources = service.quota,
         .sched = {.budget = 600'000}, .events = events});
    if (status != STATUS_OK || !service.ready) return status;
    const auto deadline = rt_after_ms(10'000);
    if (!deadline) return STATUS_INTERNAL;
    for (;;) {
        const auto ended = service.task->observe();
        if (ended.status != STATUS_OK) return ended.status;
        if (ended.value)
            return ended.value2 ? static_cast<status_t>(ended.value2) : STATUS_PEER_FAULT;
        const auto ready = sys::notification_take(service.ready_event.selector());
        if (ready.status == STATUS_OK) return STATUS_OK;
        if (ready.status != STATUS_RETRY) return ready.status;
        const auto wake = sys::notification_wait(events, *deadline);
        if (wake.status != STATUS_OK) return wake.status;
    }
}
// Imports are the only dependency graph, including recovery and start ordering.
static void recover(sv_svc& failed) {
    failed.recovering = true;
    for (auto& service : services)
        if (!service.recovering && std::ranges::any_of(service.imports, [&](const auto& i) {
                return i.source->provider == &failed;
            })) recover(service);
}
int main() {
    const auto& info = rt_startup();

    auto firmware = fw_view::open(info, 0);
    if (!firmware) sys::exit(firmware.error());
    report(info, *firmware);
    std::optional<pci_bus> pci;
    const auto bytes = fw_root(info).bundle_bytes;
    if (const auto rc = archive_mem.open(info.vspace, 0, pr_window::round_size(bytes), VM_READ); rc != STATUS_OK) sys::exit(rc);
    if (const auto rc = archive_mem.attach(fw_root(info).bundle, 0, archive_mem.size, VM_READ); rc != STATUS_OK) sys::exit(rc);
    archive = libk::cpio_view::parse({reinterpret_cast<const std::byte*>(archive_mem.address), bytes});
    if (!archive) sys::exit(STATUS_BAD_ARGS);
    const auto notification = sys::notification_create(info.pool, 1);
    if (notification.status != STATUS_OK) sys::exit(notification.status);
    cap_owner events{notification.value};
    ch_pair control;
    if (const auto rc = control.open(info.pool, 4, 1, 1); rc != STATUS_OK)
        sys::exit(rc);
    const auto receiver = sys::channel_mint(control.ends[0].selector(), info.cspace,
                                           1, RIGHT_RECEIVE | RIGHT_SEND);
    if (receiver.status != STATUS_OK) sys::exit(receiver.status);
    cap_owner control_rx{receiver.value};
    ch_port channel{control_rx.selector(), events.selector()};

    // Explicit root deployment policy; no service names enter lifecycle code.
    const auto add = [](std::string_view name, std::string_view image, pr_quota quota)
            -> sv_svc& {
        if (!archive.find(image)) sys::exit(STATUS_NOT_FOUND);
        auto& service = services.emplace_back(name, image, quota);
        service.args.push_back(name);
        return service;
    };
    // Shared storage is a sibling of each execution, and survives its restart.
    const auto share = [&](sv_svc& service, word_t bytes) {
        const auto pool = sys::resource_create_child(info.pool, bytes, 0, OBJ_BIT(ObjMem));
        if (pool.status != STATUS_OK) sys::exit(pool.status);
        service.memory_pool = cap_owner{pool.value};
        service.grants.push_back({.contract=io_memory, .cap=pool.value, .limit=word_t{RIGHT_CREATE}});
    };
    auto& uart = add("uart", "uart", {.caps = 128,
        .kinds = pr_base_kinds | OBJ_BIT(ObjChannel)});
    auto memory = sv_cap(uart_mem, firmware->console(ObjMem), RIGHT_MAP, 0, 0);
    std::get<CapView>(memory.limit).words[1] = 1;
    std::get<CapView>(memory.limit).words[2] = VM_READ | VM_WRITE;
    uart.grants = {memory, sv_cap(uart_irq, firmware->console(ObjIrq),
                                 RIGHT_ROUTE | RIGHT_OBSERVE | RIGHT_ACK, 0, 0)};
    auto& output = uart.publish(con_output, 16, 0);
    auto& input = uart.publish(con_input, 16, 0, 0, RIGHT_SEND);

    // Image identity is independent of an instance and its device grant.
    std::deque<pci_binding> devices;
    const auto block = [&](std::string_view name, size_t index) -> sv_export& {
        if (!pci) {
            auto bus=pci_bus::open(info,0x31000000);
            if(!bus) sys::exit(bus.error());
            pci.emplace(std::move(*bus));
            if(const auto rc=pci->configure(0x10421af4);rc!=STATUS_OK) sys::exit(rc);
        }
        auto device = pci->find(0x10421af4, index);
        if (!device) sys::exit(device.error());
        auto& binding = devices.emplace_back(std::move(*device));
        auto& service = add(name, "block", {.bytes = 4 * 1024 * 1024, .caps = 128,
            .kinds = pr_base_kinds | OBJ_BIT(ObjIoSpace)});
        service.grants.push_back(sv_fixed(io_host, binding.cap.selector(),
                                          RIGHT_CONNECT, binding.rid, 1));
        share(service, 2 * 1024 * 1024);
        return service.publish(blk_service);
    };
    auto& files = add("files", "bootfs", {.bytes = 4 * 1024 * 1024, .caps = 1024,
        .kinds = pr_base_kinds | OBJ_BIT(ObjPager) | OBJ_BIT(ObjChannel),
        .slots = 1024, .pages = 132});
    files.grants = {{.contract=fs_archive,.bytes=fw_root(info).bundle_bytes,.cap=fw_root(info).bundle,.limit=word_t{RIGHT_MAP}}};
    share(files, 32 * 1024 * 1024);
    auto& directory = files.publish(fs_service);
    sv_export* storage{};
    if (archive.find("store")) {
        auto& disk = block("data", 0);
        auto& store = add("store", "store", {.bytes = 8 * 1024 * 1024, .caps = 256,
            .kinds = pr_base_kinds | OBJ_BIT(ObjChannel), .slots = 256, .pages = 36});
        share(store, 4 * 1024 * 1024);
        store.ready = true;
        store.use(disk, blk_service);
        storage = &store.publish(st_service);
    }
    auto& vfs = add("vfs", "vfs", {.bytes = 4 * 1024 * 1024, .caps = 256,
        .kinds = pr_base_kinds | OBJ_BIT(ObjChannel), .slots = 256, .pages = 36});
    vfs.ready = true;
    vfs.use(directory, fs_service);
    if (storage) vfs.use(*storage, st_service, 2);
    auto& mount = vfs.publish(vfs_service);
    auto& shell = add("shell", "shell", {.bytes = 32 * 1024 * 1024, .caps = 1024,
        .kinds = pr_base_kinds | OBJ_BIT(ObjChannel), .slots = 1024, .pages = 132,
        .pool_perms = RIGHT_CREATE | RIGHT_SPLIT});
    shell.grants = {sv_fixed(rt_domain, fw_root(info).domain, RIGHT_DUPLICATE | RIGHT_CONTROL),
        sv_cap(sv_service, control.ends[1].selector(), RIGHT_SEND | RIGHT_RECEIVE, 1)};
    shell.use(directory, fs_service, 3, RIGHT_SEND | RIGHT_DUPLICATE);
    shell.use(mount, vfs_service, 2, RIGHT_SEND | RIGHT_DUPLICATE);
    shell.use(mount, vfs_read, 1, RIGHT_SEND | RIGHT_DUPLICATE);
    shell.use(output, con_output, 2, RIGHT_SEND | RIGHT_DUPLICATE);
    shell.use(input, con_input, 1, RIGHT_RECEIVE | RIGHT_DUPLICATE, 1);
    if (storage) {
        shell.use(*storage, st_admin, 3, RIGHT_SEND | RIGHT_DUPLICATE);
        shell.args.push_back("storage");
    }

    uint64_t retry_at{};
    for (;;) {
        for (auto& service : services)
            if (service.task && !service.recovering) {
                const auto ended = service.task->observe();
                if (ended.status != STATUS_OK) sys::exit(ended.status);
                if (ended.value) recover(service);
            }
        // Initiate every affected drain before waiting: sibling capability
        // copies can carry quota charged to another retiring process.
        for (auto& service : services)
            if (service.recovering && service.task)
                if (const auto rc = service.task->stop(); rc != STATUS_OK) sys::exit(rc);
        bool pending = false;
        for (auto& service : services)
            if (service.recovering && service.task) {
                const auto collected = service.task->collect();
                if (collected.status == STATUS_OK) service.task.reset();
                else if (pr_retryable(collected.status) || collected.status == STATUS_WOULD_BLOCK)
                    pending = true;
                else sys::exit(collected.status);
            }
        if (!pending) {
            const auto now = sys::clock_now();
            if (now.status != STATUS_OK) sys::exit(now.status);
            if (!retry_at || now.value >= retry_at) {
                retry_at = 0;
                bool progress;
                do {
                    progress = false;
                    for (auto& service : services)
                        if (service.recovering && service.runnable()) {
                            const auto status = start(service, info, events.selector());
                            if (status != STATUS_OK) {
                                report(info, *firmware, status, service.name);
                                recover(service);
                                retry_at = rt_after_ms(100).value_or(0);
                                if (!retry_at) sys::exit(STATUS_INTERNAL);
                                break;
                            }
                            service.recovering = false;
                            progress = true;
                        }
                } while (progress && !retry_at);
                if (!retry_at && std::ranges::any_of(services, [](const auto& s) { return s.recovering; }))
                    sys::exit(STATUS_BAD_ARGS); // Invalid deployment cycle.
            }
        }
        for (;;) {
            ch_msg request;
            cap_owner reply;
            const auto received = channel.try_receive(request, reply);
            if (received.status == STATUS_WOULD_BLOCK) break;
            if (received.status == STATUS_BAD_ARGS || received.status == STATUS_TRANSFER_FAILED) continue;
            if (received.status != STATUS_OK) sys::exit(received.status);
            ch_reply response;
            auto& message = response.message;
            message.operation = request.operation;
            message.id = request.id;
            message.status = STATUS_NOT_FOUND;
            if (request.size > sizeof(request.data)) message.status = STATUS_BAD_ARGS;
            else if (request.operation == static_cast<uint64_t>(sv_op::List)) {
                if (request.size) message.status = STATUS_BAD_ARGS;
                else if (request.value < services.size()) {
                    const auto& service = services[request.value];
                    message.status = STATUS_OK;
                    message.value = (service.task ? sv_running : 0) |
                                    (service.recovering ? sv_recovering : 0);
                    message.size = service.name.size();
                    ::memcpy(message.data, service.name.data(), message.size);
                }
            } else if (request.operation == static_cast<uint64_t>(sv_op::Restart)) {
                for (auto& service : services)
                    if (service.name == std::string_view{request.data, request.size}) {
                        recover(service);
                        message.status = STATUS_OK;
                        break;
                    }
            } else message.status = STATUS_BAD_ARGS;
            response.respond(reply.selector());
        }
        if (const auto rc = channel.arm().status; rc != STATUS_OK) sys::exit(rc);
        auto deadline = retry_at;
        // Synchronous group close can be BUSY without a notification. Timer
        // polling is restricted to actual retirement, with no fixed badge map.
        if (pending) {
            const auto poll = rt_after_ms(1);
            if (!poll) sys::exit(STATUS_INTERNAL);
            if (!deadline || *poll < deadline) deadline = *poll;
        }
        if (std::ranges::any_of(services, [](const auto& s) { return s.recovering; }) && !pending && !retry_at)
            continue;
        const auto wake = sys::notification_wait(events.selector(), deadline);
        if (wake.status != STATUS_OK && wake.status != STATUS_TIMED_OUT) sys::exit(wake.status);
    }
}
