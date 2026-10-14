#include <libk/checked_arithmetic.hpp>
#include <protocols/msg.hpp>
#include <protocols/file.hpp>
#include <protocols/block.hpp>
#include <protocols/stream.hpp>
#include <protocols/service.hpp>
#include <servers/fw.hpp>
#include <servers/devices.hpp>
#include <charconv>
#include <exec.hpp>
#include <libk/cpio.hpp>
#include <servers/uart.hpp>
#include <algorithm>
#include <deque>
#include <ranges>
#include <string>
#include <memory>
#include <vector>

struct sv_svc;
struct sv_export {
    sv_svc* provider;
    rt_req contract;
    word_t depth{4}, transfers{4}, relations{2}, side{1}, rights{RIGHT_RECEIVE};
    ch_pair channel;
};
enum class sv_link { Restart, Reconnect };
struct sv_import {
    sv_export* source;
    rt_req contract;
    word_t side{}, badge{1}, rights{RIGHT_SEND};
    sv_link link{sv_link::Restart};
};
struct sv_svc {
    std::string name, image;
    sv_svc* owner{};
    struct Donation { sv_grant spec; cap_owner cap; };
    std::deque<Donation> donations;
    pr_quota quota;
    std::vector<std::string> args;
    std::vector<pr_cap> grants;
    std::deque<sv_export> exports;
    std::vector<sv_import> imports;
    bool await_ready{}, desired{true}, registry{}, admin{}, launcher{};
    sv_state state{sv_state::Starting};
    word_t badge{};
    std::unique_ptr<pr_task> task;
    cap_owner ready_event, memory_pool, lifecycle;
    auto publish(rt_req contract, word_t depth = 4, word_t transfers = 4, word_t side = 1,
                 word_t rights = RIGHT_RECEIVE) -> sv_export& {
        return exports.emplace_back(this, contract, depth, transfers, 2, side, rights);
    }
    void use(sv_export& source, rt_req contract, word_t badge = 1, word_t rights = RIGHT_SEND,
             word_t side = 0, sv_link link = sv_link::Restart) {
        imports.push_back({&source, contract, side, badge, rights, link});
    }
    auto runnable() const -> bool {
        return (!owner || (owner->task && (owner->state == sv_state::Running || owner->state == sv_state::Starting))) && std::ranges::all_of(imports, [](const auto& i) {
            return i.link == sv_link::Reconnect ||
                (i.source->provider && i.source->provider->state == sv_state::Running);
        });
    }
};
static std::deque<std::unique_ptr<sv_svc>> instances;
static auto services = instances | std::views::transform([](auto& p) -> sv_svc& { return *p; });
static word_t next_badge{};
static vm_area archive_mem;
static libk::cpio_view archive;

