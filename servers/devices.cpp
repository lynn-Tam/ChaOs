#include <protocols/msg.hpp>
#include <servers/devices.hpp>
#include <protocols/block.hpp>
#include <protocols/service.hpp>
#include <protocols/manage.hpp>
#include <libk/fdt.hpp>
#include <libk/pci.hpp>
#include <vector>
#include <charconv>
#include <task.hpp>
#include <string>

static void dv_fence() noexcept { asm volatile("fence iorw, iorw" ::: "memory"); }
using dv_pci = pci::cfg<dv_fence>;

// The firmware describes bus addresses; only Phys creates actual authority.
struct dv_bus {
    struct Window { uint64_t child, physical, size, next; uint32_t kind; };
    const libk::fdt& tree;
    uint32_t node;
    cap_owner ecam;
    vm_area config;
    std::vector<Window> windows;
    uint32_t first{}, last{}, mapped{UINT32_MAX};
    static auto open(const rt_env& env, const libk::fdt& tree, uint32_t node) -> std::expected<dv_bus, status_t> {
        const auto reg = tree.reg(node);
        const auto up = tree.parent(node);
        const auto ranges = tree.property(node, "ranges"), buses = tree.property(node, "bus-range");
        if (!reg || !up || !ranges || !buses || buses->size() != 8 || reg->size % (1 << 20)) return std::unexpected(STATUS_BAD_ARGS);
        dv_bus bus{tree, node, {}, {}, {}};
        libk::byte_reader ids{buses->data(), buses->size()};
        if (!ids.read_be(bus.first) || !ids.read_be(bus.last) || bus.first > bus.last || bus.last > 255 ||
            reg->size / (1 << 20) < bus.last - bus.first + 1) return std::unexpected(STATUS_BAD_ARGS);
        libk::byte_reader r{ranges->data(), ranges->size()};
        while (r.remaining()) {
            uint32_t flags;
            uint64_t child, physical, bytes;
            if (!r.read_be(flags) || !libk::fdt::read_cells(r, 2, child) ||
                !libk::fdt::read_cells(r, tree.cell_count(*up, "#address-cells", 2), physical) ||
                !libk::fdt::read_cells(r, 2, bytes)) return std::unexpected(STATUS_BAD_ARGS);
            const auto kind = (flags >> 24) & 3;
            if (kind != 2 && kind != 3) continue;
            const auto span = tree.translate(*up, {physical, bytes});
            if (!span || child > UINT64_MAX - bytes) return std::unexpected(STATUS_BAD_ARGS);
            bus.windows.push_back({child, span->address, bytes, child, kind});
        }
        const auto memory = sys::memory_create_physical(env.pool, env.selector(dv_physical), reg->address, reg->size, VM_READ | VM_WRITE);
        if (memory.status != STATUS_OK) return std::unexpected(memory.status);
        bus.ecam = cap_owner{memory.value};
        const auto status = bus.config.open(env.vspace, 0, 1 << 20);
        if (status != STATUS_OK) return std::unexpected(status);
        return bus;
    }
    auto function(uint16_t rid) -> std::expected<dv_pci, status_t> {
        const auto bus = uint32_t{rid} >> 8;
        if (bus < first || bus > last) return std::unexpected(STATUS_BAD_ARGS);
        if (mapped != bus) {
            auto status = config.detach();
            if (status == STATUS_OK) status = config.attach(ecam.selector(), (bus - first) * 256, 1 << 20, VM_READ | VM_WRITE);
            if (status != STATUS_OK) return std::unexpected(status);
            mapped = bus;
        }
        return dv_pci{config.address + (rid & 255) * 4096};
    }
    auto physical(uint64_t address, uint64_t bytes) const -> std::optional<uint64_t> {
        for (const auto& w : windows)
            if (address >= w.child && address - w.child <= w.size && bytes <= w.size - (address - w.child))
                return w.physical + address - w.child;
        return {};
    }
    auto requester(uint16_t rid) const -> std::optional<uint32_t> {
        const auto map = tree.property(node, "iommu-map");
        if (!map || map->size() % 16) return {};
        const auto masked = rid & tree.number(node, "iommu-map-mask").value_or(UINT32_MAX);
        libk::byte_reader r{map->data(), map->size()};
        while (r.remaining()) {
            uint32_t first, ref, target, count;
            if (!r.read_be(first) || !r.read_be(ref) || !r.read_be(target) || !r.read_be(count)) return {};
            if (masked < first || masked - first >= count) continue;
            const auto iommu = tree.phandle(ref);
            if (!iommu || !tree.matches(*iommu, "riscv,iommu") || target > 0xffffff - (masked - first)) return {};
            return target + masked - first;
        }
        return {};
    }
    auto interrupt(uint16_t rid, uint8_t pin) const -> std::optional<uint32_t> {
        const auto map = tree.property(node, "interrupt-map"), mask = tree.property(node, "interrupt-map-mask");
        if (!map || !mask || mask->size() != 16) return {};
        std::array<uint32_t, 4> bits;
        libk::byte_reader mr{mask->data(), mask->size()}, r{map->data(), map->size()};
        for (auto& bit : bits) if (!mr.read_be(bit)) return {};
        const uint32_t key[]{uint32_t{rid} << 8, 0, 0, pin};
        while (r.remaining()) {
            bool match = true;
            for (size_t n = 0; n < 4; ++n) { uint32_t value; if (!r.read_be(value)) return {}; match &= (value & bits[n]) == (key[n] & bits[n]); }
            uint32_t ref, source;
            if (!r.read_be(ref)) return {};
            const auto intc = tree.phandle(ref);
            if (!intc || !tree.matches(*intc, "sifive,plic-1.0.0") ||
                tree.number(*intc, "#address-cells") != 0 || tree.number(*intc, "#interrupt-cells") != 1 || !r.read_be(source)) return {};
            if (match) return source;
        }
        return {};
    }
};