static void report(const rt_env& info, const fw_console& console, status_t status = STATUS_OK,
                   std::string_view name = {}) {
    if (!console.memory) return;
    vm_area region;
    auto status_map = region.open(info.vspace, 0, 4096);
    if (status_map == STATUS_OK)
        status_map = region.attach(console.memory.selector(), 0, 4096, VM_READ | VM_WRITE);
    if (status_map != STATUS_OK) sys::exit(status_map);
    uart_port port{region.address + console.offset};
    if (status == STATUS_OK) {
        port.reset();
        port.write("init: native services\n");
    } else {
        (void) libk::fmt::format_to<"init: service failed: {} task=">(port, status);
        port.write(name.data(), name.size());
        port.write("\n");
    }
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
static auto sv_fixed(rt_req contract, cap_t source, word_t rights, word_t first = 0,
                     word_t count = 0, word_t perms = 0) -> pr_cap {
    return {.contract = contract,
            .cap = source,
            .limit = CapView{.version = CAP_ATTENUATION_VERSION_CURRENT,
                             .kind = contract.kind,
                             .size = CAP_ATTENUATION_SIZE,
                             .rights = rights,
                             .words = {first, count, perms}}};
}
static auto start(sv_svc& service, const rt_env& info, cap_t events, cap_t registry) -> status_t {
    service.task = std::make_unique<pr_task>();
    auto caps = service.grants;
    for (const auto& grant : service.donations) {
        const auto& spec = grant.spec;
        caps.push_back({.contract={spec.name, spec.protocol, spec.kind, spec.major, spec.minor},
            .bytes=spec.bytes, .cap=grant.cap.selector(), .limit=word_t{spec.rights}});
    }
    if (service.registry)
        caps.push_back(sv_cap(sv_service, registry, RIGHT_SEND, 1, service.badge));
    for (auto& output : service.exports) {
        if (output.provider != &service) continue;
        const auto status =
            output.channel.open(info.pool, output.depth, output.transfers, output.relations);
        if (status != STATUS_OK)
            return status;
        caps.push_back(sv_cap(output.contract, output.channel.ends[output.side].selector(),
                              output.rights, output.side));
    }
    for (const auto& input : service.imports)
        if (input.link == sv_link::Restart)
            caps.push_back(sv_cap(input.contract, input.source->channel.ends[input.side].selector(),
                                  input.rights, input.side, input.badge));
    if (service.await_ready) {
        const auto created = sys::notification_create(info.pool, 1);
        if (created.status != STATUS_OK)
            return created.status;
        service.ready_event = cap_owner{created.value};
        caps.push_back(sv_fixed(sv_ready, created.value, RIGHT_SIGNAL));
        caps.push_back(sv_cap(sv_wake, events, RIGHT_SIGNAL, 0, 0));
    }
    const auto mod = archive.find(service.image);
    if (!mod)
        return STATUS_NOT_FOUND;
    std::vector<std::string_view> argv{service.args.begin(), service.args.end()};
    return pr_exec(*service.task,
                {info.pool, fw_root(info).domain, info.vspace, static_cast<uint32_t>(info.cpus)},
                pr_image{.bytes = mod->data},
                {.argv = argv,
                 .caps = caps,
                 .resources = service.quota,
                 .sched = {.budget = 600'000},
                 .events = events});
}
// Imports and declared component ownership form the deployment DAG.
static void recover(sv_svc& failed) {
    if (failed.state == sv_state::Draining)
        return;
    failed.state = sv_state::Draining;
    for (auto& service : services)
        if (service.owner == &failed || std::ranges::any_of(service.imports, [&](const auto& i) {
                return i.link == sv_link::Restart && i.source->provider == &failed;
            }))
            recover(service);
}
int main() {
    const auto& info = rt_startup();

    auto firmware = fw_view::open(info, 0);
    if (!firmware)
        sys::exit(firmware.error());
    auto irq = firmware->irq(info);
    if (!irq && irq.error() != STATUS_NOT_FOUND) sys::exit(irq.error());
    auto console = irq ? fw_console::open(info, firmware->tree, irq->selector())
                       : std::expected<fw_console, status_t>{std::unexpected(STATUS_NOT_FOUND)};
    if (!console && console.error() != STATUS_NOT_FOUND) sys::exit(console.error());
    if (!console) console = fw_console{};
    report(info, *console);
    cap_owner iommu;
    if (const auto node = firmware->tree.find("riscv,iommu"); node && irq) {
        const auto reg = firmware->tree.reg(*node);
        const auto interrupts = firmware->tree.property(*node, "interrupts");
        const auto plic = firmware->tree.find("sifive,plic-1.0.0");
        uint32_t source{};
        libk::byte_reader sources{interrupts ? interrupts->data() : nullptr, interrupts ? interrupts->size() : 0};
        if (!reg || !plic || !sources.read_be(source) ||
            firmware->tree.number(*node, "interrupt-parent") != firmware->tree.number(*plic, "phandle")) sys::exit(STATUS_BAD_ARGS);
        auto config = vm_area::create(info.pool, info.vspace, 0, 4096);
        if (!config) sys::exit(config.error());
        std::construct_at(reinterpret_cast<IommuConfig*>(config->address), IOMMU_RISCV, 0U,
                         reg->address, reg->size, irq->selector(), source, 24U);
        const auto ctl = sys::iommu_create(info.pool, fw_root(info).physical, config->memory.selector());
        if (ctl.status != STATUS_OK) sys::exit(ctl.status);
        iommu = cap_owner{ctl.value};
        for (;;) {
            const auto state = sys::iommu_state(iommu.selector());
            if (state.status != STATUS_OK || state.value == IO_SPACE_FAILED) sys::exit(STATUS_BACKING_FAILED);
            if (state.value == IO_SPACE_READY) break;
            sys::yield();
        }
    }
    const auto bytes = fw_root(info).bundle_bytes;
    if (const auto rc = archive_mem.open(info.vspace, 0, libk::checked_align_up(bytes, 4096).value_or(0), VM_READ);
        rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = archive_mem.attach(fw_root(info).bundle, 0, archive_mem.size, VM_READ);
        rc != STATUS_OK)
        sys::exit(rc);
    archive =
        libk::cpio_view::parse({reinterpret_cast<const std::byte*>(archive_mem.address), bytes});
    if (!archive)
        sys::exit(STATUS_BAD_ARGS);
    const auto notification = sys::notification_create(info.pool, 1);
    if (notification.status != STATUS_OK)
        sys::exit(notification.status);
    cap_owner events{notification.value};
    ch_pair control;
    if (const auto rc = control.open(info.pool, 4, 2, 1); rc != STATUS_OK)
        sys::exit(rc);
    const auto receiver =
        sys::channel_mint(control.ends[0].selector(), info.cspace, 1, RIGHT_RECEIVE | RIGHT_SEND);
    if (receiver.status != STATUS_OK)
        sys::exit(receiver.status);
    cap_owner control_rx{receiver.value};
    msg_rx channel{control_rx.selector(), events.selector()};

    // Explicit root deployment policy; no service names enter lifecycle code.
    const auto add = [](std::string_view name, std::string_view image, pr_quota quota) -> sv_svc& {
        if (!archive.find(image))
            sys::exit(STATUS_NOT_FOUND);
        auto& service = *instances.emplace_back(std::make_unique<sv_svc>());
        service.name = std::string{name}; service.image = std::string{image}; service.quota = quota;
        service.badge = ++next_badge;
        service.args.emplace_back(name);
        return service;
    };
    // Shared storage is a sibling of each execution, and survives its restart.
    const auto share = [&](sv_svc& service, word_t bytes) {
        const auto pool = sys::resource_create_child(info.pool, bytes, 0, OBJ_BIT(ObjMem));
        if (pool.status != STATUS_OK)
            sys::exit(pool.status);
        service.memory_pool = cap_owner{pool.value};
        service.grants.push_back(
            {.contract = io_memory, .cap = pool.value, .limit = word_t{RIGHT_CREATE | RIGHT_DUPLICATE}});
    };
    sv_export* output{}, *input{};
    if (console->memory) {
    auto& uart = add("uart", "uart", {.caps = 128, .kinds = pr_base_kinds | OBJ_BIT(ObjChannel)});
    auto memory = sv_cap(uart_mem, console->memory.selector(), RIGHT_MAP, 0, 0);
    std::get<CapView>(memory.limit).words[1] = 1;
    std::get<CapView>(memory.limit).words[2] = VM_READ | VM_WRITE;
    uart.grants = {memory, sv_cap(uart_irq, console->irq.selector(),
                                  RIGHT_ROUTE | RIGHT_OBSERVE | RIGHT_ACK, 0, 0)};
    char offset[24];
    const auto encoded = std::to_chars(offset, offset + sizeof(offset), console->offset);
    uart.args.emplace_back(offset, encoded.ptr);
    output = &uart.publish(con_output, 16, 0);
    input = &uart.publish(con_input, 16, 0, 0, RIGHT_SEND);

    }

    auto& files = add("files", "bootfs",
                      {.bytes = 4 * 1024 * 1024,
                       .caps = 1024,
                       .kinds = pr_base_kinds | OBJ_BIT(ObjPager) | OBJ_BIT(ObjChannel),
                       .slots = 1024,
                       .pages = 132});
    files.grants = {{.contract = fs_archive,
                     .bytes = fw_root(info).bundle_bytes,
                     .cap = fw_root(info).bundle,
                     .limit = word_t{RIGHT_MAP}}};
    // Immutable file copies follow deployment size; sessions have a bounded
    // independent working allowance. Debug information is not deployed.
    share(files, libk::checked_align_up(bytes, 4096).value_or(0) + 4 * 1024 * 1024);
    auto& directory = files.publish(fs_service);
    sv_export* storage{};
    auto device_boot = vm_area::create(info.pool, info.vspace, 0, 4096);
    if (!device_boot) sys::exit(device_boot.error());
    std::construct_at(reinterpret_cast<dv_boot*>(device_boot->address), fw_root(info).firmware_bytes,
                      fw_root(info).firmware_offset, fw_root(info).firmware_format);
    auto& devices = add("devices", "devices", {.bytes=8 * 1024 * 1024, .caps=256,
        .kinds=pr_base_kinds | OBJ_BIT(ObjIoSpace) | OBJ_BIT(ObjIrq) | OBJ_BIT(ObjChannel),
        .slots=256, .pages=36, .pool_perms=RIGHT_CREATE | RIGHT_SPLIT});
    devices.registry = devices.launcher = devices.await_ready = true;
    devices.grants = {
        {.contract=dv_firmware, .cap=fw_root(info).firmware, .limit=word_t{RIGHT_MAP | RIGHT_DUPLICATE}},
        sv_fixed(dv_config, device_boot->memory.selector(), RIGHT_MAP | RIGHT_DUPLICATE, 0, 1, VM_READ),
        {.contract=dv_physical, .cap=fw_root(info).physical, .limit=word_t{RIGHT_CREATE}},
        };
    if (irq) devices.grants.push_back({.contract=dv_interrupts, .cap=irq->selector(), .limit=word_t{RIGHT_CREATE}});
    if (iommu) devices.grants.push_back({.contract=dv_iommu, .cap=iommu.selector(), .limit=word_t{RIGHT_CREATE}});
    share(devices, 4 * 1024 * 1024);
    auto& disk = devices.publish(blk_service, 4, 4, 1, RIGHT_RECEIVE | RIGHT_DUPLICATE);
    disk.provider = nullptr; // The manager selects an actual driver process.
    if (iommu && archive.find("store")) {
        auto& store = add("store", "store", {.bytes=8 * 1024 * 1024, .caps=256,
            .kinds=pr_base_kinds | OBJ_BIT(ObjChannel), .slots=256, .pages=36});
        share(store, 4 * 1024 * 1024);
        store.await_ready = true;
        store.use(disk, blk_service);
        storage = &store.publish(st_service);
    }
    auto& vfs = add("vfs", "vfs",
                    {.bytes = 4 * 1024 * 1024,
                     .caps = 256,
                     .kinds = pr_base_kinds | OBJ_BIT(ObjChannel),
                     .slots = 256,
                     .pages = 36});
    vfs.registry = vfs.await_ready = true;
    vfs.use(directory, fs_service, fl_read | fl_exec);
    if (storage)
        vfs.use(*storage, st_service, fl_read | fl_write | fl_exec, RIGHT_SEND, 0,
                sv_link::Reconnect);
    auto& mount = vfs.publish(vfs_service);
    if (output && input) {
    auto& shell = add("shell", "shell",
                      {.bytes = 32 * 1024 * 1024,
                       .caps = 1024,
                       .kinds = pr_base_kinds | OBJ_BIT(ObjChannel),
                       .slots = 1024,
                       .pages = 132,
                       .pool_perms = RIGHT_CREATE | RIGHT_SPLIT});
    shell.registry = shell.admin = true;
    shell.grants = {sv_fixed(rt_domain, fw_root(info).domain, RIGHT_DUPLICATE | RIGHT_CONTROL)};
    shell.use(mount, vfs_service, fl_read | fl_write, RIGHT_SEND | RIGHT_DUPLICATE, 0,
              sv_link::Reconnect);
    shell.use(mount, vfs_read, fl_read, RIGHT_SEND | RIGHT_DUPLICATE, 0, sv_link::Reconnect);
    shell.use(mount, vfs_exec, fl_read | fl_exec, RIGHT_SEND | RIGHT_DUPLICATE, 0,
              sv_link::Reconnect);
    shell.use(*output, con_output, 2, RIGHT_SEND | RIGHT_DUPLICATE);
    shell.use(*input, con_input, 1, RIGHT_RECEIVE | RIGHT_DUPLICATE, 1);
    if (storage) {
        shell.use(*storage, st_admin, fl_read | fl_write | fl_admin, RIGHT_SEND | RIGHT_DUPLICATE,
                  0, sv_link::Reconnect);
        shell.args.push_back("storage");
    }

    }

    for (;;) {
        for (auto& service : services)
            if (service.task && (service.state == sv_state::Running || service.state == sv_state::Starting)) {
                const auto ended = service.task->observe();
                if (ended.status != STATUS_OK)
                    sys::exit(ended.status);
                if (ended.value) {
                    report(info, *console, ended.value2 ? static_cast<status_t>(ended.value2) : STATUS_PEER_CLOSED, service.name);
                    recover(service);
                }
                else if (service.state == sv_state::Starting && service.await_ready) {
                    const auto ready = sys::notification_take(service.ready_event.selector());
                    if (ready.status == STATUS_OK) {
                        service.state = sv_state::Running;
                        if (service.lifecycle) (void)sys::notification_signal(service.lifecycle.selector());
                    }
                    else if (ready.status != STATUS_RETRY) sys::exit(ready.status);
                }
            }
        // Begin every drain before collecting: sibling capabilities may carry
        // charges to another retiring execution. Reconnect consumers survive.
        bool pending = false;
        for (auto& service : services)
            if (service.state == sv_state::Draining || service.state == sv_state::Failed)
                {
                    if (service.owner) {
                        service.donations.clear();
                        if (service.owner->state == sv_state::Draining || !service.owner->task) service.lifecycle = {};
                    }
                    if (service.task)
                        if (const auto rc = service.task->stop(); rc != STATUS_OK) sys::exit(rc);
                }
        for (auto& service : services)
            if (service.state == sv_state::Draining || service.state == sv_state::Failed) {
                if (service.task) {
                    const auto collected = service.task->collect();
                    if (collected.status == STATUS_OK)
                        service.task.reset();
                    else if (pr_retryable(collected.status) ||
                             collected.status == STATUS_WOULD_BLOCK) {
                        pending = true;
                        continue;
                    } else
                        sys::exit(collected.status);
                }
                for (auto& owner : services)
                    for (auto& output : owner.exports)
                        if (&owner == &service || output.provider == &service)
                            if (const auto rc = output.channel.close(); rc != STATUS_OK) sys::exit(rc);
                service.ready_event = {};
                if (service.state == sv_state::Draining)
                    service.state = service.desired && !service.owner ? sv_state::Starting : sv_state::Stopped;
                if (service.lifecycle) (void)sys::notification_signal(service.lifecycle.selector());
            }
        for (auto& service : services)
            if (service.owner && service.state == sv_state::Stopped && !service.lifecycle)
                for (auto& output : service.owner->exports)
                    if (output.provider == &service) {
                        if (const auto rc = output.channel.close(); rc != STATUS_OK) sys::exit(rc);
                        output.provider = nullptr;
                    }
        std::erase_if(instances, [](const auto& p) {
            const auto& s = *p;
            return s.owner && s.state == sv_state::Stopped && !s.lifecycle;
        });
        if (!pending) {
            bool progress;
            do {
                progress = false;
                for (auto& service : services)
                    if (service.state == sv_state::Starting && !service.task && service.runnable()) {
                        const auto status =
                            start(service, info, events.selector(), control.ends[1].selector());
                        service.state = status == STATUS_OK ? (service.await_ready ? sv_state::Starting : sv_state::Running) : sv_state::Failed;
                        if (status != STATUS_OK)
                            report(info, *console, status, service.name);
                        progress = true;
                    }
            } while (progress);
        }
        for (;;) {
            msg_packet packet;
            const auto received = channel.receive(packet);
            if (received == STATUS_WOULD_BLOCK)
                break;
            if (received == STATUS_BAD_ARGS || received == STATUS_TRANSFER_FAILED)
                continue;
            if (received != STATUS_OK)
                sys::exit(received);
            if (packet.count < 1 || packet.count > 2)
                continue;
            const auto& request = packet.message;
            const auto caller = std::ranges::find(services, packet.badge, &sv_svc::badge);
            msg_reply response;
            auto& message = response.message;
            message.operation = request.operation;
            message.status = STATUS_NOT_FOUND;
            if (const auto status = rt_admit(request.deadline); status != STATUS_OK)
                message.status = status;
            else if (caller == services.end() || !(*caller).registry)
                message.status = STATUS_DENIED;
            else if (request.operation == static_cast<uint64_t>(sv_op::Lookup)) {
                if (request.value || request.size <= sizeof(sv_query))
                    message.status = STATUS_BAD_ARGS;
                else {
                    sv_query query;
                    ::memcpy(&query, request.data, sizeof(query));
                    const std::string_view name{request.data + sizeof(query),
                                                request.size - sizeof(query)};
                    for (const auto& input : (*caller).imports) {
                        const auto& contract = input.contract;
                        if (name != contract.name || query.protocol != contract.protocol ||
                            query.major != contract.major || query.minor > contract.minor)
                            continue;
                        if (!input.source->provider || input.source->provider->state != sv_state::Running) {
                            message.status = STATUS_WOULD_BLOCK;
                            break;
                        }
                        const auto endpoint = sys::channel_mint(
                            input.source->channel.ends[input.side].selector(), info.cspace,
                            input.badge, input.rights | RIGHT_DUPLICATE);
                        message.status = endpoint.status;
                        if (endpoint.status == STATUS_OK) {
                            message.status = response.offer(cap_owner{endpoint.value}, input.rights)
                                                 ? STATUS_OK
                                                 : STATUS_INTERNAL;
                            message.value = contract.kind;
                        }
                        break;
                    }
                }
            } else if (request.operation == static_cast<uint64_t>(sv_op::Define)) {
                if (!(*caller).launcher || packet.count != 2 || request.size < sizeof(sv_define)) message.status = STATUS_DENIED;
                else {
                    sv_define def;
                    ::memcpy(&def, request.data, sizeof(def));
                    if ((def.flags & ~1U) || !def.name_size || !def.image_size || sizeof(def) + def.name_size + def.image_size != request.size)
                        message.status = STATUS_BAD_ARGS;
                    else {
                        const std::string_view name{request.data + sizeof(def), def.name_size};
                        const std::string_view image{request.data + sizeof(def) + def.name_size, def.image_size};
                        if (name.find('\0') != name.npos || image.find('\0') != image.npos ||
                            std::ranges::find(services, name, &sv_svc::name) != services.end() || !archive.find(image)) message.status = STATUS_BAD_ARGS;
                        else {
                            auto& child = add(name, image, {.bytes=def.bytes, .caps=def.caps, .kinds=def.kinds, .slots=def.slots, .pages=def.pages});
                            child.owner = &*caller;
                            child.lifecycle = std::move(packet.capabilities[1]);
                            child.desired = false;
                            child.await_ready = def.flags & 1;
                            child.state = sv_state::Stopped;
                            message.status = STATUS_OK;
                            message.value = child.badge;
                        }
                    }
                }
            } else if (request.operation == static_cast<uint64_t>(sv_op::Grant)) {
                auto child = std::ranges::find(services, request.value, &sv_svc::badge);
                if (!(*caller).launcher || child == services.end() || (*child).owner != &*caller) message.status = STATUS_DENIED;
                else if (packet.count != 2 || request.size != sizeof(sv_grant) || (*child).state != sv_state::Stopped || (*child).task) message.status = STATUS_BAD_ARGS;
                else {
                    sv_grant grant;
                    ::memcpy(&grant, request.data, sizeof(grant));
                    const auto* end = static_cast<const char*>(::memchr(grant.name, 0, sizeof(grant.name)));
                    if (!end || end == grant.name || grant.reserved) message.status = STATUS_BAD_ARGS;
                    else {
                        (*child).donations.push_back({grant, std::move(packet.capabilities[1])});
                        message.status = STATUS_OK;
                    }
                }
            } else if (request.operation == static_cast<uint64_t>(sv_op::Provide)) {
                const auto child = std::ranges::find(services, request.value, &sv_svc::badge);
                if (!(*caller).launcher || child == services.end() || (*child).owner != &*caller) message.status = STATUS_DENIED;
                else if ((*child).state != sv_state::Stopped || request.size <= sizeof(sv_query)) message.status = STATUS_BAD_ARGS;
                else {
                    sv_query query;
                    ::memcpy(&query, request.data, sizeof(query));
                    const std::string_view name{request.data + sizeof(query), request.size - sizeof(query)};
                    for (auto& output : (*caller).exports) {
                        const auto& contract = output.contract;
                        if (name != contract.name || query.protocol != contract.protocol || query.major != contract.major || query.minor > contract.minor) continue;
                        if (output.provider && output.provider != &*caller) { message.status = STATUS_BUSY; break; }
                        message.status = output.channel.open(info.pool, output.depth, output.transfers, output.relations);
                        if (message.status != STATUS_OK) break;
                        const auto endpoint = sys::channel_mint(output.channel.ends[output.side].selector(), info.cspace, 1, output.rights);
                        message.status = endpoint.status;
                        if (message.status == STATUS_OK)
                            message.status = response.offer(cap_owner{endpoint.value}, output.rights) ? STATUS_OK : STATUS_INTERNAL;
                        if (message.status == STATUS_OK) output.provider = &*child;
                        else (void)output.channel.close();
                        break;
                    }
                }
            } else if (request.operation == static_cast<uint64_t>(sv_op::Remove)) {
                auto child = std::ranges::find(services, request.value, &sv_svc::badge);
                if (!(*caller).launcher || child == services.end() || (*child).owner != &*caller) message.status = STATUS_DENIED;
                else if ((*child).task || (*child).state != sv_state::Stopped) message.status = STATUS_BUSY;
                else {
                    const auto* target = &*child;
                    for (auto& output : (*caller).exports) if (output.provider == target) {
                        if (const auto rc = output.channel.close(); rc != STATUS_OK) sys::exit(rc);
                        output.provider = nullptr;
                    }
                    std::erase_if(instances, [&](const auto& p) { return p.get() == target; });
                    message.status = STATUS_OK;
                }
            } else if (!(*caller).admin && !(*caller).launcher)
                message.status = STATUS_DENIED;
            else if (request.operation == static_cast<uint64_t>(sv_op::List)) {
                const auto found = request.size ? std::ranges::find(services, std::string_view{request.data, request.size}, &sv_svc::name)
                    : request.value < services.size() ? std::next(services.begin(), request.value) : services.end();
                if (found != services.end() && ((*caller).admin || (*found).owner == &*caller)) {
                    const auto& service = *found;
                    message.status = STATUS_OK;
                    message.value = static_cast<uint64_t>(service.state);
                    message.size = service.name.size();
                    ::memcpy(message.data, service.name.data(), message.size);
                    if (request.size) {
                        const sv_status status{service.desired};
                        message.size = sizeof(status);
                        ::memcpy(message.data, &status, sizeof(status));
                    }
                }
            } else if (request.operation == static_cast<uint64_t>(sv_op::Restart) ||
                       request.operation == static_cast<uint64_t>(sv_op::Stop) ||
                       request.operation == static_cast<uint64_t>(sv_op::Start)) {
                if (request.value || !request.size)
                    message.status = STATUS_BAD_ARGS;
                else
                    for (auto& service : services)
                        if (service.name == std::string_view{request.data, request.size} && ((*caller).admin || service.owner == &*caller)) {
                            service.desired =
                                request.operation != static_cast<uint64_t>(sv_op::Stop);
                            if (request.operation == static_cast<uint64_t>(sv_op::Start)) {
                                if ((service.state == sv_state::Stopped || service.state == sv_state::Failed) &&
                                    (!service.owner || !service.donations.empty())) service.state = sv_state::Starting;
                            } else recover(service);
                            if (service.lifecycle) (void)sys::notification_signal(service.lifecycle.selector());
                            message.status = STATUS_OK;
                            break;
                        }
            } else
                message.status = STATUS_BAD_ARGS;
            response.respond(packet.capabilities[0].selector());
        }
        if (const auto rc = channel.arm().status; rc != STATUS_OK)
            sys::exit(rc);
        // Group refunds can finish without an event; poll only active drains.
        const auto deadline = pending ? rt_after_ms(1).value_or(0) : 0;
        if (pending && !deadline)
            sys::exit(STATUS_INTERNAL);
        if (!pending && std::ranges::any_of(services, [](const auto& s) {
                return s.state == sv_state::Draining || (s.state == sv_state::Failed && s.task);
            }))
            continue;
        const auto wake = sys::notification_wait(events.selector(), deadline);
        if (wake.status != STATUS_OK && wake.status != STATUS_TIMED_OUT)
            sys::exit(wake.status);
    }
}