struct dv_device {
    uint16_t rid;
    uint32_t did, source;
    std::string name;
    cap_owner group, space, irq;
    std::array<vm_area, 6> bars;
    uint64_t instance{}, incarnation{};
    std::array<uint64_t, 6> addresses{};
    auto reset(dv_pci cfg) -> status_t {
        cfg.write<uint16_t>(4, 2 | (1 << 10)); // disable bus mastering and INTx
        std::array<std::pair<uintptr_t, size_t>, 6> views;
        for (size_t n = 0; n < 6; ++n) views[n] = {bars[n].address, bars[n].size};
        const auto transport = cfg.virtio(views);
        if (!transport) return STATUS_INVALID_OP;
        const dv_pci common{transport->regs[0].first};
        (void)common.read<uint8_t>(20); // drain prior posted MMIO writes before reset
        common.write<uint8_t>(20, 0);
        const auto deadline = rt_after_ms(1000);
        if (!deadline) return STATUS_INTERNAL;
        while (common.read<uint8_t>(20)) {
            if (rt_admit(*deadline) != STATUS_OK) return STATUS_TIMED_OUT;
            sys::yield();
        }
        // For this transport, acknowledged virtio reset cancels the old queues.
        // IOMMU fencing alone would not establish this device-side fact.
        return STATUS_OK;
    }
    auto prepare(const rt_env& env, dv_bus& bus) -> status_t {
        ++incarnation;
        const auto made = sys::resource_create_child(env.pool, 4 * 1024 * 1024, 128,
            OBJ_BIT(ObjMem) | OBJ_BIT(ObjIoSpace) | OBJ_BIT(ObjIrq));
        if (made.status != STATUS_OK) return made.status;
        group = cap_owner{made.value};
        auto fn = bus.function(rid);
        if (!fn) return fn.error();
        fn->write<uint16_t>(4, 1 << 10);
        const auto probed = fn->bars();
        if (!probed) return STATUS_INVALID_OP;
        for (size_t n = 0; n < 6; ++n) {
            const auto& bar = (*probed)[n];
            if (!bar.size) continue;
            const auto bytes = std::max<uint64_t>(4096, bar.size);
            uint64_t address = bar.pa;
            if (!address) {
                for (auto& w : bus.windows) {
                    if (!bar.wide && w.kind != 2) continue;
                    if (w.next > UINT64_MAX - bytes + 1) continue;
                    const auto aligned = (w.next + bytes - 1) & ~(bytes - 1);
                    if (aligned < w.child || aligned - w.child > w.size || bytes > w.size - (aligned - w.child) ||
                        (!bar.wide && aligned > UINT32_MAX)) continue;
                    address = aligned;
                    w.next = aligned + bytes;
                    break;
                }
            }
            const auto physical = bus.physical(address, bytes);
            if (!address || address % bytes || !physical || *physical % 4096) return STATUS_BAD_ARGS;
            const auto flags = fn->read<uint32_t>(0x10 + n * 4) & 15;
            fn->write<uint32_t>(0x10 + n * 4, static_cast<uint32_t>(address) | flags);
            if (bar.wide) fn->write<uint32_t>(0x14 + n * 4, address >> 32);
            addresses[n] = *physical;
            const auto mem = sys::memory_create_physical(group.selector(), env.selector(dv_physical), *physical, bytes, VM_READ | VM_WRITE);
            if (mem.status != STATUS_OK) return mem.status;
            auto mapping = vm_area::map(env.vspace, cap_owner{mem.value}, 0, bytes, VM_READ | VM_WRITE);
            if (!mapping) return mapping.error();
            bars[n] = std::move(*mapping);
        }
        const auto status = reset(*fn);
        if (status != STATUS_OK) return status;
        const auto interrupt = sys::irq_create(group.selector(), env.selector(dv_interrupts), source);
        if (interrupt.status != STATUS_OK) return interrupt.status;
        irq = cap_owner{interrupt.value};
        const auto domain = sys::io_space_create(group.selector(), env.selector(dv_iommu), did);
        if (domain.status != STATUS_OK) return domain.status;
        space = cap_owner{domain.value};
        for (;;) {
            const auto state = sys::io_space_state(space.selector());
            if (state.status != STATUS_OK) return state.status;
            if (state.value == IO_SPACE_READY) return STATUS_OK;
            if (state.value != IO_SPACE_PENDING) return STATUS_BACKING_FAILED;
            sys::yield();
        }
    }
    auto deploy(const rt_env& env, dv_bus& bus, rpc_client& root, cap_t changes) -> status_t {
        constexpr std::string_view image = "block";
        sv_define def{8 * 1024 * 1024, 256, pr_base_kinds | OBJ_BIT(ObjChannel), 256, 36,
                      static_cast<uint16_t>(name.size()), image.size(), 1};
        msg_frame request{.operation=static_cast<uint64_t>(sv_op::Define), .size=sizeof(def) + name.size() + image.size()};
        if (request.size > sizeof(request.data)) return STATUS_BAD_ARGS;
        ::memcpy(request.data, &def, sizeof(def));
        ::memcpy(request.data + sizeof(def), name.data(), name.size());
        ::memcpy(request.data + sizeof(def) + name.size(), image.data(), image.size());
        msg_packet reply;
        const CapXfer notice{changes, RIGHT_SEND, CAP_COPY, 0};
        auto status = root.exchange(request, reply, {&notice, 1});
        if (status != STATUS_OK) return status;
        instance = request.value;
        for (size_t n = 0; n < bars.size(); ++n) if (bars[n].memory) {
            status = sv_grant_cap(root, instance, dv_regs[n], bars[n].memory.selector(), RIGHT_MAP | RIGHT_DUPLICATE, bars[n].size);
            if (status != STATUS_OK) return status;
        }
        auto config = vm_area::create(env.pool, env.vspace, 0, 4096);
        if (!config) return config.error();
        const auto fn = bus.function(rid);
        if (!fn) return fn.error();
        // The driver consumes immutable transport metadata, never ECAM authority.
        for (size_t offset = 0; offset < 256; offset += 4)
            *reinterpret_cast<uint32_t*>(config->address + offset) = fn->read<uint32_t>(offset);
        status = config->detach();
        if (status == STATUS_OK) status = sys::memory_seal(config->memory.selector()).status;
        if (status == STATUS_OK) status = sv_grant_cap(root, instance, dv_regs[6], config->memory.selector(), RIGHT_MAP | RIGHT_DUPLICATE, 4096);
        if (status == STATUS_OK) status = sv_grant_cap(root, instance, dv_space, space.selector(), RIGHT_MAP | RIGHT_UNMAP | RIGHT_INSPECT | RIGHT_OBSERVE | RIGHT_CLOSE | RIGHT_DUPLICATE);
        if (status == STATUS_OK) status = sv_grant_cap(root, instance, dv_irq, irq.selector(), RIGHT_ROUTE | RIGHT_OBSERVE | RIGHT_ACK | RIGHT_DUPLICATE);
        if (status == STATUS_OK) status = sv_grant_cap(root, instance, io_memory, env.selector(io_memory), RIGHT_CREATE);
        if (status != STATUS_OK) return status;
        const std::string_view service{blk_service.name};
        request = {.operation=static_cast<uint64_t>(sv_op::Provide), .value=instance, .size=sizeof(sv_query) + service.size()};
        const sv_query contract{blk_service.protocol, blk_service.major, blk_service.minor};
        ::memcpy(request.data, &contract, sizeof(contract));
        ::memcpy(request.data + sizeof(contract), service.data(), service.size());
        if ((status = root.exchange(request, reply)) != STATUS_OK) return status;
        if (reply.count != 1) return STATUS_PEER_FAULT;
        if ((status = sv_grant_cap(root, instance, blk_service, reply.capabilities[0].selector(), RIGHT_RECEIVE)) != STATUS_OK) return status;
        request = {.operation=static_cast<uint64_t>(sv_op::Start), .size=name.size()};
        ::memcpy(request.data, name.data(), name.size());
        return root.exchange(request);
    }
    auto retire(dv_pci cfg) -> status_t {
        auto status = sys::io_space_close(space.selector()).status;
        if (status != STATUS_OK) return status;
        for (;;) {
            const auto state = sys::io_space_state(space.selector());
            if (state.status != STATUS_OK) return state.status;
            if (state.value == IO_SPACE_CLOSED) break;
            if (state.value == IO_SPACE_FAILED) return STATUS_BACKING_FAILED;
            sys::yield();
        }
        status = reset(cfg);
        if (status != STATUS_OK) return status;
        for (auto& bar : bars) if ((status = bar.close()) != STATUS_OK) return status;
        space = {}; irq = {};
        for (;;) {
            status = sys::resource_close(group.selector()).status;
            if (status == STATUS_OK) break;
            if (status != STATUS_BUSY && status != STATUS_RETRY) return status;
            sys::yield();
        }
        return group.close();
    }
};

int main() {
    const auto& env = rt_startup();
    const auto event = sys::notification_create(env.pool, 1);
    if (event.status != STATUS_OK) sys::exit(event.status);
    cap_owner replies{event.value};
    rpc_client root;
    // Reply readiness is local to RPC. It must not retrigger the lifecycle
    // loop's next query and manufacture an endless stream of reply channels.
    auto status = root.open(env.selector(sv_service), env.pool, env.cspace, replies.selector());
    if (status != STATUS_OK) sys::exit(status);
    auto boot = vm_area::map(env.vspace, cap_owner{sys::cap_duplicate(env.selector(dv_config), 0, RIGHT_MAP).value}, 0, 4096, VM_READ);
    if (!boot) sys::exit(boot.error());
    const auto config = *reinterpret_cast<const dv_boot*>(boot->address);
    if (config.firmware_format != FW_FDT || config.firmware_bytes > SIZE_MAX - 8190) sys::exit(STATUS_BAD_ARGS);
    auto firmware = vm_area::map(env.vspace, cap_owner{sys::cap_duplicate(env.selector(dv_firmware), 0, RIGHT_MAP).value},
        0, (config.firmware_offset + config.firmware_bytes + 4095) & ~size_t{4095}, VM_READ);
    if (!firmware) sys::exit(firmware.error());
    const auto tree = libk::fdt::open(std::span{reinterpret_cast<const uint8_t*>(firmware->address + config.firmware_offset), size_t{config.firmware_bytes}});
    if (!tree) sys::exit(STATUS_BAD_ARGS);
    const auto pci = tree->find("pci-host-ecam-generic");
    std::optional<dv_bus> bus;
    std::optional<dv_device> device;
    if (pci && env.selector(dv_iommu)) {
        auto opened = dv_bus::open(env, *tree, *pci);
        if (!opened) sys::exit(opened.error());
        bus.emplace(std::move(*opened));
        // bus-range bounds config access; only the root and configured bridges
        // establish reachable buses. Do not map every possible ECAM bus.
        std::vector<uint32_t> buses{bus->first};
        for (size_t at = 0; at < buses.size(); ++at) {
            const auto id = buses[at];
            for (uint32_t slot = 0; slot < 32; ++slot) {
                uint32_t functions = 1;
                for (uint32_t n = 0; n < functions; ++n) {
                    const auto rid = static_cast<uint16_t>((id << 8) | (slot << 3) | n);
                    const auto fn = bus->function(rid);
                    if (!fn) sys::exit(fn.error());
                    const auto identity = fn->read<uint32_t>(0);
                    if (identity == UINT32_MAX || !(identity & 0xffff)) continue;
                    if (!n && (fn->read<uint8_t>(14) & 0x80)) functions = 8;
                    fn->write<uint16_t>(4, (fn->read<uint16_t>(4) & ~uint16_t{4}) | (1 << 10));
                    if ((fn->read<uint32_t>(8) >> 16) == 0x0604) {
                        const auto secondary = fn->read<uint8_t>(0x19), subordinate = fn->read<uint8_t>(0x1a);
                        if (secondary && secondary >= bus->first && secondary <= subordinate && subordinate <= bus->last &&
                            std::ranges::find(buses, secondary) == buses.end()) buses.push_back(secondary);
                    }
                    if (identity != 0x1042'1af4 || device) continue;
                    const auto requester = bus->requester(rid), irq = bus->interrupt(rid, fn->read<uint8_t>(0x3d));
                    if (!requester || !irq) continue;
                    char name[24] = "block-";
                    const auto text = std::to_chars(name + 6, name + sizeof(name), rid, 16);
                    device.emplace(rid, *requester, *irq, std::string{name, text.ptr}, cap_owner{}, cap_owner{}, cap_owner{}, std::array<vm_area, 6>{}, 0, 0, std::array<uint64_t, 6>{});
                }
            }
        }
    }
    msg_rx management{env.selector(mg_devices), env.events};
    ch_pair lifecycle;
    if (const auto status = lifecycle.open(env.pool, 1, 0); status != STATUS_OK) sys::exit(status);
    const auto sender = sys::channel_mint(lifecycle.ends[1].selector(), env.cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE);
    if (sender.status != STATUS_OK) sys::exit(sender.status);
    cap_owner changes{sender.value};
    msg_rx life{lifecycle.ends[0].selector(), env.events};
    bool ready{};
    if (device) {
        status = device->prepare(env, *bus);
        if (status == STATUS_OK) status = device->deploy(env, *bus, root, changes.selector());
        if (status != STATUS_OK) sys::exit(status);
    }
    for (;;) {
        msg_frame change;
        while (life.try_receive(change).status == STATUS_OK) {}
        for (unsigned n = 0; n < 16; ++n) {
            msg_packet packet;
            const auto received = management.receive(packet);
            if (received == STATUS_WOULD_BLOCK) break;
            if (received != STATUS_OK || packet.count != 1) continue;
            msg_reply response;
            auto& message = response.message;
            const auto& request = packet.message;
            message.operation = request.operation;
            message.status = STATUS_NOT_FOUND;
            if (packet.badge != mg_control && packet.badge != mg_observe) message.status = STATUS_DENIED;
            else if (device && request.operation == static_cast<uint64_t>(mg_op::Devices) && request.value < uint64_t{device->rid} + 1) {
                mg_device row{uint64_t{device->rid} + 1, device->incarnation, device->instance, 0, 0,
                    device->did, device->source, static_cast<uint32_t>(mg_state::Stopped), device->rid, UINT64_MAX,
                    static_cast<uint32_t>(mg_observe | (packet.badge == mg_control ? mg_control | mg_stop | mg_start | mg_restart : 0)), {}};
                for (size_t i = 0; i < device->bars.size(); ++i) if (device->bars[i].memory) {
                    row.mmio = device->addresses[i]; row.bytes = device->bars[i].size; break;
                }
                if (device->space) {
                    const auto io = sys::io_space_state(device->space.selector());
                    if (io.status == STATUS_OK) row.iospace = io.value;
                }
                msg_frame query{.operation = static_cast<uint64_t>(sv_op::List), .size = device->name.size()};
                ::memcpy(query.data, device->name.data(), query.size);
                if (root.exchange(query) == STATUS_OK) {
                    row.state = static_cast<uint32_t>(query.value);
                    if (row.state == static_cast<uint32_t>(mg_state::Stopped) && device->group)
                        row.state = static_cast<uint32_t>(mg_state::Draining);
                    if (row.state == static_cast<uint32_t>(mg_state::Running)) {
                        const auto cfg = bus->function(device->rid);
                        if (!cfg || !(cfg->read<uint16_t>(4) & 4) || row.iospace != IO_SPACE_READY)
                            row.state = static_cast<uint32_t>(mg_state::Starting);
                    }
                }
                ::memcpy(row.name, device->name.data(), std::min(device->name.size(), sizeof(row.name) - 1));
                mg_pack(message, row); message.value = row.id;
                message.deadline = sys::clock_now().value;
            } else if (device && request.operation == static_cast<uint64_t>(mg_op::DeviceAction)) {
                if (packet.badge != mg_control) message.status = STATUS_DENIED;
                else if (request.size != sizeof(mg_target)) message.status = STATUS_BAD_ARGS;
                else {
                    mg_target expected; ::memcpy(&expected, request.data, sizeof(expected));
                    if (request.value != uint64_t{device->rid} + 1 || expected.incarnation != device->incarnation) message.status = STATUS_STALE;
                    else {
                        const auto action = static_cast<mg_action>(request.status);
                        const auto op = action == mg_action::Start ? sv_op::Start : action == mg_action::Stop ? sv_op::Stop : sv_op::Restart;
                        if (action != mg_action::Start && action != mg_action::Stop && action != mg_action::Restart) message.status = STATUS_BAD_ARGS;
                        else {
                            msg_frame change{.operation = static_cast<uint64_t>(op), .size = device->name.size()};
                            ::memcpy(change.data, device->name.data(), change.size);
                            message.status = root.exchange(change);
                        }
                    }
                }
            }
            response.respond(packet.capabilities[0].selector());
        }
        (void)management.arm();
        if (device) {
            msg_frame query{.operation=static_cast<uint64_t>(sv_op::List), .size=device->name.size()};
            ::memcpy(query.data, device->name.data(), device->name.size());
            if ((status = root.exchange(query)) != STATUS_OK || query.size != sizeof(sv_status)) sys::exit(status);
            sv_status observed;
            ::memcpy(&observed, query.data, sizeof(observed));
            if (query.value == static_cast<uint64_t>(sv_state::Failed)) sys::exit(STATUS_BACKING_FAILED);
            if (query.value == static_cast<uint64_t>(sv_state::Running)) {
                auto fn = bus->function(device->rid);
                if (!fn) sys::exit(fn.error());
                fn->write<uint16_t>(4, 6); // queue setup and IOSpace admission have completed
                (void)fn->read<uint16_t>(4);
                if (!ready) {
                    if ((status = sys::notification_signal(env.selector(sv_ready)).status) != STATUS_OK) sys::exit(status);
                    if ((status = sys::notification_signal(env.selector(sv_wake)).status) != STATUS_OK) sys::exit(status);
                    ready = true;
                }
            } else if (query.value == static_cast<uint64_t>(sv_state::Stopped)) {
                const auto fn = bus->function(device->rid);
                if (!fn) sys::exit(fn.error());
                if (device->group && (status = device->retire(*fn)) != STATUS_OK) sys::exit(status);
                if (!observed.desired) {
                    (void)management.arm(); (void)life.arm();
                    if ((status = sys::notification_wait(env.events).status) != STATUS_OK) sys::exit(status);
                    continue;
                }
                msg_frame remove{.operation=static_cast<uint64_t>(sv_op::Remove), .value=device->instance};
                if ((status = root.exchange(remove)) != STATUS_OK) sys::exit(status);
                if ((status = device->prepare(env, *bus)) != STATUS_OK) sys::exit(status);
                if ((status = device->deploy(env, *bus, root, changes.selector())) != STATUS_OK) sys::exit(status);
                continue;
            }
        } else if (!ready) {
            if ((status = sys::notification_signal(env.selector(sv_ready)).status) != STATUS_OK) sys::exit(status);
            if ((status = sys::notification_signal(env.selector(sv_wake)).status) != STATUS_OK) sys::exit(status);
            ready = true;
        }
        (void)management.arm(); (void)life.arm();
        if ((status = sys::notification_wait(env.events).status) != STATUS_OK) sys::exit(status);
    }
}
